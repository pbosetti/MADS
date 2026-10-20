/*
 ___       _   ____       _
|_ _|_ __ (_) / ___|  ___| |__   ___ _ __ ___   __ _
 | || '_ \| | \___ \ / __| '_ \ / _ \ '_ ` _ \ / _` |
 | || | | | |  ___) | (__| | | |  __/ | | | | | (_| |
|___|_| |_|_| |____/ \___|_| |_|\___|_| |_| |_|\__,_|

What the mads-ini editor knows about mads.ini: the node types it can create
(built-in agents, plugin loaders, custom agents), the keys each one reads,
and the two document sections [agents] and [broker]. The data lives in
ini_schema.toml, embedded in the binary at build time.

Author(s): Paolo Bosetti
*/
#pragma once

#include "ini_document.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace Mads::Ini {

enum class ValueType {
  String,
  Integer,
  Float,
  Boolean,
  StringArray, ///< e.g. sub_topic
  Array,       ///< any other array
  Table,       ///< inline table
  Any          ///< not checked
};

/// Parses "string", "integer", "float", "boolean", "string_array", "array",
/// "table", "any". Unknown names map to Any.
ValueType value_type_from_string(std::string_view name);
std::string_view to_string(ValueType type);

/// One documented key.
struct KeySpec {
  std::string name;
  ValueType type = ValueType::Any;
  std::string default_raw;          ///< TOML text of the default, may be empty
  std::vector<std::string> choices; ///< allowed string values, if an enum
  std::string doc;                  ///< one-line description
  std::string group;                ///< inspector group, e.g. "Topics"
  /// "file" (path picker), "topic" (single topic), "topics" (sub_topic
  /// list) or empty for the default widget of `type`.
  std::string widget;
  /// The key may also be set fleet-wide in [agents]; the section value
  /// overrides it.
  bool inherits = false;
};

/// A kind of section the editor can create.
struct NodeType {
  std::string id;         ///< stored in the `#@ type=` meta, e.g. "filter"
  std::string label;      ///< palette label, e.g. "Filter plugin"
  std::string executable; ///< what runs it, e.g. "mads-filter"
  std::string category;   ///< palette group, e.g. "Plugin loaders"
  /// "source", "filter", "sink" or "any": which pins the node shows.
  std::string role = "any";
  bool custom_keys = false; ///< arbitrary extra keys are legitimate
  bool document = false;    ///< [agents] / [broker]: no node, no common keys
  std::vector<KeySpec> keys; ///< own keys, in display order
};

class Schema {
public:
  /// The schema embedded at build time. Parsed once, on first use.
  static const Schema &builtin();
  /// Parses a schema TOML text; throws std::runtime_error on malformed data.
  static Schema parse(std::string_view toml_text);

  const std::vector<NodeType> &types() const { return _types; }
  const NodeType *type(std::string_view id) const;
  /// Keys every agent node accepts (pub_topic, sub_topic, queue_size, ...).
  const std::vector<KeySpec> &common_keys() const { return _common_keys; }
  /// The spec of `key` for a node of type `type_id`: the type's own keys
  /// first, then the common keys (not for document types). nullptr if none.
  const KeySpec *key(std::string_view type_id, std::string_view key) const;

  /// The node type of `section`:
  ///  1. `section.meta.type` if it names a known type;
  ///  2. "agents"/"broker" for those section names;
  ///  3. a type whose id equals the section name (e.g. [logger]);
  ///  4. with an `attachment` key: "source", "filter" or "sink" from the
  ///     active pub_topic/sub_topic keys (pub only, both, sub only);
  ///  5. "custom".
  std::string infer_type(const Section &section) const;

  /// Schema-level problems of `doc`, as Warnings (Errors for invalid TOML):
  ///  - unknown keys in sections whose type has custom_keys == false;
  ///  - values whose TOML type does not match the KeySpec type, or a string
  ///    not among `choices`;
  ///  - sub_topic entries that are invalid wildcard patterns ('#' not last);
  ///  - dangling topics (Info), as reported by Mads::dangling_topics().
  /// Disabled entries are not checked.
  std::vector<Diagnostic> lint(const Document &doc) const;

private:
  std::vector<NodeType> _types;
  std::vector<KeySpec> _common_keys;
};

} // namespace Mads::Ini
