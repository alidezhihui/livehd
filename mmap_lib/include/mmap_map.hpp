//  This file is distributed under the BSD 3-Clause License. See LICENSE for details.
//
// Code based on robin-hood hash map. Changed to use only flat_map (not
// node_map), small changes in hashing function (zero hash use as end), and
// significant changes to support mmap storage, strings with mmap at index...

//                 ______  _____                 ______                _________
//  ______________ ___  /_ ___(_)_______         ___  /_ ______ ______ ______  /
//  __  ___/_  __ \__  __ \__  / __  __ \        __  __ \_  __ \_  __ \_  __  /
//  _  /    / /_/ /_  /_/ /_  /  _  / / /        _  / / // /_/ // /_/ // /_/ /
//  /_/     \____/ /_.___/ /_/   /_/ /_/ ________/_/ /_/ \____/ \____/ \__,_/
//                                      _/_____/
//
// robin_hood::unordered_map for C++14
// version 3.2.7
// https://github.com/martinus/robin-hood-hashing
//
// Licensed under the MIT License <http://opensource.org/licenses/MIT>.
// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2019 Martin Ankerl <http://martin.ankerl.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "mmap_gc.hpp"
#include "mmap_hash.hpp"
#include "mmap_str.hpp"

#define FAST_MISS_ALIGNED_CPU 1

//#define mmap_map_LOG_ENABLED
#ifdef mmap_map_LOG_ENABLED
#define mmap_map_LOG(x) std::cout << __FUNCTION__ << "@" << __LINE__ << ": " << x << std::endl
#else
#define mmap_map_LOG(x)
#endif

// mark unused members with this macro
#define mmap_map_UNUSED(identifier)

// bitness
#ifdef FORCE_ROBIN_32
#define mmap_map_BITNESS 32
#else
#if SIZE_MAX == UINT32_MAX
#define mmap_map_BITNESS 32
#elif SIZE_MAX == UINT64_MAX
#define mmap_map_BITNESS 64
#else
#error Unsupported bitness
#endif
#endif

// endianess
#ifdef _WIN32
#define mmap_map_LITTLE_ENDIAN 1
#define mmap_map_BIG_ENDIAN    0
#else
#if __GNUC__ >= 4
#define mmap_map_LITTLE_ENDIAN (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define mmap_map_BIG_ENDIAN    (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#else
#error cannot determine endianness
#endif
#endif

// inline
#ifdef _WIN32
#define mmap_map_NOINLINE __declspec(noinline)
#else
#if __GNUC__ >= 4
#define mmap_map_NOINLINE __attribute__((noinline))
#else
#define mmap_map_NOINLINE
#endif
#endif

// count leading/trailing bits
#ifdef _WIN32
#if mmap_map_BITNESS == 32
#define mmap_map_BITSCANFORWARD _BitScanForward
#else
#define mmap_map_BITSCANFORWARD _BitScanForward64
#endif
#include <intrin.h>
#pragma intrinsic(mmap_map_BITSCANFORWARD)
#define mmap_map_COUNT_TRAILING_ZEROES(x)                                    \
  [](size_t mask) -> int {                                                   \
    unsigned long index;                                                     \
    return mmap_map_BITSCANFORWARD(&index, mask) ? index : mmap_map_BITNESS; \
  }(x)
#else
#if __GNUC__ >= 4
#if mmap_map_BITNESS == 32
#define mmap_map_CTZ(x) __builtin_ctzl(x)
#define mmap_map_CLZ(x) __builtin_clzl(x)
#else
#define mmap_map_CTZ(x) __builtin_ctzll(x)
#define mmap_map_CLZ(x) __builtin_clzll(x)
#endif
#define mmap_map_COUNT_LEADING_ZEROES(x)  (x ? mmap_map_CLZ(x) : mmap_map_BITNESS)
#define mmap_map_COUNT_TRAILING_ZEROES(x) (x ? mmap_map_CTZ(x) : mmap_map_BITNESS)
#else
#error clz not supported
#endif
#endif

// umul
namespace mmap_lib {

template <typename T>
struct is_array_serializable {
  template <typename U>
  static constexpr decltype(std::declval<U>().data(), bool()) test_data(int) {
    return true;
  }

  template <typename U>
  static constexpr bool test_data(...) {
    return false;
  }

  template <typename U>
  static constexpr decltype(std::declval<U>().size(), bool()) test_size(int) {
    return true;
  }

  template <typename U>
  static constexpr bool test_size(...) {
    return false;
  }

  // std::string can work too but it can easily do copies all over. Not worth it for performance reasons
  static constexpr bool value = test_data<T>(int()) && test_size<T>(int()) && !std::is_same<T, std::string>::value;
};

namespace detail {
#if defined(__SIZEOF_INT128__)
#define mmap_map_UMULH(a, b) static_cast<uint64_t>((static_cast<unsigned __int128>(a) * static_cast<unsigned __int128>(b)) >> 64u)

#define mmap_map_HAS_UMUL128 1
inline uint64_t umul128(uint64_t a, uint64_t b, uint64_t* high) {
  auto result = static_cast<unsigned __int128>(a) * static_cast<unsigned __int128>(b);
  *high       = static_cast<uint64_t>(result >> 64);
  return static_cast<uint64_t>(result);
}
#elif (defined(_WIN32) && mmap_map_BITNESS == 64)
#define mmap_map_HAS_UMUL128 1
#include <intrin.h>  // for __umulh
#pragma intrinsic(__umulh)
#pragma intrinsic(_umul128)
#define mmap_map_UMULH(a, b) __umulh(a, b)
inline uint64_t umul128(uint64_t a, uint64_t b, uint64_t* high) { return _umul128(a, b, high); }
#endif

// make sure this is not inlined as it is slow and dramatically enlarges code, thus making other
// inlinings more difficult. Throws are also generally the slow path.
template <typename E, typename... Args>
static mmap_map_NOINLINE void doThrow(Args&&... args) {
  throw E(std::forward<Args>(args)...);
}

template <typename E, typename T, typename... Args>
static T* assertNotNull(T* t, Args&&... args) {
  if (MMAP_LIB_UNLIKELY(nullptr == t)) {
    doThrow<E>(std::forward<Args>(args)...);
  }
  return t;
}

template <typename T>
inline T unaligned_load(void const* ptr) {
  // using memcpy so we don't get into unaligned load problems.
  // compiler should optimize this very well anyways.
  T t;
  std::memcpy(&t, ptr, sizeof(T));
  return t;
}

// All empty maps initial mInfo point to this infobyte. That way lookup in an empty map
// always returns false, and this is a very hot byte.
//
// we have to use data >1byte (at least 2 bytes), because initially we set mShift to 63 (has to be
// <63), so initial index will be 0 or 1.

}  // namespace detail

struct is_transparent_tag {};

// A custom pair implementation is used in the map because std::pair is not is_trivially_copyable,
// which means it would  not be allowed to be used in std::memcpy. This struct is copyable, which is
// also tested.
template <typename First, typename Second>
struct pair {
  using first_type  = First;
  using second_type = Second;

  // pair constructors are explicit so we don't accidentally call this ctor when we don't have to.
  explicit pair(std::pair<First, Second> const& o) : first{o.first}, second{o.second} {}

  // pair constructors are explicit so we don't accidentally call this ctor when we don't have to.
  explicit pair(std::pair<First, Second>&& o) : first{std::move(o.first)}, second{std::move(o.second)} {}

  constexpr pair(const First& firstArg, const Second& secondArg) : first{firstArg}, second{secondArg} {}

  constexpr pair(First&& firstArg, Second&& secondArg) : first{std::move(firstArg)}, second{std::move(secondArg)} {}

  template <typename FirstArg, typename SecondArg>
  constexpr pair(FirstArg&& firstArg, SecondArg&& secondArg)
      : first{std::forward<FirstArg>(firstArg)}, second{std::forward<SecondArg>(secondArg)} {}

  template <typename... Args1, typename... Args2>
  pair(std::piecewise_construct_t /*unused*/, std::tuple<Args1...> firstArgs, std::tuple<Args2...> secondArgs)
      : pair{firstArgs, secondArgs, std::index_sequence_for<Args1...>{}, std::index_sequence_for<Args2...>{}} {}

  // constructor called from the std::piecewise_construct_t ctor
  template <typename... Args1, size_t... Indexes1, typename... Args2, size_t... Indexes2>
  inline pair(std::tuple<Args1...>& tuple1, std::tuple<Args2...>& tuple2, std::index_sequence<Indexes1...> /*unused*/,
              std::index_sequence<Indexes2...> /*unused*/)
      : first{std::forward<Args1>(std::get<Indexes1>(tuple1))...}, second{std::forward<Args2>(std::get<Indexes2>(tuple2))...} {
    // make visual studio compiler happy about warning about unused tuple1 & tuple2.
    // Visual studio's pair implementation disables warning 4100.
    (void)tuple1;
    (void)tuple2;
  }

  first_type&        getFirst() { return first; }
  first_type const&  getFirst() const { return first; }
  second_type&       getSecond() { return second; }
  second_type const& getSecond() const { return second; }

  void swap(pair<First, Second>& o) {
    using std::swap;
    swap(first, o.first);
    swap(second, o.second);
  }

  First  first;
  Second second;
};

// A thin wrapper around std::hash, performing a single multiplication to (hopefully) get nicely
// randomized upper bits, which are used by the mmap_lib.
template <typename T>
struct hash : public std::hash<T> {
  constexpr size_t operator()(T const& obj) const { return std::hash<T>::operator()(obj); }
};

// specialization used for uint64_t and int64_t. Uses 128bit multiplication
template <>
struct hash<uint64_t> {
  constexpr size_t operator()(uint64_t const& obj) const {
    // murmurhash 3 finalizer
    uint64_t h = obj;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccd;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53;
    h ^= h >> 33;
    return static_cast<size_t>(h);
  }
};


template <>
struct hash<mmap_lib::str> {
  constexpr size_t operator()(mmap_lib::str const &o) const {
    return o.hash();
  }
};

template <>
struct hash<int64_t> {
  constexpr size_t operator()(int64_t const& obj) const { return hash<uint64_t>{}(static_cast<uint64_t>(obj)); }
};

template <>
struct hash<uint32_t> {
  constexpr size_t operator()(uint32_t const& h) const {
    return static_cast<size_t>((UINT64_C(0xca4bcaa75ec3f625) * (uint64_t)h) >> 32);
  }
};

template <>
struct hash<int32_t> {
  constexpr size_t operator()(int32_t const& obj) const { return hash<uint32_t>{}(static_cast<uint32_t>(obj)); }
};

// Hash an arbitrary amount of bytes. This is basically Murmur2 hash without caring about big
// endianness. TODO add a fallback for very large strings?
inline size_t hash_bytes(void const* ptr, size_t const len) {
  static constexpr uint64_t     m    = UINT64_C(0xc6a4a7935bd1e995);
  static constexpr uint64_t     seed = UINT64_C(0xe17a1465);
  static constexpr unsigned int r    = 47;

  auto const data64 = reinterpret_cast<uint64_t const*>(ptr);
  uint64_t   h      = seed ^ (len * m);

  size_t const n_blocks = len / 8;
  for (size_t i = 0; i < n_blocks; ++i) {
    uint64_t k = detail::unaligned_load<uint64_t>(data64 + i);

    k *= m;
    k ^= k >> r;
    k *= m;

    h ^= k;
    h *= m;
  }

  auto const data8 = reinterpret_cast<uint8_t const*>(data64 + n_blocks);
  switch (len & 7u) {
    case 7:
      h ^= static_cast<uint64_t>(data8[6]) << 48u;
      // fallthrough
    case 6:
      h ^= static_cast<uint64_t>(data8[5]) << 40u;
      // fallthrough
    case 5:
      h ^= static_cast<uint64_t>(data8[4]) << 32u;
      // fallthrough
    case 4:
      h ^= static_cast<uint64_t>(data8[3]) << 24u;
      // fallthrough
    case 3:
      h ^= static_cast<uint64_t>(data8[2]) << 16u;
      // fallthrough
    case 2:
      h ^= static_cast<uint64_t>(data8[1]) << 8u;
      // fallthrough
    case 1: h ^= static_cast<uint64_t>(data8[0]); h *= m;
  };

  h ^= h >> r;
  h *= m;
  h ^= h >> r;

  return static_cast<size_t>(h);
}

template <>
struct hash<std::string_view> {
  size_t operator()(std::string_view str) const noexcept { return mmap_lib::hash64(str.data(), str.size()); }
};

template <class T>
struct hash<std::vector<T>> {
  size_t operator()(const std::vector<T>& str) const noexcept { return mmap_lib::hash64(str.data(), sizeof(T) * str.size()); }
};

namespace detail {

// A highly optimized hashmap implementation, using the Robin Hood algorithm.
//
// In most cases, this map should be usable as a drop-in replacement for std::map, but be
// about 2x faster in most cases and require much less allocations.
//
// This implementation uses the following memory layout:
//
// [Node, Node, ... Node | info, info, ... infoSentinel ]
//
// * Node: either a DataNode that directly has the std::pair<key, val> as member,
//   or a DataNode with a pointer to std::pair<key,val>. Which DataNode representation to use
//   depends on how fast the swap() operation is. Heuristically, this is automatically choosen based
//   on sizeof(). there are always 2^n Nodes.
//
// * info: Each Node in the map has a corresponding info byte, so there are 2^n info bytes.
//   Each byte is initialized to 0, meaning the corresponding Node is empty. Set to 1 means the
//   corresponding node contains data. Set to 2 means the corresponding Node is filled, but it
//   actually belongs to the previous position and was pushed out because that place is already
//   taken.
//
// * infoSentinel: Sentinel byte set to 1, so that iterator's ++ can stop at end() without the need
// for a idx
//   variable.
//
// According to STL, order of templates has effect on throughput. That's why I've moved the boolean
// to the front.
// https://www.reddit.com/r/cpp/comments/ahp6iu/compile_time_binary_size_reductions_and_cs_future/eeguck4/
template <size_t MaxLoadFactor100, typename Key, typename T, typename Hash>
class map : public Hash {
private:
  // static_assert(std::is_trivially_destructible<Key>::value, "Objects in map should be simple without pointer (simple
  // destruction)");
  static_assert(!std::is_same<Key, std::string>::value, "mmap_lib::map uses mmap_lib::str as key (not std::string)\n");
  static_assert(!std::is_same<T, std::string>::value, "mmap_lib::map uses mmap_lib::str as value (not std::string)\n");
  static_assert(!std::is_same<Key, std::string_view>::value, "mmap_lib::map uses mmap_lib::str as key (not std::string_view)\n");
  static_assert(!std::is_same<T, std::string_view>::value, "mmap_lib::map uses mmap_lib::str as value (not std::string_view)\n");
  static_assert(!is_array_serializable<Key>::value, "mmap_lib::map can not have an array for key (first)\n");
  static_assert(!is_array_serializable<T>::value, "mmap_lib::map can not have an array for contents (second)\n");

  static_assert(MaxLoadFactor100 > 10 && MaxLoadFactor100 < 100, "MaxLoadFactor100 needs to be >10 && < 100");

  static constexpr size_t  InitialNumElements   = 1024;
  static constexpr int     InitialInfoNumBits   = 5;
  static constexpr uint8_t InitialInfoInc       = 1 << InitialInfoNumBits;
  static constexpr uint8_t InitialInfoHashShift = sizeof(int) * 8 - InitialInfoNumBits;

  // type needs to be wider than uint8_t.
  using InfoType = int32_t;

public:
  using key_type   = Key;
  using value_type = mmap_lib::pair<Key, T>;
  using size_type  = size_t;
  using hasher     = Hash;
  using Self       = map<MaxLoadFactor100, key_type, T, hasher>;

private:
  // DataNode ////////////////////////////////////////////////////////

  // Primary template for the data node. We have special implementations for small and big
  // objects. For large objects it is assumed that swap() is fairly slow, so we allocate these on
  // the heap so swap merely swaps a pointer.

  // Small: just allocate on the stack.
  template <typename M>
  class DataNode {
  public:
    template <typename... Args>
    explicit DataNode(M& mmap_map_UNUSED(map) /*unused*/, Args&&... args) : mData(std::forward<Args>(args)...) {}

    DataNode(M& mmap_map_UNUSED(map) /*unused*/, DataNode<M>&& n) : mData(std::move(n.mData)) {}

    // doesn't do anything
    void destroy(M& mmap_map_UNUSED(map) /*unused*/) {}
    void destroyDoNotDeallocate() {}

    value_type const* operator->() const noexcept { return &mData; }
    value_type*       operator->() noexcept { return &mData; }

    const value_type& operator*() const noexcept { return mData; }

    value_type& operator*() noexcept { return mData; }

    typename value_type::first_type& getFirst() noexcept { return mData.first; }

    typename value_type::first_type const& getFirst() const noexcept { return mData.first; }

    typename value_type::second_type& getSecond() noexcept { return mData.second; }

    typename value_type::second_type const& getSecond() const noexcept { return mData.second; }

    void swap(DataNode<M>& o) noexcept { mData.swap(o.mData); }

  private:
    value_type mData;
  };

  using Node = DataNode<Self>;

  // Iter ////////////////////////////////////////////////////////////

  struct fast_forward_tag {};

  // generic iterator for both const_iterator and iterator.
  template <bool IsConst>
  class Iter {
  private:
    using NodePtr = typename std::conditional<IsConst, Node const*, Node*>::type;

  public:
    using difference_type   = std::ptrdiff_t;
    using value_type        = typename Self::value_type;
    using reference         = typename std::conditional<IsConst, value_type const&, value_type&>::type;
    using pointer           = typename std::conditional<IsConst, value_type const*, value_type*>::type;
    using iterator_category = std::forward_iterator_tag;

    // default constructed iterator can be compared to itself, but WON'T return true when
    // compared to end().
    explicit constexpr Iter() : mKeyVals(nullptr), mInfo(nullptr), map_ptr(nullptr) {}

    explicit Iter(const Iter &it) : mKeyVals(it.mKeyVals), mInfo(it.mInfo), map_ptr(it.map_ptr) {
      if (map_ptr) {
        map_ptr->ref_lock();
      }
    }

    explicit Iter(const map* _map_ptr, NodePtr valPtr, uint8_t const* infoPtr) : mKeyVals(valPtr), mInfo(infoPtr), map_ptr(_map_ptr) {
    }

    explicit Iter(const map* _map_ptr, NodePtr valPtr, uint8_t const* infoPtr, fast_forward_tag mmap_map_UNUSED(tag) /*unused*/)
        : mKeyVals(valPtr), mInfo(infoPtr), map_ptr(_map_ptr) {
      fastForward();
    }

    ~Iter() {
      if (map_ptr) {
        map_ptr->ref_unlock();
      }
    }

    void operator=(const Iter& other) {
      mKeyVals = other.mKeyVals;
      mInfo    = other.mInfo;
      map_ptr  = other.map_ptr;
      if (map_ptr) {
        map_ptr->ref_lock();
      }
    }

    // prefix increment. Undefined behavior if we are at end()!
    Iter& operator++() {
      assert(map_ptr);             // true iter should have ptr to avoid gc
      assert(map_ptr->mmap_base);  // no gc
      mInfo++;
      mKeyVals++;
      fastForward();
      return *this;
    }

    reference operator*() const { return **mKeyVals; }

    pointer operator->() const { return &**mKeyVals; }

    template <bool O>
    bool operator==(Iter<O> const& o) const {
      return mKeyVals == o.mKeyVals;
    }

    template <bool O>
    bool operator!=(Iter<O> const& o) const {
      return mKeyVals != o.mKeyVals;
    }

  private:
    // fast forward to the next non-free info byte
    void fastForward() {
#ifdef FAST_MISS_ALIGNED_CPU
      int inc;
      do {
        auto const n = detail::unaligned_load<uint64_t>(mInfo);
#if mmap_map_LITTLE_ENDIAN
        inc = mmap_map_COUNT_TRAILING_ZEROES(n) / 8;
#else
        inc = mmap_map_COUNT_LEADING_ZEROES(n) / 8;
#endif
        mInfo += inc;
        mKeyVals += inc;
      } while (MMAP_LIB_UNLIKELY(inc == sizeof(uint64_t)));
#else
      while (true) {
        if (*mInfo)
          return;
        mInfo++;
        mKeyVals++;
      }
#endif
    }

    friend class map<MaxLoadFactor100, key_type, T, hasher>;
    NodePtr        mKeyVals;
    uint8_t const* mInfo;
    const map*     map_ptr;
  };

  ////////////////////////////////////////////////////////////////////

  size_t calcNumBytesInfo(size_t numElements) const noexcept {
    const size_t s = sizeof(uint8_t) * (numElements + 1);
    assert(s / sizeof(uint8_t) == numElements + 1);
    // make sure it's a bit larger, so we can load 64bit numbers
    return s + sizeof(uint64_t);
  }
  size_t calcNumBytesNode(size_t numElements) const noexcept {
    const size_t s = sizeof(Node) * numElements;
    assert(s / sizeof(Node) == numElements);
    return s;
  }
  size_t calcNumBytesTotal(size_t numElements) const noexcept {
    const size_t si = calcNumBytesInfo(numElements);
    const size_t sn = calcNumBytesNode(numElements);
    const size_t s  = si + sn;
    assert(!(s <= si || s <= sn));
    return s;
  }

  bool gc_done(void* base, bool force_recycle) const noexcept {
    (void)force_recycle;

    if (mmap_base != base) {  // WARNING: Possible because 2 mmaps can be active during rehash
      return false;
    }

    bool lock_was_set = std::atomic_exchange_explicit(&in_use_mutex, true, std::memory_order_relaxed);
    if (lock_was_set)
      return false; // lock in use, abort!!
    assert(!ref_locked);

    if (mmap_fd >= 0) {
      if (empty()) {
        unlink(mmap_name.c_str());
        mmap_size = 0;  // forget memoize size
      }
    }else{
      mmap_size = 0;  // forget memoize size if no file to backup
    }

    assert(mmap_base);

    local_mMask                  = mmap_base[0];
    local_mNumElements           = mmap_base[1];
    local_mMaxNumElementsAllowed = mmap_base[2];

    mMask                  = &local_mMask;
    mNumElements           = &local_mNumElements;
    mMaxNumElementsAllowed = &local_mMaxNumElementsAllowed;

    mmap_base = nullptr;
    // NOTE: preserve mmap_size to avoid read file in reload
    // mmap_size = 0;
    mmap_fd = -1;

    in_use_mutex.store(false, std::memory_order_release);

    return true;
  }

  size_t calc_mmap_size(size_t nelems) const {
    size_t total = (3 + 2) * sizeof(uint64_t);  // m* fields + 2 for alignment
    total += calcNumBytesTotal(nelems);

    return total;
  }

  size_t calc_num_entries(size_t m_size) const {
    size_t total = m_size - (3 + 2) * sizeof(uint64_t) - sizeof(uint64_t);
    auto   n     = total / (sizeof(Node) + sizeof(uint8_t));
    return n - 1;
  }

  __attribute__((noinline, cold)) void setup_mmap(size_t n_entries) const {
    assert(mmap_base == nullptr);

    auto new_mmap_size = mmap_size;

    if (mmap_name.empty()) {
      // MMAP----------------------------------
      assert(mmap_fd == -1);
      if (n_entries) {  // force a size
        new_mmap_size = calc_mmap_size(n_entries);
      } else {
        assert(mmap_size == 0);  // Reload should not be called unless file backup set
        new_mmap_size = calc_mmap_size(InitialNumElements);
        n_entries     = InitialNumElements;
      }
    } else {
      // MMAP----------------------------------
      if (mmap_fd < 0) {
        mmap_fd = mmap_gc::open(mmap_name);
        assert(mmap_fd >= 0);
      }

      if (n_entries) {  // force a size
        new_mmap_size = calc_mmap_size(n_entries);
      } else if (mmap_size == 0) {  // first reload
        int sz = read(mmap_fd, &n_entries, 8);
        if (sz != 8) {  // Still not mmap
          n_entries = InitialNumElements;
        } else if (n_entries == 0) {  // mmap but all zeroes
          n_entries = InitialNumElements;
        } else {
          n_entries++;
          assert(n_entries >= InitialNumElements);
        }
        new_mmap_size = calc_mmap_size(n_entries);
      } else {
        assert(new_mmap_size);  // preserve last size
        n_entries = InitialNumElements;
        assert(n_entries >= InitialNumElements);
      }
    }

    {
      assert(in_use_mutex);
      auto  gc_func = std::bind(&map<MaxLoadFactor100, Key, T, Hash>::gc_done, this, std::placeholders::_1, std::placeholders::_2);
      void* base    = nullptr;
      std::tie(base, mmap_size) = mmap_gc::mmap(mmap_name, mmap_fd, new_mmap_size, gc_func);
      mmap_base                 = reinterpret_cast<uint64_t*>(base);
    }

    mMask                  = &mmap_base[0];
    mNumElements           = &mmap_base[1];
    mMaxNumElementsAllowed = &mmap_base[2];
    mInfoInc               = reinterpret_cast<InfoType*>(&mmap_base[3]);
    mInfoHashShift         = reinterpret_cast<InfoType*>(&mmap_base[4]);

    mInfo = reinterpret_cast<uint8_t*>(&mmap_base[5]);
    if (*mNumElements != 0) { //if (*mMask == n_entries - 1 || n_entries == 0)
      assert(*mMaxNumElementsAllowed <= *mMask);
      assert(calc_mmap_size(*mMask + 1) <= mmap_size);
      // assert(mInfo[*mMask+1] == 1); // Sentinel
      mKeyVals = reinterpret_cast<Node*>(&mmap_base[5 + (*mMask + 9) / sizeof(uint64_t)]);
    } else {
      assert(*mMaxNumElementsAllowed <= n_entries);  // less due to load factor
      assert(*mNumElements == 0);
      // Setup intial mmap values
      *mMaxNumElementsAllowed = calcMaxNumElementsAllowed(n_entries);
      *mMask                  = n_entries - 1;
      mInfo[n_entries]        = 1;  // Sentinel
      *mInfoInc               = InitialInfoInc;
      *mInfoHashShift         = InitialInfoHashShift;
      mKeyVals                = reinterpret_cast<Node*>(&mmap_base[5 + (*mMask + 9) / sizeof(uint64_t)]);  // 9 to be 8 byte aligned
    }
  }

  void reload() const {
    assert(in_use_mutex);
    if (MMAP_LIB_UNLIKELY(mmap_base == nullptr)) {
      assert(mmap_base == nullptr);
      assert(mmap_fd < 0);

      setup_mmap(0);

      assert(mmap_base);
    }
  }

  // highly performance relevant code.
  // Lower bits are used for indexing into the vector (2^n size)
  // The upper 1-5 bits need to be a reasonable good hash, to save comparisons.
  void keyToIdx(const Key& key, int& idx, InfoType& info) const {
    static constexpr size_t bad_hash_prevention
        = std::is_same<::mmap_lib::hash<key_type>, hasher>::value
              ? 1
              : (mmap_map_BITNESS == 64 ? UINT64_C(0xb3727c1f779b8d8b) : UINT32_C(0xda4afe47));

    reload();

    idx  = Hash::operator()(key) * bad_hash_prevention;
    info = static_cast<InfoType>(*mInfoInc + static_cast<InfoType>(idx >> *mInfoHashShift));
    idx &= *mMask;
  }

  // forwards the index by one, wrapping around at the end
  inline int next_idx(int idx) const {
    idx = (idx + 1) & *mMask;
    return idx;
  }

  inline InfoType next_info(InfoType info) const { return static_cast<InfoType>(info + *mInfoInc); }

  void nextWhileLess(InfoType* info, int* idx) const {
    // unrolling this by hand did not bring any speedups.
    while (*info < mInfo[*idx]) {
      *idx  = next_idx(*idx);
      *info = next_info(*info);
    }
  }

  // Shift everything up by one element. Tries to move stuff around.
  // True if some shifting has occured (entry under idx is a constructed object)
  // Fals if no shift has occured (entry under idx is unconstructed memory)
  void shiftUp(int idx, int const insertion_idx) {
    // FIXME: if the values are trivial, a simple for loop without constructors should be faster/cleaner
#if 0
		if (MMAP_LIB_LIKELY(idx>insertion_idx)) {
			memmove(&mKeyVals[insertion_idx+1],&mKeyVals[insertion_idx],(idx-insertion_idx)*sizeof(Node));
			for(auto i=idx;i>insertion_idx;--i) {
				mInfo[i] = static_cast<uint8_t>(mInfo[i-1] + *mInfoInc);
				if (MMAP_LIB_UNLIKELY(mInfo[i] + *mInfoInc > 0xFF)) {
					*mMaxNumElementsAllowed = 0;
				}
			}

			return;
		}
#endif
    while (idx != insertion_idx) {
      unsigned int prev_idx = (idx - 1) & *mMask;
#if 1
      std::memmove(&mKeyVals[idx], &mKeyVals[prev_idx], sizeof(Node));
#else
      if (mInfo[idx]) {
        // mKeyVals[idx] = std::move(mKeyVals[prev_idx]);
        std::memmove(&mKeyVals[idx], &mKeyVals[prev_idx], sizeof(Node));
      } else {
        ::new (static_cast<void*>(mKeyVals + idx)) Node(std::move(mKeyVals[prev_idx]));
      }
#endif
      mInfo[idx] = static_cast<uint8_t>(mInfo[prev_idx] + *mInfoInc);
      if (MMAP_LIB_UNLIKELY(mInfo[idx] + *mInfoInc > 0xFF)) {
        *mMaxNumElementsAllowed = 0;
      }
      idx = prev_idx;
    }
  }

  void shiftDown(int idx) {
    // until we find one that is either empty or has zero offset.
    // TODO we don't need to move everything, just the last one for the same bucket.
    mKeyVals[idx].destroy(*this);

    while (mInfo[idx + 1] >= 2 * *mInfoInc) {
      mInfo[idx] = static_cast<uint8_t>(mInfo[idx + 1] - *mInfoInc);
      // mKeyVals[idx] = std::move(mKeyVals[idx+1]);
      std::memmove(&mKeyVals[idx], &mKeyVals[idx + 1], sizeof(Node));
      ++idx;
    }

    mInfo[idx] = 0;
    // don't destroy, we've moved it
    // mKeyVals[idx].destroy(*this);
    mKeyVals[idx].~Node();
  }

  inline bool equals(const Key& k1, const Key& k2) const { return k1 == k2; }

  // copy of find(), except that it returns iterator instead of const_iterator.
  template <typename Other>
  int findIdx(Other const& key) const {
    reload();
    int      idx;
    InfoType info;
    keyToIdx(key, idx, info);

    do {
      // unrolling this twice gives a bit of a speedup. More unrolling did not help.
      if (info == mInfo[idx] && equals(key, mKeyVals[idx].getFirst())) {
        return idx;
      }
      idx  = next_idx(idx);
      info = next_info(info);
      if (info == mInfo[idx] && equals(key, mKeyVals[idx].getFirst())) {
        return idx;
      }
      idx  = next_idx(idx);
      info = next_info(info);
    } while (info <= mInfo[idx]);

    // nothing found!
    return -1;  //*mMask == 0 ? 0 : *mMask + 1;
  }

  // inserts a keyval that is guaranteed to be new, e.g. when the hashmap is resized.
  // @return index where the element was created
  size_t insert_move(Node&& keyval) {
    // we don't retry, fail if overflowing
    // don't need to check max num elements
    if (*mMaxNumElementsAllowed == 0) {
      bool ok = try_increase_info();
      (void)ok;
      assert(ok);
    }

    int      idx;
    InfoType info;
    keyToIdx(keyval.getFirst(), idx, info);

    // skip forward. Use <= because we are certain that the element is not there.
    while (info <= mInfo[idx]) {
      idx  = next_idx(idx);
      info = next_info(info);
    }

    // key not found, so we are now exactly where we want to insert it.
    auto const insertion_idx  = idx;
    auto const insertion_info = static_cast<uint8_t>(info);
    if (MMAP_LIB_UNLIKELY(insertion_info + *mInfoInc > 0xFF)) {
      *mMaxNumElementsAllowed = 0;
    }

    // find an empty spot
    while (0 != mInfo[idx]) {
      idx  = next_idx(idx);
      info = next_info(info);
#ifndef NDEBUG
      conflicts++;
#endif
    }

    auto& l = mKeyVals[insertion_idx];
    if (idx == insertion_idx) {
      //::new (static_cast<void*>(&l)) Node(std::move(keyval));
      std::memmove(&l, &keyval, sizeof(Node));
    } else {
      shiftUp(idx, insertion_idx);
      // l = std::move(keyval);
      std::memmove(&l, &keyval, sizeof(Node));
    }

    // put at empty spot
    mInfo[insertion_idx] = insertion_info;
#ifndef NDEBUG
    static int conta = 0;
    if (((++conta) & 0xFFFF) == 0 && *mNumElements > 100) {
      if (conflict_factor() > 0.05) {
        std::cerr << "potential bad hash for mmap_name:" << mmap_name << ", conflicts " << conflicts << "try to debug it\n";
      }
    }
#endif

    ++(*mNumElements);
    return insertion_idx;
  }

  // Local s used when mmap_base is nullptr to remember the value without requiring reload
  mutable uint64_t       local_mNumElements           = 0;
  mutable uint64_t       local_mMask                  = 0;
  mutable uint64_t       local_mMaxNumElementsAllowed = 0;
  static inline InfoType static_InitialInfoInc        = InitialInfoInc;
  static inline InfoType static_InitialInfoHashShift  = InitialInfoHashShift;

  void setup_pointers() {
    assert(local_mMask == 0);

    mNumElements           = &local_mNumElements;
    mMask                  = &local_mMask;
    mMaxNumElementsAllowed = &local_mMaxNumElementsAllowed;
    mInfoInc               = &static_InitialInfoInc;
    mInfoHashShift         = &static_InitialInfoHashShift;
  }

public:
  using iterator       = Iter<false>;
  using const_iterator = Iter<true>;

  explicit map(std::string_view _path, std::string_view _map_name)
      : Hash{Hash{}}
      , mmap_path(_path.empty() ? "." : _path)
      , mmap_name{_map_name.empty() ? "" : (std::string(_path) + std::string("/") + std::string(_map_name))} {
    if (mmap_path != ".") {
      struct stat sb;
      if (stat(mmap_path.c_str(), &sb) != 0 || !S_ISDIR(sb.st_mode)) {
        int e = mkdir(mmap_path.c_str(), 0755);
        (void)e;
        assert(e >= 0);
      }
    }

    setup_pointers();
  }

  explicit map() : Hash{Hash{}} { setup_pointers(); }

  map(map&& o) = delete;
  map& operator=(map&& o) = delete;
  map(const map& o)       = delete;
  map& operator=(map const& o) = delete;
  void swap(map& o)            = delete;

  void clear() {
    assert(!ref_locked); // do not call clear while in an iterator

    if (mmap_base != nullptr) {
      mmap_gc::recycle(mmap_base);
      assert(in_use_mutex || mmap_base == nullptr);
    }

    while(std::atomic_exchange_explicit(&in_use_mutex, true, std::memory_order_relaxed))
      ;

    if (!mmap_name.empty()) {
      unlink(mmap_name.c_str());
    }

    local_mNumElements           = 0;
    local_mMask                  = 0;
    local_mMaxNumElementsAllowed = 0;
    assert(*mNumElements == 0);

    in_use_mutex.store(false, std::memory_order_release);
  }

  // Destroys the map and all it's contents.
  virtual ~map() { destroy(); }

  int set(key_type&& key, T&& val) { return doCreate(std::move(key), std::move(val)); }
  int set(const key_type& key, T&& val) { return doCreate(key, std::move(val)); }
  int set(const key_type& key, const T& val) { return doCreate(key, val); }
  int set(key_type&& key, const T& val) { return doCreate(std::move(key), val); }

  // Returns 1 if key is found, 0 otherwise.
  [[nodiscard]] bool has(const key_type& key) const {
    if (!ref_locked)
      while(std::atomic_exchange_explicit(&in_use_mutex, true, std::memory_order_relaxed))
        ;

    auto ret = findIdx(key) >= 0;

    if (!ref_locked) {
      in_use_mutex.store(false, std::memory_order_release);
    }

    return ret;
  }

  [[nodiscard]] int find_key(const key_type& key) const {
    if (!ref_locked)
      while(std::atomic_exchange_explicit(&in_use_mutex, true, std::memory_order_relaxed))
        ;

    auto ret = findIdx(key);

    if (!ref_locked) {
      in_use_mutex.store(false, std::memory_order_release);
    }

    return ret;
  }

  size_t get_lock_num() const {
    return ref_locked;
  }

  void ref_lock() const {
    // Single thread check: assert(in_use_mutex == 0);
    if (ref_locked==0)
      while(std::atomic_exchange_explicit(&in_use_mutex, true, std::memory_order_relaxed))
        ;
    ++ref_locked;
  }

  void ref_unlock() const {
    assert(ref_locked);
    --ref_locked;
    assert(in_use_mutex);
    if (ref_locked==0) {
      in_use_mutex.store(false, std::memory_order_release);
    }
  }

  [[nodiscard]] const T get(key_type const& key) const {
    if (!ref_locked)
      while(std::atomic_exchange_explicit(&in_use_mutex, true, std::memory_order_relaxed))
        ;

    const auto idx = findIdx(key);
    assert(idx >= 0);

    const auto ret = mKeyVals[idx].getSecond();

    if (!ref_locked) {
      in_use_mutex.store(false, std::memory_order_release);
    }

    return ret;
  }

#if 0
  [[nodiscard]] const T  get(const const_iterator& it) const {
    assert(ref_locked);
    return it->second;
  }

  [[nodiscard]] Key get_key(const const_iterator& it) const {
    assert(ref_locked);
    return it->first;
  }
  [[nodiscard]] Key get_key(const value_type& it) const {
    assert(ref_locked);
    return it.first;
  }
#endif

  [[nodiscard]] T* ref(key_type const& key) {
    assert(ref_locked);

    auto idx = findIdx(key);
    assert(idx >= 0);

    return &mKeyVals[idx].getSecond();
  }

  [[nodiscard]] T* ref(const value_type& it) {
    assert(ref_locked);

    return &it.second;
  }

  [[nodiscard]] T* ref(iterator& it) {
    assert(ref_locked);
    return &it->second;
  }

  [[nodiscard]] const_iterator find(const key_type& key) const {
    ref_lock();
    assert(ref_locked); // when the iterator is destroyed, the ref_unlock will be called

    const auto idx = findIdx(key);
    if (idx < 0)
      return const_iterator{this, reinterpret_cast<Node*>(&mKeyVals[*mMask + 1]), nullptr};

    return const_iterator{this, mKeyVals + idx, mInfo + idx};
  }

  template <typename OtherKey>
  [[nodiscard]] const_iterator find(const OtherKey& key, is_transparent_tag /*unused*/) const {
    ref_lock();

    const auto idx = findIdx(key);
    if (idx < 0)
      return const_iterator{this, reinterpret_cast<Node*>(&mKeyVals[*mMask + 1]), nullptr};

    return const_iterator{this, mKeyVals + idx, mInfo + idx};
  }

  [[nodiscard]] iterator find(const key_type& key) {
    ref_lock();

    const auto idx = findIdx(key);
    if (idx < 0)
      return iterator{this, reinterpret_cast<Node*>(&mKeyVals[*mMask + 1]), nullptr};

    return iterator{this, mKeyVals + idx, mInfo + idx};
  }

  template <typename OtherKey>
  [[nodiscard]] iterator find(const OtherKey& key, is_transparent_tag /*unused*/) {
    ref_lock();

    const auto idx = findIdx(key);
    if (idx < 0)
      return iterator{this, reinterpret_cast<Node*>(&mKeyVals[*mMask + 1]), nullptr};

    return iterator{this, mKeyVals + idx, mInfo + idx};
  }

  [[nodiscard]] iterator begin() {
    ref_lock();

    reload();

    if (empty()) {
      return iterator{this, reinterpret_cast<Node*>(&mKeyVals[*mMask + 1]), nullptr};
    }
    return iterator{this, mKeyVals, mInfo, fast_forward_tag{}};
  }
  [[nodiscard]] const_iterator begin() const {
    return cbegin();
  }
  [[nodiscard]] const_iterator cbegin() const {
    ref_lock();

    reload();

    if (empty()) {
      return const_iterator{this, reinterpret_cast<Node*>(&mKeyVals[*mMask + 1]), nullptr};
    }
    return const_iterator{this, mKeyVals, mInfo, fast_forward_tag{}};
  }

  [[nodiscard]] iterator end() {
    ref_lock();

    reload();
    // no need to supply valid info pointer: end() must not be dereferenced, and only node
    // pointer is compared.
    return iterator{this, reinterpret_cast<Node*>(&mKeyVals[*mMask + 1]), nullptr};
  }
  [[nodiscard]] const_iterator end() const {
    return cend();
  }
  [[nodiscard]] const_iterator cend() const {
    ref_lock();
    reload();
    return const_iterator{this, reinterpret_cast<Node*>(&mKeyVals[*mMask + 1]), nullptr};
  }

  iterator erase(const_iterator& pos) {
    assert(ref_locked);

    // its safe to perform const cast here
    return erase(iterator{this, const_cast<Node*>(pos.mKeyVals), const_cast<uint8_t*>(pos.mInfo)});
  }

#if 0
  // Erases element at pos, returns iterator to the next element.
  iterator erase(iterator &pos) {
    assert(ref_locked);

    // we assume that pos always points to a valid entry, and not end().
    auto const idx = static_cast<size_t>(pos.mKeyVals - mKeyVals);

    shiftDown(idx);
    --(*mNumElements);

    if (*pos.mInfo) {
      // we've backward shifted, return this again
      return pos;
    }

    // no backward shift, return next element
    auto it = pos; // copy iterator and move it
    return ++it;
  }
#else
  bool erase(const iterator &pos) {
    assert(ref_locked);

    // we assume that pos always points to a valid entry, and not end().
    auto const idx = static_cast<size_t>(pos.mKeyVals - mKeyVals);

    shiftDown(idx);
    --(*mNumElements);

    if (*pos.mInfo) {
      return false;
    }

    return true;
  }
#endif

  size_t erase(const key_type& key) {
    if (!ref_locked)
      while(std::atomic_exchange_explicit(&in_use_mutex, true, std::memory_order_relaxed))
        ;

    int      idx;
    InfoType info;
    keyToIdx(key, idx, info);

    // check while info matches with the source idx
    do {
      if (info == mInfo[idx] && equals(key, mKeyVals[idx].getFirst())) {
        shiftDown(idx);
        --(*mNumElements);

        if (!ref_locked) {
          in_use_mutex.store(false, std::memory_order_release);
        }

        return 1;
      }
      idx  = next_idx(idx);
      info = next_info(info);
    } while (info <= mInfo[idx]);

    if (!ref_locked) {
      in_use_mutex.store(false, std::memory_order_release);
    }

    return 0;
  }

  void reserve(size_t count) {
    if (!ref_locked)
      while(std::atomic_exchange_explicit(&in_use_mutex, true, std::memory_order_relaxed))
        ;

    auto newSize = InitialNumElements > *mMask + 1 ? InitialNumElements : *mMask + 1;
    while (calcMaxNumElementsAllowed(newSize) < count && newSize != 0) {
      newSize *= 2;
    }
    assert(newSize != 0);

    rehash(newSize);

    if (!ref_locked) {
      in_use_mutex.store(false, std::memory_order_release);
    }
  }

  [[nodiscard]] size_type size() const { return *mNumElements; }

  [[nodiscard]] bool empty() const { return 0 == *mNumElements; }

  [[nodiscard]] inline std::string_view get_name() const { return mmap_name; }
  [[nodiscard]] inline std::string_view get_path() const { return mmap_path; }

  [[nodiscard]] size_t capacity() const {
    if (!ref_locked)
      while(std::atomic_exchange_explicit(&in_use_mutex, true, std::memory_order_relaxed))
        ;

    if (mmap_base) {
      auto ret =  *mMaxNumElementsAllowed;
      if (!ref_locked) {
        in_use_mutex.store(false, std::memory_order_release);
      }
      return ret;
    }
    auto ret = calcMaxNumElementsAllowed(InitialNumElements);

    if (!ref_locked) {
      in_use_mutex.store(false, std::memory_order_release);
    }

    return ret;
  }

  [[nodiscard]] float max_load_factor() const { return MaxLoadFactor100 / 100.0f; }

  // Average number of elements per bucket. Since we allow only 1 per bucket
  [[nodiscard]] float load_factor() const { return static_cast<float>(size()) / (*mMask + 1); }

#ifndef NDEBUG
  float conflict_factor() const { return static_cast<float>(conflicts) / (*mNumElements + 1); }
#else
  float conflict_factor() const { return 0.0; }
#endif

private:
  void rehash(size_t numBuckets) {
    assert(in_use_mutex);
    assert(MMAP_LIB_UNLIKELY((numBuckets & (numBuckets - 1)) == 0));  // rehash only allowed for power of two

    reload();

    const size_t oldMaxElements = *mMask + 1;
    if (oldMaxElements >= numBuckets)
      return;  // done

    if (mmap_fd >= 0) {
      mmap_gc::delete_file(mmap_base);
      mmap_fd = -1;
    }

    uint64_t* old_mmap_base = mmap_base;

    Node* const          oldKeyVals = mKeyVals;
    uint8_t const* const oldInfo    = mInfo;

    assert(mmap_fd == -1);
    mmap_base                    = nullptr;
    local_mMask                  = 0;  // reset for new mmap
    local_mNumElements           = 0;
    local_mMaxNumElementsAllowed = 0;
    setup_mmap(numBuckets);

    assert(old_mmap_base != mmap_base);
    assert(oldKeyVals != mKeyVals);
    assert(oldInfo != mInfo);
    assert(*mNumElements == 0);
    assert(*mMask == numBuckets - 1);
    assert(*mMaxNumElementsAllowed == calcMaxNumElementsAllowed(numBuckets));

    // std::cout << "resize sz:" << numBuckets << " mmap_name:" << mmap_name << "\n";

    for (size_t i = 0; i < oldMaxElements; ++i) {
      if (oldInfo[i] != 0) {
        insert_move(std::move(oldKeyVals[i]));
        // destroy the node but DON'T destroy the data.
        oldKeyVals[i].~Node();
      }
    }

    // don't destroy old data: put it into the pool instead
    mmap_gc::recycle(old_mmap_base);
  }

  size_t mask() const { return *mMask; }

  template <typename Arg, typename Data>
  int doCreate(Arg&& key, Data&& val) {
    if (!ref_locked)
      while(std::atomic_exchange_explicit(&in_use_mutex, true, std::memory_order_relaxed))
        ;

    reload();
    while (true) {
      int      idx;
      InfoType info;
      keyToIdx(key, idx, info);
      nextWhileLess(&info, &idx);

      // while we potentially have a match. Can't do a do-while here because when mInfo is 0
      // we don't want to skip forward
      bool found = false;
      while (info == mInfo[idx]) {
        if (equals(key, mKeyVals[idx].getFirst())) {
          found = true;
          break;
        }
        idx  = next_idx(idx);
        info = next_info(info);
      }

      auto const insertion_idx  = idx;
      auto const insertion_info = info;
      if (!found) {
        // unlikely that this evaluates to true
        if (MMAP_LIB_UNLIKELY(*mNumElements >= *mMaxNumElementsAllowed)) {
          increase_size();
          continue;
        }

        // key not found, so we are now exactly where we want to insert it.
        if (MMAP_LIB_UNLIKELY(insertion_info + *mInfoInc > 0xFF)) {
          *mMaxNumElementsAllowed = 0;
        }

        // find an empty spot
        while (0 != mInfo[idx]) {
          idx  = next_idx(idx);
          info = next_info(info);
        }
      }

      auto& l = mKeyVals[insertion_idx];
      if (idx == insertion_idx) {
        // put at empty spot. This forwards all arguments into the node where the object is
        // constructed exactly where it is needed.
        ::new (static_cast<void*>(&l))
          Node(*this, std::piecewise_construct, std::forward_as_tuple(std::forward<Arg>(key)), std::forward_as_tuple(val));
      } else {
        assert(!found);
        shiftUp(idx, insertion_idx);
        ::new (&l)
          Node(*this, std::piecewise_construct, std::forward_as_tuple(std::forward<Arg>(key)), std::forward_as_tuple(val));
      }

      if (!found) {
        // mKeyVals[idx].getFirst() = std::move(key);
        mInfo[insertion_idx] = static_cast<uint8_t>(insertion_info);

        ++(*mNumElements);
      }

      if (!ref_locked) {
        in_use_mutex.store(false, std::memory_order_release);
      }

      return insertion_idx;
    }
    assert(false);
    return 0;
  }

  size_t calcMaxNumElementsAllowed(size_t maxElements) const {
    static constexpr size_t overflowLimit = (std::numeric_limits<size_t>::max)() / 100;
    static constexpr double factor        = MaxLoadFactor100 / 100.0;

    // make sure we can't get an overflow; use floatingpoint arithmetic if necessary.
    if (maxElements > overflowLimit) {
      return static_cast<size_t>(static_cast<double>(maxElements) * factor);
    } else {
      return (maxElements * MaxLoadFactor100) / 100;
    }
  }

  bool try_increase_info() {
    mmap_map_LOG("mInfoInc=" << *mInfoInc << ", numElements=" << mNumElements
                             << ", maxNumElementsAllowed=" << calcMaxNumElementsAllowed(mMask + 1));
    if (*mInfoInc <= 2) {
      // need to be > 2 so that shift works (otherwise undefined behavior!)
      return false;
    }
    // we got space left, try to make info smaller
    *mInfoInc = static_cast<uint8_t>(*mInfoInc >> 1);

    // remove one bit of the hash, leaving more space for the distance info.
    // This is extremely fast because we can operate on 8 bytes at once.
    ++(*mInfoHashShift);
    auto const data       = reinterpret_cast<uint64_t*>(mInfo);
    auto const numEntries = (*mMask + 1) / 8;

    for (size_t i = 0; i < numEntries; ++i) {
      data[i] = (data[i] >> 1) & UINT64_C(0x7f7f7f7f7f7f7f7f);
    }
    *mMaxNumElementsAllowed = calcMaxNumElementsAllowed(*mMask + 1);
    return true;
  }

  void increase_size() {
    // nothing allocated yet? just allocate InitialNumElements
    if (*mMask == 0) {
      reload();
      return;
    }

    auto const maxNumElementsAllowed = calcMaxNumElementsAllowed(*mMask + 1);
    if (*mNumElements < maxNumElementsAllowed && try_increase_info()) {
      return;
    }

    mmap_map_LOG("mNumElements=" << *mNumElements << ", maxNumElementsAllowed=" << maxNumElementsAllowed
                                 << ", load=" << (static_cast<double>(*mNumElements) * 100.0 / (static_cast<double>(*mMask) + 1)));
    // it seems we have a really bad hash function! don't try to resize again
    assert(*mNumElements * 2 >= calcMaxNumElementsAllowed(*mMask + 1));

    rehash((*mMask + 1) * 2);
  }

  void destroy() {
    if (mmap_base) {
      mmap_gc::recycle(mmap_base);
      assert(mmap_base == nullptr);
      assert(mmap_fd == -1);
    }
  }

  mutable Node*     mKeyVals = nullptr;
  mutable uint8_t*  mInfo    = nullptr;
  mutable uint64_t* mNumElements;
  mutable uint64_t* mMask;
  mutable uint64_t* mMaxNumElementsAllowed;
  mutable InfoType* mInfoInc;
  mutable InfoType* mInfoHashShift;
  const std::string mmap_path;
  const std::string mmap_name;
  mutable int       mmap_fd       = -1;
  mutable size_t    mmap_size     = 0;
  mutable uint64_t* mmap_base     = 0;
  mutable std::atomic<bool> in_use_mutex{false};
  mutable std::atomic<int>  ref_locked{0};
#ifndef NDEBUG
  size_t conflicts = 0;
#endif
};

}  // namespace detail

template <typename Key, typename T, typename Hash = hash<Key>, size_t MaxLoadFactor100 = 80>
using map = detail::map<MaxLoadFactor100, Key, T, Hash>;

}  // namespace mmap_lib
