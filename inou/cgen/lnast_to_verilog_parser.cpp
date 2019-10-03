
#include "lnast_to_verilog_parser.hpp"

std::map<std::string, std::string> Lnast_to_verilog_parser::stringify(std::string filepath) {
  root_filename = get_filename(filepath);
  curr_module = new Verilog_parser_module(root_filename);

  inc_indent_buffer();
  inc_indent_buffer();
  for (const mmap_lib::Tree_index &it: lnast->depth_preorder(lnast->get_root())) {
    process_node(it);
  }
  process_buffer();
  dec_indent_buffer();

  file_map.insert(std::pair<std::string, std::string>(curr_module->filename, curr_module->create_file()));
  return file_map;
}

// infustructure
// don't need to change for anything
void Lnast_to_verilog_parser::process_node(const mmap_lib::Tree_index& it) {
  const auto& node_data = lnast->get_data(it);
  std::string type = ntype_dbg(node_data.type);

  // add while to see pop_statement and to add buffer
  if (it.level < curr_statement_level) {
    pop_statement(it.level, node_data.type);

    while (it.level + 1 < curr_statement_level) {
      pop_statement(it.level, node_data.type);
    }
    type = ntype_dbg(node_data.type);
  }

  if (node_data.type == Lnast_ntype_top) {
    process_top(it.level);
  } else if (node_data.type == Lnast_ntype_statements) {
    // check the buffer to see if this is an if statement
    // and if it is, check if this is an ifel or an else
    add_to_buffer(node_data);
    push_statement(it.level, node_data.type);
  } else if (node_data.type == Lnast_ntype_cstatements) {
    add_to_buffer(node_data);
    push_statement(it.level, node_data.type);
  } else if (it.level == curr_statement_level) {
    fmt::print("standard process_buffer\n");
    process_buffer();
    add_to_buffer(node_data);
    fmt::print("finished process_buffer\n");
  } else {
    add_to_buffer(node_data);
  }

  if (node_data.type == Lnast_ntype_func_def) {
    module_stack.push_back(curr_module);
    curr_module = new Verilog_parser_module();
  }
}

// no change
void Lnast_to_verilog_parser::process_top(mmap_lib::Tree_level level) {
  level_stack.push_back(level);
  curr_statement_level = level;
}

// no change
void Lnast_to_verilog_parser::push_statement(mmap_lib::Tree_level level, Lnast_ntype_id type) {
  fmt::print("push\n");

  level = level + 1;
  level_stack.push_back(curr_statement_level);
  prev_statement_level = curr_statement_level;
  curr_statement_level = level;

  buffer_stack.push_back(node_buffer);
  node_buffer.clear();

  // change the current Verilog_parser_module
  if (Lnast_ntype_statements == type) {
    curr_module->node_buffer_stack();
    inc_indent_buffer();
  }

  fmt::print("after push\n");
}

void Lnast_to_verilog_parser::pop_statement(mmap_lib::Tree_level level, Lnast_ntype_id type) {
  (void)level;
  (void)type;
  fmt::print("pop\n");

  process_buffer();

  node_buffer = buffer_stack.back();
  buffer_stack.pop_back();

  // change to update Verilog_parser_module
  if (Lnast_ntype_statements == node_buffer.back().type) {
    curr_module->node_buffer_queue();
    dec_indent_buffer();
  }

  level_stack.pop_back();
  curr_statement_level = prev_statement_level;
  prev_statement_level = level_stack.back();
}

void Lnast_to_verilog_parser::add_to_buffer(Lnast_node node) {
  node_buffer.push_back(node);
}

void Lnast_to_verilog_parser::process_buffer() {
  if (!node_buffer.size()) return;

  Lnast_ntype_id type = node_buffer.front().type;

  if (type == Lnast_ntype_pure_assign) {
    // check if should be in combinational or stateful
    process_pure_assign();
  } else if (type == Lnast_ntype_as) {
    process_operator();
    // process_as();
  } else if (type == Lnast_ntype_label) {
    process_label();
  } else if (type == Lnast_ntype_and) {
    process_operator();
    // process_and();
  } else if (type == Lnast_ntype_xor) {
    process_operator();
    // process_xor();
  } else if (type == Lnast_ntype_plus) {
    process_operator();
    // process_plus();
  } else if (type == Lnast_ntype_gt) {
    process_operator();
    // process_gt();
  } else if (type == Lnast_ntype_if) {
    process_if();
  } else if (type == Lnast_ntype_func_call) {
    process_func_call();
  } else if (type == Lnast_ntype_func_def) {
    process_func_def();
  }

  for (auto const& node : node_buffer) {
    std::string name(node.token.get_text(memblock)); // str_view to string
    std::string type_dbg = ntype_dbg(node.type);
    if (name == "") {
      fmt::print("{}({}) ", type_dbg, node.type);
    } else {
      fmt::print("{} ", name);
    }
  }
  fmt::print("\n");

  node_buffer.clear();
}

std::string_view Lnast_to_verilog_parser::get_node_name(Lnast_node node) {
  return node.token.get_text(memblock);
}

bool Lnast_to_verilog_parser::is_number(std::string_view test_string) {
  // TODO(joapena): check how to use regex
  return test_string.at(1) == 'd';
}

std::string_view Lnast_to_verilog_parser::process_number(std::string_view num) {
    std::string::size_type n;
    n = num.find("d");
    return num.substr(n + 1);
}

bool Lnast_to_verilog_parser::is_ref(std::string_view test_string) {
  /*
  std::regex e("___[a-zA-Z]");
  return std::regex_match(test_string, e);
  */
  // TODO(joapena): check for better way to compare string
  return test_string.find("___") == 0;
}

void Lnast_to_verilog_parser::inc_indent_buffer(){
  // indent_buffer() = absl::StrCat(indent_buffer(), "  ");
  indent_buffer_size++;
}

void Lnast_to_verilog_parser::dec_indent_buffer() {
  // indent_buffer() = indent_buffer().substr(0, indent_buffer().size() - 2);
  indent_buffer_size--;
}

std::string Verilog_parser_module::indent_buffer(int32_t size) {
  return std::string(size * 2, ' ');
}

std::string Verilog_parser_module::create_header() {
  std::string inputs = "input clk,\ninput reset";
  std::string outputs;
  std::string wires;

  for (auto var_name : statefull_set) {
    uint32_t var_type = get_variable_type(var_name);
    if (var_type == 1) {
      inputs = absl::StrCat(inputs, ",\ninput ", var_name);
    } else if (var_type == 2) {
      outputs = absl::StrCat(outputs, ",\noutput ", var_name);
    } else {
      wires = absl::StrCat(wires, "  wire ", var_name, ";\n");
    }
  }

  return absl::StrCat("module ", filename, " (", inputs, outputs, ");\n", wires, "\n");
}

std::string Verilog_parser_module::create_footer() {
  // TODO(joapena): probably didn't need this, will evaulate later
  return absl::StrCat("end module\n");
}

std::string Verilog_parser_module::create_always() {
  // return absl::StrCat(indent_buffer(1), "always @(*) begin\n", logic, indent_buffer(1), "end\n");
  std::string buffer = absl::StrCat(indent_buffer(1), "always @(*) begin\n");

  for (auto node : node_str_buffer) {
    buffer = absl::StrCat(buffer, indent_buffer(node.first), node.second);
  }

  return absl::StrCat(buffer, indent_buffer(1), "end\n");
}

std::string Verilog_parser_module::create_next() {
  std::string tmp = absl::StrCat(indent_buffer(1), "always @(posedge clk) begin\n");
  // inc_indent_buffer();
  for (auto ele : statefull_set) {
    if (get_variable_type(ele) == 2) {
      tmp = absl::StrCat(tmp, indent_buffer(2), ele, " = ", ele, "_next\n");
    }
  }
  // dec_indent_buffer();
  return absl::StrCat(tmp, indent_buffer(1), "end\n");
}

std::string Verilog_parser_module::create_file() {
  return absl::StrCat(create_header(), create_always(), create_next(), create_footer());
}

void Verilog_parser_module::add_to_buffer_single(std::pair<int32_t, std::string> next, std::set<std::string_view> new_vars) {
  node_str_buffer.push_back(next);
  statefull_set.insert(std::make_move_iterator(new_vars.begin()), std::make_move_iterator(new_vars.end()));
}

void Verilog_parser_module::add_to_buffer_multiple(std::vector<std::pair<int32_t, std::string>> nodes, std::set<std::string_view> new_vars) {
  node_str_buffer.insert(node_str_buffer.end(), std::make_move_iterator(nodes.begin()), std::make_move_iterator(nodes.end()));
  statefull_set.insert(std::make_move_iterator(new_vars.begin()), std::make_move_iterator(new_vars.end()));
}

std::string Lnast_to_verilog_parser::get_filename(std::string filepath) {
  std::vector<std::string> filepath_split = absl::StrSplit(filepath, '/');
  std::pair<std::string, std::string> fname = absl::StrSplit(filepath_split[filepath_split.size() - 1], '.');
  return fname.first;
}

// returns 1 if input
// returns 2 if output
// otherwise returns 0
uint32_t Verilog_parser_module::get_variable_type(std::string_view var_name) {
  if (var_name.at(0) == '$') {
    return 1;
  } else if (var_name.at(0) == '%'){
    return 2;
  }

  return 0;
}

void Verilog_parser_module::node_buffer_stack() {
    sts_buffer_stack.push_back(node_str_buffer);
    node_str_buffer.clear();
}

void Verilog_parser_module::node_buffer_queue() {
  sts_buffer_queue.push_back(node_str_buffer);
  node_str_buffer = sts_buffer_stack.back();
  sts_buffer_stack.pop_back();
}

std::vector<std::pair<int32_t, std::string>> Verilog_parser_module::pop_queue() {
  std::vector<std::pair<int32_t, std::string>> tmp = sts_buffer_queue.front();
  sts_buffer_queue.erase(sts_buffer_queue.begin());
  return tmp;
}

void Lnast_to_verilog_parser::process_pure_assign() {
  std::string value = "";
  std::set<std::string_view> new_vars;

  std::vector<Lnast_node>::iterator it = node_buffer.begin();
  it++;
  std::string_view key = get_node_name(*it);
  it++;

  std::map<std::string_view, std::pair<std::string, std::set<std::string_view>>>::iterator map_it;
  std::string_view ref = get_node_name(*it);
  map_it = ref_map.find(ref);
  if (map_it != ref_map.end()) {
    ref = map_it->second.first;
    new_vars.insert(map_it->second.second.begin(), map_it->second.second.end());
    // connect the two stateful stuff
    fmt::print("map_it: find: {} | {}\n", map_it->first, map_it->second.first);
  } else if (!is_number(ref)) {
    new_vars.insert(ref);
  }
  value = absl::StrCat(value, ref);

  if (is_ref(key)) {
    fmt::print("map_it: inserting:\tkey:{}\tvalue:{}\n", key, value);
    ref_map.insert(std::pair<std::string_view, std::pair<std::string, std::set<std::string_view>>>(key, std::pair<std::string, std::set<std::string_view>>(value, new_vars)));
  } else {
    fmt::print("statefull_set:\tinserting:\tkey:{}\n", key);

    std::string phrase = absl::StrCat(key);
    if (curr_module->get_variable_type(key) == 2) {
      phrase = absl::StrCat(phrase, "_next");
    }
    phrase = absl::StrCat(phrase, " = ", value, ";\n");

    new_vars.insert(key);

    curr_module->add_to_buffer_single(std::pair<int32_t, std::string>(indent_buffer_size, phrase), new_vars);
  }

  fmt::print("pure_assign value:\tkey: {}\tvalue: {}\n", key, value);
}

/*
void Lnast_to_verilog_parser::process_as() {
  std::vector<Lnast_node>::iterator it = node_buffer.begin();
  it++;
  node_str_buffer = absl::StrCat(node_str_buffer, "process_as:\t");
  std::string_view key = get_node_name(*it);
  it++;

  std::string value = "";
  std::map<std::string_view, std::string>::iterator map_it;
  std::string_view ref = get_node_name(*it);
  map_it = ref_map.find(ref);
  if (map_it != ref_map.end()) {
    ref = map_it->second;
  }
  value = absl::StrCat(value, ref);

  if (is_ref(key)) {
    fmt::print("inserting:\tkey:{}\tvalue:{}\n", key, value);
    ref_map.insert(std::pair<std::string_view, std::string>(key, value));
  }

  fmt::print("process_as value:\tkey: {}\tvalue: {}\n", key, value);

  node_str_buffer = absl::StrCat(node_str_buffer, key, " as " , value, "\n");
}
*/

void Lnast_to_verilog_parser::process_label() {
  std::string value = "";
  std::set<std::string_view> new_vars;

  std::vector<Lnast_node>::iterator it = node_buffer.begin();
  it++;
  std::string_view key = get_node_name(*it);
  it++;

  std::string_view ref = get_node_name(*it);
  std::map<std::string_view, std::pair<std::string, std::set<std::string_view>>>::iterator map_it;
  map_it = ref_map.find(ref);
  if (map_it != ref_map.end()) {
    ref = map_it->second.first;
    new_vars.insert(map_it->second.second.begin(), map_it->second.second.end());
  }
  it++;
  value = absl::StrCat(value, ref, ":", process_number(get_node_name(*it)));

  fmt::print("process_label value:\tkey: {}\tvalue: {}\n", key, value);
  if (is_ref(key)) {
    fmt::print("inserting:\tkey:{}\tvalue:{}\n", key, value);
    ref_map.insert(std::pair<std::string_view, std::pair<std::string, std::set<std::string_view>>>(key, std::pair<std::string, std::set<std::string_view>>(value, new_vars)));
  } else {
    // check what this is
    // node_str_buffer = absl::StrCat(node_str_buffer, key, " saved as ", value, "\n");
  }
  // TODO(joapena): check how labels are formated
  /*
  if (get_node_name(*it) == "__bits") {
    it++;

    std::string_view value = get_node_name(*it);
    // std::smatch m;
    // std::regex_search(value, m, std::regex("b|o|d|h"));
    value = process_number(value);

    node_str_buffer = absl::StrCat(node_str_buffer, "process_label:\t", key, " saved as ", value, "\n");
  }
  */
}

void Lnast_to_verilog_parser::process_operator() {
  std::string value = "";
  std::set<std::string_view> new_vars;

  std::vector<Lnast_node>::iterator it = node_buffer.begin();
  std::string op_type = ntype_dbg((*it).type);
  it++;
  std::string_view key = get_node_name(*it);
  it++;

  while (it != node_buffer.end()) {
    std::string_view ref = get_node_name(*it);
    std::map<std::string_view, std::pair<std::string, std::set<std::string_view>>>::iterator map_it;
    map_it = ref_map.find(ref);
    if (map_it != ref_map.end()) {
      ref = map_it->second.first;
      new_vars.insert(map_it->second.second.begin(), map_it->second.second.end());
      fmt::print("map_it find: {} | {}\n", map_it->first, map_it->second.first);
    } else if (ref.size() > 2 && !is_number(ref)) {
      new_vars.insert(ref);
    }
    // check if a number

    value = absl::StrCat(value, ref);
    if (++it != node_buffer.end()) {
      value = absl::StrCat(value, " ", op_type, " ");
    }
  }

  fmt::print("process_{} value:\tkey: {}\tvalue: {}\n", op_type, key, value);
  if (is_ref(key)) {
    fmt::print("inserting:\tkey:{}\tvalue:{}\n", key, value);
    ref_map.insert(std::pair<std::string_view, std::pair<std::string, std::set<std::string_view>>>(key, std::pair<std::string, std::set<std::string_view>>(value, new_vars)));
  } else {
    std::string phrase = absl::StrCat(key, " ", op_type,"  ", value, "\n");
    curr_module->add_to_buffer_single(std::pair<int32_t, std::string>(indent_buffer_size, phrase), new_vars);
  }
}

void Lnast_to_verilog_parser::process_if() {
  fmt::print("start process_if\n");
  std::vector<std::pair<int32_t, std::string>> new_nodes;
  std::set<std::string_view> new_vars;

  std::vector<Lnast_node>::iterator it = node_buffer.begin();
  it++; // if
  it++; // csts
  std::string_view ref = get_node_name(*it);
  std::map<std::string_view, std::pair<std::string, std::set<std::string_view>>>::iterator map_it;
  map_it = ref_map.find(ref);
  if (map_it != ref_map.end()) {
    ref = map_it->second.first;
    new_vars.insert(std::make_move_iterator(map_it->second.second.begin()), make_move_iterator(map_it->second.second.end()));
    fmt::print("map_it find: {} | {}\n", map_it->first, map_it->second.first);
  }
  new_nodes.push_back(std::pair<int32_t, std::string>(indent_buffer_size, absl::StrCat("if(", ref, ") {\n")));
  it++; // cond
  std::vector<std::pair<int32_t, std::string>> queue_nodes = curr_module->pop_queue();
  new_nodes.insert(new_nodes.end(), std::make_move_iterator(queue_nodes.begin()), std::make_move_iterator(queue_nodes.end()));
  new_nodes.push_back(std::pair<int32_t, std::string>(indent_buffer_size, "}"));
  it++; // sts

  while (it != node_buffer.end()) {
    // this is the elif
    if ((*it).type == Lnast_ntype_cstatements) {
      it++; // csts
      ref = get_node_name(*it);
      map_it = ref_map.find(ref);
      if (map_it != ref_map.end()) {
        ref = map_it->second.first;
        new_vars.insert(map_it->second.second.begin(), map_it->second.second.end());
        fmt::print("map_it find: {} | {}\n", map_it->first, map_it->second.first);
      }
      new_nodes.push_back(std::pair<int32_t, std::string>(0, absl::StrCat(" elif (", ref, ") {\n")));
      it++; // cond
      queue_nodes = curr_module->pop_queue();
      new_nodes.insert(new_nodes.end(), std::make_move_iterator(queue_nodes.begin()), std::make_move_iterator(queue_nodes.end()));
      new_nodes.push_back(std::pair<int32_t, std::string>(indent_buffer_size, "}"));
      it++; // sts
    }

    // this is the else
    else {
      new_nodes.push_back(std::pair<int32_t, std::string>(0, " else {\n"));
      queue_nodes = curr_module->pop_queue();
      new_nodes.insert(new_nodes.end(), std::make_move_iterator(queue_nodes.begin()), std::make_move_iterator(queue_nodes.end()));
      new_nodes.push_back(std::pair<int32_t, std::string>(indent_buffer_size, "}"));
      it++; // sts
    }
  }
  new_nodes.push_back(std::pair<int32_t, std::string>(indent_buffer_size, "\n"));

  curr_module->add_to_buffer_multiple(new_nodes, new_vars);
}

void Lnast_to_verilog_parser::process_func_call() {
  std::string value = "";
  std::set<std::string_view> new_vars;

  std::vector<Lnast_node>::iterator it = node_buffer.begin();
  it++; // func_call
  std::string_view key = get_node_name(*it);
  it++; // sts

  value = absl::StrCat(value, root_filename, "_",  get_node_name(*it), "(");
  it++; // ref
  while (it != node_buffer.end()) {
    std::string_view ref = get_node_name(*it);
    std::map<std::string_view, std::pair<std::string, std::set<std::string_view>>>::iterator map_it;
    map_it = ref_map.find(ref);
    if (map_it != ref_map.end()) {
      ref = map_it->second.first;
      new_vars.insert(map_it->second.second.begin(), map_it->second.second.end());
      fmt::print("map_it find: {} | {}\n", map_it->first, map_it->second.first);
    }

    value = absl::StrCat(value, ref);
    if (++it != node_buffer.end()) {
      value = absl::StrCat(value, ", ");
    } else {
      value = absl::StrCat(value, ")");
    }
  }

  fmt::print("process_func_call: value:\tkey: {}\tvalue: {}\n", key, value);
  if (is_ref(key)) {
    fmt::print("inserting:\tkey:{}\tvalue:{}\n", key, value);
    ref_map.insert(std::pair<std::string_view, std::pair<std::string, std::set<std::string_view>>>(key, std::pair<std::string, std::set<std::string_view>>(value, new_vars)));
  } else {
    // std::string phrase = absl::StrCat(key, " ", op_type,"  ", value, "\n");
    curr_module->add_to_buffer_single(std::pair<int32_t, std::string>(indent_buffer_size, value), new_vars);
  }
}

void Lnast_to_verilog_parser::process_func_def() {
  std::vector<Lnast_node>::iterator it = node_buffer.begin();
  it++; // func_def
  it++; // sts
  std::string func_name = absl::StrCat(root_filename, "_", get_node_name(*it));
  curr_module->filename = func_name;
  // function name
  it++; // ref
  // the variables
  std::set<std::string_view> new_vars;
  while (it != node_buffer.end()) {
    new_vars.insert(get_node_name(*it));
    it++;
  }

  curr_module->add_to_buffer_multiple(curr_module->pop_queue(), new_vars);

  file_map.insert(std::pair<std::string, std::string>(func_name, curr_module->create_file()));

  fmt::print("new module:\n{}", curr_module->create_file());

  curr_module = module_stack.back();
  module_stack.pop_back();
}

void Lnast_to_verilog_parser::setup_ntype_str_mapping() {
  ntype2str[Lnast_ntype_invalid] = "invalid";
  ntype2str[Lnast_ntype_statements] = "sts";
  ntype2str[Lnast_ntype_cstatements] = "csts";
  ntype2str[Lnast_ntype_pure_assign] = "=";
  ntype2str[Lnast_ntype_dp_assign] = ":=";
  ntype2str[Lnast_ntype_as] = "as";
  ntype2str[Lnast_ntype_label] = "label";
  ntype2str[Lnast_ntype_dot] = "dot";
  ntype2str[Lnast_ntype_logical_and] = "and";
  ntype2str[Lnast_ntype_logical_or] = "or";
  ntype2str[Lnast_ntype_and] = "&";
  ntype2str[Lnast_ntype_or] = "|";
  ntype2str[Lnast_ntype_xor] = "^";
  ntype2str[Lnast_ntype_plus] = "+";
  ntype2str[Lnast_ntype_minus] = "-";
  ntype2str[Lnast_ntype_mult] = "*";
  ntype2str[Lnast_ntype_div] = "/";
  ntype2str[Lnast_ntype_same] = "==";
  ntype2str[Lnast_ntype_lt] = "<";
  ntype2str[Lnast_ntype_le] = "<=";
  ntype2str[Lnast_ntype_gt] = ">";
  ntype2str[Lnast_ntype_ge] = ">=";
  ntype2str[Lnast_ntype_tuple] = "()";
  ntype2str[Lnast_ntype_ref] = "ref";
  ntype2str[Lnast_ntype_const] = "const";
  ntype2str[Lnast_ntype_attr_bits] = "attr_bits";
  ntype2str[Lnast_ntype_assert] = "I";
  ntype2str[Lnast_ntype_if] = "if";
  //ntype2str[Lnast_ntype_else] = "else";
  ntype2str[Lnast_ntype_cond] = "cond";
  ntype2str[Lnast_ntype_uif] = "uif";
  ntype2str[Lnast_ntype_for] = "for";
  ntype2str[Lnast_ntype_while] = "while";
  ntype2str[Lnast_ntype_func_call] = "func_call";
  ntype2str[Lnast_ntype_func_def] = "func_def";
  ntype2str[Lnast_ntype_top] = "top";
}

std::string Lnast_to_verilog_parser::ntype_dbg(Lnast_ntype_id ntype) {
  return ntype2str[ntype];
}

