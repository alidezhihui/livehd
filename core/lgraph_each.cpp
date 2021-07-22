//  This file is distributed under the BSD 3-Clause License. See LICENSE for details.

#include "absl/container/flat_hash_map.h"
#include "lgedgeiter.hpp"
#include "lgraph.hpp"
#include "mmap_map.hpp"
#include "node.hpp"
#include "node_pin.hpp"
#include "sub_node.hpp"
#include "thread_pool.hpp"

//#define NO_BOTTOM_UP_PARALLEL 1

void Lgraph::each_sorted_graph_io(const std::function<void(Node_pin &pin, Port_ID pos)>& f1, bool hierarchical) {
  if (node_internal.size() < Hardcoded_output_nid)
    return;

  struct Pair_type {
    Pair_type(Lgraph *lg, Hierarchy_index hidx, Index_id idx, Port_ID pid, Port_ID _pos)
        : dpin(lg, lg, hidx, idx, pid, false), pos(_pos) {}
    Node_pin dpin;
    Port_ID  pos;
  };
  std::vector<Pair_type> pin_pair;

  auto hidx = hierarchical ? Hierarchy_tree::root_index() : Hierarchy_tree::invalid_index();

  for (const auto &io_pin : get_self_sub_node().get_io_pins()) {
    if (io_pin.is_invalid())
      continue;

    Port_ID pid = get_self_sub_node().get_instance_pid(io_pin.name);

    Index_id nid = Hardcoded_output_nid;
    if (io_pin.is_input())
      nid = Hardcoded_input_nid;
    auto idx = find_idx_from_pid(nid, pid);
    if (idx) {
      Pair_type p(this, hidx, idx, pid, io_pin.graph_io_pos);
      if (p.dpin.has_name()) {
        pin_pair.emplace_back(p);
      }
    }
  }

  std::sort(pin_pair.begin(), pin_pair.end(), [](const Pair_type &a, const Pair_type &b) -> bool {
    if (a.pos == Port_invalid && b.pos == Port_invalid) {
      if (a.dpin.is_graph_input() && b.dpin.is_graph_output()) {
        return true;
      }
      if (a.dpin.is_graph_output() && b.dpin.is_graph_input()) {
        return false;
      }
      if (a.dpin.is_graph_input() && b.dpin.is_graph_input()) {
        auto a_name = a.dpin.get_name();
        if (a_name == "clock")
          return true;
        if (a_name == "reset")
          return true;
        auto b_name = b.dpin.get_name();
        if (b_name == "clock")
          return false;
        if (b_name == "reset")
          return false;
      }

      return a.dpin.get_name() < b.dpin.get_name();
    }
    if (a.pos == Port_invalid)
      return true;
    if (b.pos == Port_invalid)
      return false;

    return a.pos < b.pos;
  });

  for (auto &pp : pin_pair) {
    f1(pp.dpin, pp.pos);
  }
}

void Lgraph::each_pin(const Node_pin &dpin, const std::function<bool(Index_id idx)>& f1) const {
  Index_id first_idx2 = dpin.get_root_idx();
  Index_id idx2       = first_idx2;

  bool should_not_find = false;

  while (true) {
    I(!should_not_find);
    bool cont = f1(idx2);
    if (!cont)
      return;

    node_internal.ref_lock();
    do {
      if (node_internal.ref(idx2)->is_last_state()) {
        node_internal.ref_unlock();
        return;
      } else {
        idx2 = node_internal.ref(idx2)->get_next();
      }
      if (idx2 == first_idx2) {
        node_internal.ref_unlock();
        return;
      }
    } while (node_internal.ref(idx2)->get_dst_pid() != dpin.get_pid());
    node_internal.ref_unlock();
  }
}

void Lgraph::each_graph_input(const std::function<void(Node_pin &pin)>& f1, bool hierarchical) {
  if (node_internal.size() < Hardcoded_output_nid)
    return;

  auto hidx = hierarchical ? Hierarchy_tree::root_index() : Hierarchy_tree::invalid_index();

  for (const auto &io_pin : get_self_sub_node().get_io_pins()) {
    if (io_pin.is_input()) {
      Port_ID pid = get_self_sub_node().get_instance_pid(io_pin.name);
      auto    idx = find_idx_from_pid(Hardcoded_input_nid, pid);
      if (idx) {
        Node_pin dpin(this, this, hidx, idx, pid, false);
        if (dpin.has_name())
          f1(dpin);
      }
    }
  }
}

void Lgraph::each_graph_output(const std::function<void(Node_pin &pin)>& f1, bool hierarchical) {
  if (node_internal.size() < Hardcoded_output_nid)
    return;

  auto hidx = hierarchical ? Hierarchy_tree::root_index() : Hierarchy_tree::invalid_index();

  for (const auto &io_pin : get_self_sub_node().get_io_pins()) {
    if (io_pin.is_output()) {
      Port_ID pid = get_self_sub_node().get_instance_pid(io_pin.name);
      auto    idx = find_idx_from_pid(Hardcoded_output_nid, pid);
      if (idx) {
        Node_pin dpin(this, this, hidx, idx, pid, false);
        if (dpin.has_name())  // It could be partially deleted
          f1(dpin);
      }
    }
  }
}

void Lgraph::each_local_sub_fast_direct(const std::function<bool(Node &, Lg_type_id)>& fn) {
  for (auto e : get_down_nodes_map()) {
    Index_id cid = e.first.nid;
    I(cid);

    auto node = Node(this, e.first);

    bool cont = fn(node, e.second);
    if (!cont)
      return;
  }
}

void Lgraph::each_hier_fast(const std::function<bool(Node &)>& f) {
  const auto ht = ref_htree();

  for (const auto &hidx : ht->depth_preorder()) {
    Lgraph *lg = ht->ref_lgraph(hidx);
    for (auto fn : lg->fast()) {
      Node hn(this, lg, hidx, fn.nid);

      if (!f(hn)) {
        return;
      }
    }
  }
}

void Lgraph::each_local_unique_sub_fast(const std::function<bool(Lgraph *sub_lg)>& fn) {
  std::set<Lg_type_id> visited;
  for (auto e : get_down_nodes_map()) {
    Index_id cid = e.first.nid;
    I(cid);

    if (visited.find(e.second) != visited.end())
      continue;

    visited.insert(e.second);

    auto *sub_lg = Lgraph::open(path, e.second);
    if (sub_lg) {
      bool cont = fn(sub_lg);
      if (!cont)
        return;
    }
  }
}

void Lgraph::each_hier_unique_sub_bottom_up_int(std::set<Lg_type_id> &visited, const std::function<void(Lgraph *lg_sub)>& fn) {
  for (auto e : get_down_nodes_map()) {
    Index_id cid = e.first.nid;
    I(cid);

    if (visited.find(e.second) != visited.end())
      continue;

    Lgraph *lg = Lgraph::open(get_path(), e.second);
    if (lg == nullptr)
      continue;

    lg->each_hier_unique_sub_bottom_up_int(visited, fn);
    if (visited.find(e.second) == visited.end()) {
      visited.insert(e.second);
      fn(lg);
    }
  }
}

void Lgraph::each_hier_unique_sub_bottom_up(const std::function<void(Lgraph *lg_sub)>& fn) {
  std::set<Lg_type_id> visited;
  each_hier_unique_sub_bottom_up_int(visited, fn);
}

void Lgraph::each_hier_unique_sub_bottom_up_parallel(const std::function<void(Lgraph *lg_sub)>& fn) {
  std::unordered_map<uint32_t, int> visited;

  std::vector<Lgraph *> next_round;

  const auto &href = get_htree();

  href.each_bottom_up_fast([this, &href, &visited, &next_round](const Hierarchy_index &hidx, const Hierarchy_data &data) {
    auto it = visited.find(data.lgid);
    if (it != visited.end())
      return;
    if (unlikely(hidx.is_root()))
      return;

    // I(href.is_leaf(hidx));  // Otherwise, it will be not visited
    visited[data.lgid] = 0;

    auto *lg = Lgraph::open(path, data.lgid);
    if (lg != nullptr && !lg->is_empty())
      next_round.emplace_back(lg);

    auto index = href.get_parent(hidx);
    int  level = 0;
    while (!index.is_root()) {
      const auto index_lgid = href.get_data(index).lgid;

      const auto it2 = visited.find(index_lgid);
      if (it2 == visited.end()) {
        visited[index_lgid] = level;
      } else {
        if (it2->second > level) {
          level = it2->second;
        } else {
          it2->second = level;
        }
      }
      index = href.get_parent(index);
      level = level + 1;
    }
  });

  for (auto *lg : next_round) {
#ifdef NO_BOTTOM_UP_PARALLEL
    fn(lg);
#else
    thread_pool.add(fn, lg);
#endif
    visited.erase(lg->get_lgid());
  }
  if (!next_round.empty())
    thread_pool.wait_all();

  int level = 0;
  while (!visited.empty()) {
    next_round.clear();
    auto it = visited.begin();
    while (it != visited.end()) {
      if (it->second > level) {
        ++it;
        continue;
      }
      I(level == it->second);

      auto *lg = Lgraph::open(path, it->first);
      if (lg != nullptr && !lg->is_empty())
        next_round.emplace_back(lg);
      it = visited.erase(it);
    }
    ++level;
    for (auto *lg : next_round) {
#ifdef NO_BOTTOM_UP_PARALLEL
      fn(lg);
#else
      thread_pool.add(fn, lg);
#endif
    }
    if (!next_round.empty())
      thread_pool.wait_all();
  }
}
