// Key schema of the mads.ini node types: see ini_schema.hpp for the contract.
#include "ini_schema.hpp"

#include "ini_schema_data.hpp"

#include "../topology_graph.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

namespace Mads::Ini {

namespace {

// ---------------------------------------------------------------- parsing

// Reads a string field. Missing fields yield an empty string; present fields
// of the wrong type are a malformed schema.
std::string string_field(const toml::table &table, std::string_view key,
                         const std::string &where) {
  const toml::node *node = table.get(key);
  if (!node) {
    return {};
  }
  const auto *value = node->as_string();
  if (!value) {
    throw std::runtime_error(where + ": '" + std::string(key) +
                             "' must be a string");
  }
  return value->get();
}

bool bool_field(const toml::table &table, std::string_view key,
                const std::string &where) {
  const toml::node *node = table.get(key);
  if (!node) {
    return false;
  }
  const auto *value = node->as_boolean();
  if (!value) {
    throw std::runtime_error(where + ": '" + std::string(key) +
                             "' must be a boolean");
  }
  return value->get();
}

// Reads an array of strings (choices). Missing means empty.
std::vector<std::string> string_list_field(const toml::table &table,
                                           std::string_view key,
                                           const std::string &where) {
  std::vector<std::string> out;
  const toml::node *node = table.get(key);
  if (!node) {
    return out;
  }
  const auto *array = node->as_array();
  if (!array) {
    throw std::runtime_error(where + ": '" + std::string(key) +
                             "' must be an array of strings");
  }
  for (const auto &el : *array) {
    const auto *value = el.as_string();
    if (!value) {
      throw std::runtime_error(where + ": '" + std::string(key) +
                               "' must be an array of strings");
    }
    out.push_back(value->get());
  }
  return out;
}

// Reads one [[common]] or [[types.keys]] table.
KeySpec parse_key(const toml::table &table, const std::string &where) {
  KeySpec spec;
  spec.name = string_field(table, "name", where);
  if (spec.name.empty()) {
    throw std::runtime_error(where + ": key without a 'name'");
  }
  const std::string at = where + "." + spec.name;
  spec.type = value_type_from_string(
      table.get("type") ? string_field(table, "type", at) : "any");
  spec.default_raw = string_field(table, "default", at);
  spec.choices = string_list_field(table, "choices", at);
  spec.doc = string_field(table, "doc", at);
  spec.group = string_field(table, "group", at);
  spec.widget = string_field(table, "widget", at);
  spec.inherits = bool_field(table, "inherits", at);
  return spec;
}

// Reads one [[types]] table, including its [[types.keys]].
NodeType parse_type(const toml::table &table, const std::string &where) {
  NodeType type;
  type.id = string_field(table, "id", where);
  if (type.id.empty()) {
    throw std::runtime_error(where + ": type without an 'id'");
  }
  const std::string at = where + " '" + type.id + "'";
  type.label = string_field(table, "label", at);
  type.category = string_field(table, "category", at);
  if (type.label.empty() || type.category.empty()) {
    throw std::runtime_error(at + ": 'label' and 'category' are required");
  }
  type.executable = string_field(table, "executable", at);
  if (table.get("role")) {
    type.role = string_field(table, "role", at);
  }
  if (type.role != "source" && type.role != "filter" && type.role != "sink" &&
      type.role != "any") {
    throw std::runtime_error(at + ": unknown role '" + type.role + "'");
  }
  type.custom_keys = bool_field(table, "custom_keys", at);
  type.document = bool_field(table, "document", at);
  if (const toml::node *keys = table.get("keys")) {
    const auto *array = keys->as_array();
    if (!array) {
      throw std::runtime_error(at + ": 'keys' must be an array of tables");
    }
    for (const auto &el : *array) {
      const auto *key_table = el.as_table();
      if (!key_table) {
        throw std::runtime_error(at + ": 'keys' must be an array of tables");
      }
      type.keys.push_back(parse_key(*key_table, at));
    }
  }
  return type;
}

// ---------------------------------------------------------------- linting

// '#' is valid only as the whole last level of a sub_topic entry ("#" or
// "a/b/#"): anywhere else it is an invalid wildcard (see topic_match.hpp).
bool is_valid_wildcard(std::string_view entry) {
  const auto pos = entry.find('#');
  if (pos == std::string_view::npos) {
    return true;
  }
  if (pos != entry.size() - 1) {
    return false;
  }
  return pos == 0 || entry[pos - 1] == '/';
}

// Parses `raw` as a single TOML value. The node lives in `holder`, so it is
// valid while `holder` is; nullptr if `raw` is not a valid value.
const toml::node *parse_value(const std::string &raw, toml::table &holder) {
  try {
    holder = toml::parse("v = " + raw);
  } catch (const toml::parse_error &) {
    return nullptr;
  }
  return holder.get("v");
}

// A diagnostic about a whole section (not a single entry), with `key` set.
Diagnostic make_info(const std::string &section, const std::string &key,
                     std::string message) {
  Diagnostic d;
  d.severity = Diagnostic::Severity::Info;
  d.section = section;
  d.key = key;
  d.message = std::move(message);
  return d;
}

// Human name of a TOML node type, for messages.
std::string_view toml_type_name(const toml::node &node) {
  if (node.is_string()) return "string";
  if (node.is_integer()) return "integer";
  if (node.is_floating_point()) return "float";
  if (node.is_boolean()) return "boolean";
  if (node.is_array()) return "array";
  if (node.is_table()) return "table";
  return "value";
}

// Empty string when `node` matches `spec`, else a message describing why not.
std::string value_problem(const KeySpec &spec, const toml::node &node) {
  bool ok = true;
  switch (spec.type) {
  case ValueType::String:
    ok = node.is_string();
    break;
  case ValueType::Integer:
    ok = node.is_integer();
    break;
  case ValueType::Float:
    // An integer is accepted where a float is expected (e.g. 1 for 1.0).
    ok = node.is_floating_point() || node.is_integer();
    break;
  case ValueType::Boolean:
    ok = node.is_boolean();
    break;
  case ValueType::StringArray: {
    // A bare string is a one-element list: Agent::init() accepts
    // `sub_topic = "x"` as well as `sub_topic = ["x"]`.
    ok = node.is_array() || node.is_string();
    if (const auto *array = node.as_array()) {
      for (const auto &el : *array) {
        ok = ok && el.is_string();
      }
    }
    break;
  }
  case ValueType::Array:
    ok = node.is_array();
    break;
  case ValueType::Table:
    ok = node.is_table();
    break;
  case ValueType::Any:
    break;
  }
  if (!ok) {
    return "expected " + std::string(to_string(spec.type)) + ", found " +
           std::string(toml_type_name(node));
  }
  if (!spec.choices.empty() && node.is_string()) {
    const std::string value = node.as_string()->get();
    if (std::find(spec.choices.begin(), spec.choices.end(), value) ==
        spec.choices.end()) {
      std::string list;
      for (const auto &c : spec.choices) {
        list += (list.empty() ? "" : ", ") + c;
      }
      return "'" + value + "' is not one of: " + list;
    }
  }
  return {};
}

Diagnostic make_diagnostic(Diagnostic::Severity severity, const Section &section,
                           const Entry *entry, std::string message) {
  Diagnostic d;
  d.severity = severity;
  d.section = section.name;
  d.key = entry ? entry->key : std::string{};
  d.message = std::move(message);
  return d;
}

} // namespace

// ------------------------------------------------------------ value types

ValueType value_type_from_string(std::string_view name) {
  if (name == "string") return ValueType::String;
  if (name == "integer") return ValueType::Integer;
  if (name == "float") return ValueType::Float;
  if (name == "boolean") return ValueType::Boolean;
  if (name == "string_array") return ValueType::StringArray;
  if (name == "array") return ValueType::Array;
  if (name == "table") return ValueType::Table;
  return ValueType::Any;
}

std::string_view to_string(ValueType type) {
  switch (type) {
  case ValueType::String: return "string";
  case ValueType::Integer: return "integer";
  case ValueType::Float: return "float";
  case ValueType::Boolean: return "boolean";
  case ValueType::StringArray: return "string_array";
  case ValueType::Array: return "array";
  case ValueType::Table: return "table";
  case ValueType::Any: return "any";
  }
  return "any";
}

// ----------------------------------------------------------------- Schema

const Schema &Schema::builtin() {
  // Parsed once, on first use: the embedded text is a valid schema by
  // construction (the build fails on malformed data in the unit tests).
  static const Schema schema =
      Schema::parse(reinterpret_cast<const char *>(kIniSchemaData));
  return schema;
}

Schema Schema::parse(std::string_view toml_text) {
  toml::table root;
  try {
    root = toml::parse(toml_text);
  } catch (const toml::parse_error &e) {
    throw std::runtime_error(std::string("ini schema: ") +
                             std::string(e.description()));
  }

  Schema schema;
  if (const toml::node *common = root.get("common")) {
    const auto *array = common->as_array();
    if (!array) {
      throw std::runtime_error("ini schema: 'common' must be an array of tables");
    }
    for (const auto &el : *array) {
      const auto *table = el.as_table();
      if (!table) {
        throw std::runtime_error("ini schema: 'common' must be an array of tables");
      }
      schema._common_keys.push_back(parse_key(*table, "common"));
    }
  }

  const toml::node *types = root.get("types");
  const auto *types_array = types ? types->as_array() : nullptr;
  if (!types_array || types_array->empty()) {
    throw std::runtime_error("ini schema: no [[types]] defined");
  }
  std::set<std::string> ids;
  for (const auto &el : *types_array) {
    const auto *table = el.as_table();
    if (!table) {
      throw std::runtime_error("ini schema: 'types' must be an array of tables");
    }
    NodeType type = parse_type(*table, "ini schema");
    if (!ids.insert(type.id).second) {
      throw std::runtime_error("ini schema: duplicate type '" + type.id + "'");
    }
    schema._types.push_back(std::move(type));
  }
  return schema;
}

const NodeType *Schema::type(std::string_view id) const {
  for (const auto &type : _types) {
    if (type.id == id) {
      return &type;
    }
  }
  return nullptr;
}

const KeySpec *Schema::key(std::string_view type_id,
                           std::string_view key_name) const {
  const NodeType *node_type = type(type_id);
  if (!node_type) {
    return nullptr;
  }
  for (const auto &spec : node_type->keys) {
    if (spec.name == key_name) {
      return &spec;
    }
  }
  if (node_type->document) {
    return nullptr;
  }
  for (const auto &spec : _common_keys) {
    if (spec.name == key_name) {
      return &spec;
    }
  }
  return nullptr;
}

std::string Schema::infer_type(const Section &section) const {
  // 1. An explicit, known type in the editor metadata.
  if (!section.meta.type.empty() && type(section.meta.type)) {
    return section.meta.type;
  }
  // 2. The two document sections.
  if (section.name == "agents" || section.name == "broker") {
    return section.name;
  }
  // 3. A type named after the section, e.g. [logger].
  if (type(section.name)) {
    return section.name;
  }
  // 4. A plugin loader: it has an attachment and declares its topics.
  if (section.find("attachment")) {
    const bool pub = section.find("pub_topic") != nullptr;
    const bool sub = section.find("sub_topic") != nullptr;
    if (pub && sub) return "filter";
    if (pub) return "source";
    if (sub) return "sink";
  }
  // 5. Anything else is a custom agent.
  return "custom";
}

std::vector<Diagnostic> Schema::lint(const Document &doc) const {
  std::vector<Diagnostic> out;
  using Severity = Diagnostic::Severity;

  for (const auto &section : doc.sections()) {
    const std::string type_id = infer_type(section);
    const NodeType *node_type = type(type_id);
    if (!node_type) {
      continue;
    }
    for (const auto &entry : section.entries) {
      if (entry.disabled) {
        continue;
      }
      const KeySpec *spec = key(type_id, entry.key);
      if (!spec) {
        if (!node_type->custom_keys) {
          out.push_back(make_diagnostic(
              Severity::Warning, section, &entry,
              "unknown key '" + entry.key + "' for type '" + type_id + "'"));
        }
        continue;
      }

      toml::table holder;
      const toml::node *value = parse_value(entry.raw_value, holder);
      if (!value) {
        out.push_back(make_diagnostic(Severity::Error, section, &entry,
                                      "value is not valid TOML: " +
                                          entry.raw_value));
        continue;
      }
      const toml::node &node = *value;
      const std::string problem = value_problem(*spec, node);
      if (!problem.empty()) {
        out.push_back(make_diagnostic(Severity::Warning, section, &entry,
                                      "'" + entry.key + "': " + problem));
        continue;
      }
      // Wildcard entries in sub_topic: '#' must be the whole last level.
      if (entry.key == "sub_topic") {
        std::vector<std::string> patterns;
        if (const auto *array = node.as_array()) {
          for (const auto &el : *array) {
            patterns.push_back(el.value_or(std::string{}));
          }
        } else {
          patterns.push_back(node.value_or(std::string{}));
        }
        for (const auto &pattern : patterns) {
          if (!is_valid_wildcard(pattern)) {
            out.push_back(make_diagnostic(
                Severity::Warning, section, &entry,
                "invalid wildcard '" + pattern +
                    "': '#' must be the whole last level (e.g. 'a/#')"));
          }
        }
      }
    }
  }

  // Dangling topics, on the serialized document so that the declarations
  // match what an agent would read (disabled entries are gone by then).
  try {
    const toml::table config = toml::parse(doc.serialize());
    const auto infos = Mads::agent_topic_infos(config);
    const auto dangling = Mads::dangling_topics(infos);
    for (const auto &name : dangling.pub) {
      const auto it = infos.find(name);
      const std::string topic = it == infos.end() ? name : it->second.pub_topic;
      out.push_back(make_info(name, "pub_topic",
                              "pub_topic '" + topic +
                                  "' is not subscribed by any agent"));
    }
    for (const auto &[name, entries] : dangling.sub) {
      for (const auto &pattern : entries) {
        out.push_back(make_info(name, "sub_topic",
                                "sub_topic '" + pattern +
                                    "' is not published by any agent"));
      }
    }
  } catch (const toml::parse_error &e) {
    Diagnostic d;
    d.severity = Severity::Error;
    d.message = std::string("document is not valid TOML: ") +
                std::string(e.description());
    out.push_back(std::move(d));
  }
  return out;
}

} // namespace Mads::Ini
