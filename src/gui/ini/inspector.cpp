#include "inspector.hpp"

#include "gui_style.hpp"
#include "ini_document.hpp"

#include <imgui.h>

#include <portable-file-dialogs.h>

#include <toml++/toml.hpp>

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace MadsIni {

using Mads::Ini::Document;
using Mads::Ini::Entry;
using Mads::Ini::KeySpec;
using Mads::Ini::NodeType;
using Mads::Ini::Section;
using Mads::Ini::ValueType;

struct Inspector::KeyRow {
  std::string name;
  const KeySpec *spec = nullptr; ///< nullptr for a key outside the schema
};

namespace {

constexpr ImVec4 kWarningColor{0.95F, 0.75F, 0.30F, 1.0F};
constexpr ImVec4 kInvalidFrame{0.45F, 0.12F, 0.12F, 1.0F};
constexpr const char *kDocumentSections[] = {"agents", "broker"};
constexpr const char *kKeyTypes[] = {"string", "integer", "float", "boolean",
                                     "array", "raw TOML"};
constexpr const char *kSectionPlaceholder = "<section name>";

// Escapes `text` as a TOML basic string, quotes included.
std::string quote(std::string_view text) {
  std::string out = "\"";
  for (const char raw : text) {
    const auto c = static_cast<unsigned char>(raw);
    switch (c) {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\t':
      out += "\\t";
      break;
    case '\r':
      out += "\\r";
      break;
    default:
      if (c < 0x20 || c == 0x7F) {
        char escaped[8];
        std::snprintf(escaped, sizeof escaped, "\\u%04X", c);
        out += escaped;
      } else {
        out += raw;
      }
    }
  }
  return out + "\"";
}

// Shortest decimal text that reads back as `value`; always a TOML float
// (integer-looking results get ".0", nan and inf are kept as they are).
std::string format_float(double value) {
  char buffer[64] = {};
  for (int precision = 1; precision <= 17; ++precision) {
    std::snprintf(buffer, sizeof buffer, "%.*g", precision, value);
    if (std::strtod(buffer, nullptr) == value) {
      break;
    }
  }
  std::string text = buffer;
  if (text.find_first_of(".eEn") == std::string::npos) {
    text += ".0";
  }
  return text;
}

std::string replace_all(std::string text, std::string_view from,
                        std::string_view to) {
  for (std::size_t pos = text.find(from); pos != std::string::npos;
       pos = text.find(from, pos + to.size())) {
    text.replace(pos, from.size(), to);
  }
  return text;
}

// The TOML value of `raw`, read through a one-key table under "v".
std::optional<toml::table> parse_value(std::string_view raw) {
  if (!Mads::Ini::is_valid_value(raw)) {
    return std::nullopt;
  }
  try {
    return toml::parse("v = " + std::string(raw));
  } catch (const std::exception &) {
    return std::nullopt;
  }
}

std::optional<double> as_number(const toml::node *node) {
  if (const auto number = node->value<double>()) {
    return *number;
  }
  if (const auto integer = node->value<int64_t>()) {
    return static_cast<double>(*integer);
  }
  return std::nullopt;
}

// The strings of a string_array value. A bare string is one element.
std::optional<std::vector<std::string>> string_elements(const toml::node *node) {
  if (const auto text = node->value<std::string>()) {
    return std::vector<std::string>{*text};
  }
  const toml::array *array = node->as_array();
  if (array == nullptr) {
    return std::nullopt;
  }
  std::vector<std::string> items;
  for (const toml::node &element : *array) {
    const auto text = element.value<std::string>();
    if (!text) {
      return std::nullopt;
    }
    items.push_back(*text);
  }
  return items;
}

std::string array_text(const std::vector<std::string> &items) {
  std::string text = "[";
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i > 0) {
      text += ", ";
    }
    text += quote(items[i]);
  }
  return text + "]";
}

// Raw TOML written for a key that is enabled with no value yet.
std::string default_text(const KeySpec *spec, const std::string &section) {
  if (spec != nullptr && !spec->default_raw.empty()) {
    return replace_all(spec->default_raw, kSectionPlaceholder, section);
  }
  const ValueType type = spec != nullptr ? spec->type : ValueType::Any;
  switch (type) {
  case ValueType::Integer:
    return "0";
  case ValueType::Float:
    return "0.0";
  case ValueType::Boolean:
    return "false";
  case ValueType::StringArray:
  case ValueType::Array:
    return "[]";
  case ValueType::Table:
    return "{}";
  case ValueType::String:
  case ValueType::Any:
    break;
  }
  return "\"\"";
}

// Hover text of a key row: documentation, default, choices, inheritance.
std::string key_tooltip(Document &doc, const std::string &key,
                        const KeySpec *spec) {
  if (spec == nullptr) {
    return "Not in the schema";
  }
  std::string tip = spec->doc;
  const auto add_line = [&tip](const std::string &line) {
    if (!tip.empty()) {
      tip += '\n';
    }
    tip += line;
  };
  if (!spec->default_raw.empty()) {
    add_line("default: " + spec->default_raw);
  }
  if (!spec->choices.empty()) {
    std::string choices = "choices: ";
    for (std::size_t i = 0; i < spec->choices.size(); ++i) {
      choices += (i > 0 ? ", " : "") + spec->choices[i];
    }
    add_line(choices);
  }
  if (spec->inherits) {
    const auto fleet = doc.effective_raw_value("agents", key);
    add_line("from [agents]: " + fleet.value_or("(not set)"));
  }
  return tip;
}

// ImGui edits a char buffer: the string is passed with its spare capacity
// and grows through this callback.
int grow_string_callback(ImGuiInputTextCallbackData *data) {
  if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
    auto *text = static_cast<std::string *>(data->UserData);
    text->resize(static_cast<std::size_t>(data->BufTextLen));
    data->Buf = text->data();
  }
  return 0;
}

// InputText on a std::string (imgui_stdlib is not part of the build). ImGui
// NUL-terminates what it wrote, so the string is trimmed to that NUL: this
// also covers deletions, which do not trigger the callback.
bool edit_text(std::string &text, const char *label, const char *hint,
               bool multiline, ImVec2 size) {
  const ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackResize;
  const std::size_t room = text.capacity() + 1;
  char *buffer = text.data();
  bool changed = false;
  if (multiline) {
    changed = ImGui::InputTextMultiline(label, buffer, room, size, flags,
                                        grow_string_callback, &text);
  } else if (hint != nullptr) {
    changed = ImGui::InputTextWithHint(label, hint, buffer, room, flags,
                                       grow_string_callback, &text);
  } else {
    changed = ImGui::InputText(label, buffer, room, flags,
                               grow_string_callback, &text);
  }
  text.resize(std::strlen(text.c_str()));
  return changed;
}

std::size_t count_lines(const std::string &text) {
  return static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')) +
         1;
}

} // namespace

Inspector::Inspector(Mads::Ini::Editor &editor, UiState &ui)
    : _editor(editor), _ui(ui) {}

void Inspector::draw() {
  _editing_seen = false;
  // Copied: renaming the node changes _ui.selected in the middle of the frame.
  const std::string selected = _ui.selected;
  if (is_node(selected)) {
    draw_node(selected);
  } else {
    draw_document();
  }
  // The edited field was not drawn (its section went away): forget it.
  if (!_editing_seen) {
    _editing.clear();
  }
}

bool Inspector::is_node(const std::string &name) const {
  const auto &nodes = _editor.nodes();
  return std::find(nodes.begin(), nodes.end(), name) != nodes.end();
}

// ------------------------------------------------------------ text fields

std::string &Inspector::buffer_for(const std::string &id,
                                   const std::string &model) {
  std::string &buffer = _buffers[id];
  if (id == _editing) {
    _editing_seen = true;
  } else {
    buffer = model;
  }
  return buffer;
}

std::optional<std::string> Inspector::text_input(const char *label,
                                                 const std::string &id,
                                                 const std::string &model,
                                                 const TextOptions &options) {
  std::string &buffer = buffer_for(id, model);
  const bool invalid = options.check_value && !Mads::Ini::is_valid_value(buffer);
  if (invalid) {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kInvalidFrame);
  }
  const float width = options.width != 0.0F ? options.width : -FLT_MIN;
  const float height = options.height > 0.0F
                           ? options.height
                           : ImGui::GetTextLineHeight() * 4.0F +
                                 ImGui::GetStyle().FramePadding.y * 2.0F;
  ImGui::SetNextItemWidth(width);
  edit_text(buffer, label, options.hint, options.multiline,
            ImVec2(width, height));
  if (invalid) {
    ImGui::PopStyleColor();
  }

  // Only the field that is active keeps its buffer across frames.
  if (ImGui::IsItemActive()) {
    _editing = id;
  } else if (_editing == id) {
    _editing.clear();
  }
  if (!ImGui::IsItemDeactivatedAfterEdit() || invalid || buffer == model) {
    return std::nullopt;
  }
  return buffer;
}

void Inspector::apply_raw(const std::string &section, const std::string &key,
                          const std::string &raw) {
  if (_editor.doc().set_raw_value(section, key, raw)) {
    _editor.commit("Edit " + section + "." + key);
  } else {
    _ui.notify("Invalid value for " + key + ": " + raw, true);
  }
}

// ------------------------------------------------------------ node header

void Inspector::draw_node(const std::string &name) {
  if (draw_node_header(name)) {
    return;
  }
  draw_keys(name, _editor.type_of(name));
}

bool Inspector::draw_node_header(const std::string &name) {
  Section *sec = _editor.doc().find(name);
  if (sec == nullptr) {
    return false;
  }
  ImGui::PushID("header");

  ImGui::TextDisabled("Name");
  if (const auto renamed = text_input("##name", name + "/@name", name, {})) {
    if (!Mads::Ini::is_bare_key(*renamed)) {
      _ui.notify("Invalid name \"" + *renamed +
                     "\": use letters, digits, '_' or '-'",
                 true);
    } else if (_editor.doc().find(*renamed) != nullptr) {
      _ui.notify("[" + *renamed + "] already exists", true);
    } else if (_editor.rename_node(name, *renamed)) {
      _ui.selected = *renamed;
      ImGui::PopID();
      return true;
    } else {
      _ui.notify("Cannot rename [" + name + "]", true);
    }
  }

  ImGui::TextDisabled("Type");
  const std::string type_id = _editor.type_of(name);
  const NodeType *type = _editor.schema().type(type_id);
  const std::string preview = type != nullptr ? type->label : type_id;
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::BeginCombo("##type", preview.c_str())) {
    for (const NodeType &candidate : _editor.schema().types()) {
      if (candidate.document) {
        continue;
      }
      std::string item = candidate.label;
      if (!candidate.executable.empty()) {
        item += " (" + candidate.executable + ")";
      }
      const bool selected = candidate.id == type_id;
      if (ImGui::Selectable(item.c_str(), selected) && !selected) {
        sec->meta.type = candidate.id;
        _editor.commit("Change type");
      }
    }
    ImGui::EndCombo();
  }
  if (type != nullptr && !type->executable.empty()) {
    ImGui::TextDisabled("%s", type->executable.c_str());
  }

  for (const auto &diagnostic : _editor.diagnostics()) {
    if (diagnostic.section != name) {
      continue;
    }
    std::string text;
    if (!diagnostic.key.empty()) {
      text = diagnostic.key + ": ";
    }
    text += diagnostic.message;
    if (diagnostic.line > 0) {
      text += " (line " + std::to_string(diagnostic.line) + ")";
    }
    ImVec4 color = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    if (diagnostic.severity == Mads::Ini::Diagnostic::Severity::Error) {
      color = MadsGui::kErrorColor;
    } else if (diagnostic.severity == Mads::Ini::Diagnostic::Severity::Warning) {
      color = kWarningColor;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
  }

  ImGui::TextDisabled("Comment (written under [%s])", name.c_str());
  TextOptions comment_options;
  comment_options.multiline = true;
  if (const auto edited =
          text_input("##comment", name + "/@comment", sec->comment,
                     comment_options)) {
    sec->comment = *edited;
    _editor.commit("Edit comment");
  }
  ImGui::Spacing();

  ImGui::PopID();
  return false;
}

// ---------------------------------------------------------- document view

void Inspector::draw_document() {
  ImGui::TextDisabled("Settings of the whole file. Select a node on the canvas "
                      "to edit it.");
  if (!ImGui::BeginTabBar("document_tabs")) {
    return;
  }
  for (const char *name : kDocumentSections) {
    const std::string label = std::string("[") + name + "]";
    if (!ImGui::BeginTabItem(label.c_str())) {
      continue;
    }
    ImGui::PushID(name);
    if (_editor.doc().find(name) == nullptr) {
      ImGui::TextDisabled("%s is not in the file.", label.c_str());
      if (ImGui::Button(("Create " + label).c_str())) {
        if (_editor.doc().add_section(name) != nullptr) {
          _editor.commit("Add " + label);
        } else {
          _ui.notify("Cannot create " + label, true);
        }
      }
    } else {
      draw_keys(name, name);
    }
    ImGui::PopID();
    ImGui::EndTabItem();
  }
  ImGui::EndTabBar();
}

// ------------------------------------------------------------- keys view

void Inspector::draw_keys(const std::string &section,
                          const std::string &type_id) {
  if (_editor.doc().find(section) == nullptr) {
    return;
  }
  const Mads::Ini::Schema &schema = _editor.schema();
  const NodeType *type = schema.type(type_id);
  const bool document = type != nullptr && type->document;

  // Schema keys grouped in first-appearance order; the inherited ones of a
  // node go to their own header instead.
  std::vector<std::pair<std::string, std::vector<KeyRow>>> groups;
  std::vector<KeyRow> overrides;
  const auto add = [&](const KeySpec &spec) {
    if (!document && spec.inherits) {
      overrides.push_back({spec.name, &spec});
      return;
    }
    const std::string group = spec.group.empty() ? "General" : spec.group;
    auto found = std::find_if(groups.begin(), groups.end(),
                              [&](const auto &entry) {
                                return entry.first == group;
                              });
    if (found == groups.end()) {
      groups.emplace_back(group, std::vector<KeyRow>{});
      found = std::prev(groups.end());
    }
    found->second.push_back({spec.name, &spec});
  };
  if (type != nullptr) {
    for (const KeySpec &spec : type->keys) {
      add(spec);
    }
  }
  if (!document) {
    for (const KeySpec &spec : schema.common_keys()) {
      const bool own = type != nullptr &&
                       std::any_of(type->keys.begin(), type->keys.end(),
                                   [&](const KeySpec &key) {
                                     return key.name == spec.name;
                                   });
      if (!own) {
        add(spec);
      }
    }
  }

  ImGui::PushID(section.c_str());
  for (std::size_t g = 0; g < groups.size(); ++g) {
    ImGui::SeparatorText(groups[g].first.c_str());
    draw_table(section, groups[g].second, std::to_string(g).c_str());
  }
  if (!overrides.empty() && ImGui::CollapsingHeader("Overrides of [agents]")) {
    draw_table(section, overrides, "overrides");
  }
  draw_other_keys(section, type_id, type != nullptr && type->custom_keys);
  draw_sub_tables(section);
  ImGui::PopID();
}

void Inspector::draw_table(const std::string &section,
                           const std::vector<KeyRow> &rows, const char *id) {
  float name_width = 0.0F;
  for (const KeyRow &row : rows) {
    name_width = std::max(name_width, ImGui::CalcTextSize(row.name.c_str()).x);
  }
  const float padding = ImGui::GetStyle().CellPadding.x * 2.0F;
  const std::string table_id = std::string("keys##") + id;
  if (!ImGui::BeginTable(table_id.c_str(), 4,
                         ImGuiTableFlags_SizingStretchProp)) {
    return;
  }
  ImGui::TableSetupColumn("enabled", ImGuiTableColumnFlags_WidthFixed,
                          ImGui::GetFrameHeight() + padding);
  ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed,
                          name_width + padding);
  ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.0F);
  ImGui::TableSetupColumn("comment", ImGuiTableColumnFlags_WidthFixed,
                          ImGui::GetFontSize() * 6.0F);
  for (const KeyRow &row : rows) {
    draw_key_row(section, row);
  }
  ImGui::EndTable();
}

void Inspector::draw_key_row(const std::string &section, const KeyRow &row) {
  const Section *sec = _editor.doc().find(section);
  if (sec == nullptr) {
    return;
  }
  const std::string &key = row.name;
  const KeySpec *spec = row.spec;
  // Copies: the entries move when a key is removed, so the row looks them up
  // again after any change.
  std::optional<Entry> active;
  std::optional<Entry> disabled;
  if (const Entry *entry = sec->find(key)) {
    active = *entry;
  }
  if (const Entry *entry = sec->find_disabled(key)) {
    disabled = *entry;
  }
  const bool has_entry = active.has_value() || disabled.has_value();
  const float x_width = ImGui::CalcTextSize("x").x +
                        ImGui::GetStyle().FramePadding.x * 2.0F +
                        ImGui::GetStyle().ItemSpacing.x;

  ImGui::PushID(key.c_str());
  ImGui::TableNextRow();

  ImGui::TableNextColumn();
  bool enabled = active.has_value();
  if (ImGui::Checkbox("##enabled", &enabled)) {
    bool done = false;
    if (!enabled) {
      done = _editor.doc().set_enabled(section, key, false);
    } else if (disabled.has_value()) {
      done = _editor.doc().set_enabled(section, key, true);
    } else {
      done = _editor.doc().set_raw_value(section, key,
                                         default_text(spec, section));
    }
    if (done) {
      _editor.commit(std::string(enabled ? "Enable " : "Disable ") + section +
                     "." + key);
    } else {
      _ui.notify("Cannot change " + section + "." + key, true);
    }
  }

  ImGui::TableNextColumn();
  if (active.has_value()) {
    ImGui::TextUnformatted(key.c_str());
  } else {
    ImGui::TextDisabled("%s", key.c_str());
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
    const std::string tip = key_tooltip(_editor.doc(), key, spec);
    if (!tip.empty()) {
      ImGui::SetTooltip("%s", tip.c_str());
    }
  }

  ImGui::TableNextColumn();
  std::string shown = default_text(spec, section);
  if (active.has_value()) {
    shown = active->raw_value;
  } else if (disabled.has_value()) {
    shown = disabled->raw_value;
  }
  ImGui::PushID("value");
  ImGui::BeginDisabled(!active.has_value());
  const auto applied = draw_value(section, key, spec, shown);
  ImGui::EndDisabled();
  ImGui::PopID();
  if (applied) {
    apply_raw(section, key, *applied);
  }

  ImGui::TableNextColumn();
  std::string comment;
  if (active.has_value()) {
    comment = active->inline_comment;
  } else if (disabled.has_value()) {
    comment = disabled->inline_comment;
  }
  ImGui::PushID("comment");
  TextOptions options;
  options.width = -x_width;
  options.hint = "comment";
  ImGui::BeginDisabled(!has_entry);
  const auto edited = text_input("##comment", section + "/" + key + "/comment",
                                 comment, options);
  ImGui::EndDisabled();
  ImGui::PopID();
  if (edited) {
    Section *now = _editor.doc().find(section);
    Entry *target = now != nullptr ? now->find(key) : nullptr;
    if (target == nullptr && now != nullptr) {
      target = now->find_disabled(key);
    }
    if (target != nullptr) {
      target->inline_comment = *edited;
      _editor.commit("Edit comment");
    }
  }
  if (has_entry) {
    ImGui::SameLine();
    if (ImGui::SmallButton("x") && _editor.doc().remove_key(section, key)) {
      _editor.commit("Remove " + section + "." + key);
    }
  }
  ImGui::PopID();
}

std::optional<std::string> Inspector::draw_value(const std::string &section,
                                                 const std::string &key,
                                                 const KeySpec *spec,
                                                 const std::string &shown) {
  const ValueType type = spec != nullptr ? spec->type : ValueType::Any;
  const std::string field = section + "/" + key + "/value";
  const auto parsed = parse_value(shown);
  const toml::node *node = parsed ? parsed->get("v") : nullptr;
  if (node == nullptr) {
    return draw_raw(field, shown);
  }

  switch (type) {
  case ValueType::Boolean:
    if (const auto flag = node->value<bool>()) {
      bool checked = *flag;
      if (ImGui::Checkbox("##value", &checked)) {
        return std::string(checked ? "true" : "false");
      }
      return std::nullopt;
    }
    break;
  case ValueType::Integer:
    if (const auto integer = node->value<int64_t>()) {
      int64_t number = *integer;
      ImGui::SetNextItemWidth(-FLT_MIN);
      ImGui::InputScalar("##value", ImGuiDataType_S64, &number);
      if (ImGui::IsItemDeactivatedAfterEdit() && number != *integer) {
        return std::to_string(number);
      }
      return std::nullopt;
    }
    break;
  case ValueType::Float:
    if (const auto number = as_number(node)) {
      double value = *number;
      ImGui::SetNextItemWidth(-FLT_MIN);
      ImGui::InputDouble("##value", &value, 0.0, 0.0, "%g");
      if (ImGui::IsItemDeactivatedAfterEdit() && value != *number) {
        return format_float(value);
      }
      return std::nullopt;
    }
    break;
  case ValueType::String:
    if (const auto text = node->value<std::string>()) {
      if (spec != nullptr && !spec->choices.empty()) {
        std::optional<std::string> chosen;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##value", text->c_str())) {
          for (const std::string &choice : spec->choices) {
            if (ImGui::Selectable(choice.c_str(), choice == *text) &&
                choice != *text) {
              chosen = quote(choice);
            }
          }
          ImGui::EndCombo();
        }
        return chosen;
      }
      if (spec != nullptr && spec->widget == "file") {
        const float button = ImGui::CalcTextSize("...").x +
                             ImGui::GetStyle().FramePadding.x * 2.0F;
        std::optional<std::string> chosen;
        TextOptions options;
        options.width = -(button + ImGui::GetStyle().ItemSpacing.x);
        if (const auto edited = text_input("##value", field, *text, options)) {
          chosen = quote(*edited);
        }
        ImGui::SameLine();
        if (ImGui::Button("...")) {
          const std::string start =
              std::filesystem::path(*text).parent_path().string();
          const auto picked =
              pfd::open_file("Choose file", start,
                             std::vector<std::string>{"Plugin files",
                                                      "*.plugin", "All files",
                                                      "*"},
                             pfd::opt::none)
                  .result();
          if (!picked.empty() && picked.front() != *text) {
            chosen = quote(picked.front());
          }
        }
        return chosen;
      }
      if (const auto edited = text_input("##value", field, *text, {})) {
        return quote(*edited);
      }
      return std::nullopt;
    }
    break;
  case ValueType::StringArray:
    if (const auto items = string_elements(node)) {
      return draw_string_list(field, *items);
    }
    break;
  case ValueType::Array:
  case ValueType::Table:
  case ValueType::Any:
    break;
  }
  return draw_raw(field, shown);
}

std::optional<std::string> Inspector::draw_raw(const std::string &field,
                                               const std::string &shown) {
  TextOptions options;
  options.check_value = true;
  options.multiline = shown.find('\n') != std::string::npos;
  return text_input("##value", field, shown, options);
}

std::optional<std::string>
Inspector::draw_string_list(const std::string &field,
                            std::vector<std::string> items) {
  const float button = ImGui::CalcTextSize("-").x +
                       ImGui::GetStyle().FramePadding.x * 2.0F;
  const float spacing = ImGui::GetStyle().ItemSpacing.x;
  bool changed = false;
  for (std::size_t i = 0; i < items.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    TextOptions options;
    options.width = -(button + spacing);
    if (const auto text = text_input("##element", field + "/" + std::to_string(i),
                                     items[i], options)) {
      items[i] = *text;
      changed = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("-")) {
      items.erase(items.begin() + static_cast<std::ptrdiff_t>(i));
      changed = true;
      ImGui::PopID();
      break;
    }
    ImGui::PopID();
  }
  if (ImGui::SmallButton("+")) {
    items.emplace_back();
    changed = true;
  }
  if (!changed) {
    return std::nullopt;
  }
  return array_text(items);
}

void Inspector::draw_other_keys(const std::string &section,
                                const std::string &type_id, bool custom_keys) {
  const Section *sec = _editor.doc().find(section);
  if (sec == nullptr) {
    return;
  }
  std::vector<KeyRow> rows;
  for (const Entry &entry : sec->entries) {
    if (_editor.schema().key(type_id, entry.key) != nullptr) {
      continue;
    }
    const bool seen = std::any_of(rows.begin(), rows.end(), [&](const KeyRow &row) {
      return row.name == entry.key;
    });
    if (!seen) {
      rows.push_back({entry.key, nullptr});
    }
  }
  if (rows.empty() && !custom_keys) {
    return;
  }

  if (!custom_keys) {
    ImGui::PushStyleColor(ImGuiCol_Text, kWarningColor);
  }
  const bool open = ImGui::CollapsingHeader(
      custom_keys ? "Custom keys" : "Unknown keys",
      ImGuiTreeNodeFlags_DefaultOpen);
  if (!custom_keys) {
    ImGui::PopStyleColor();
  }
  if (!open) {
    return;
  }
  ImGui::TextDisabled(custom_keys
                          ? "Keys outside the schema of this agent type."
                          : "Not in the schema: the agent ignores these keys.");
  if (!rows.empty()) {
    draw_table(section, rows, "other");
  }
  if (custom_keys) {
    draw_add_key(section);
  }
}

void Inspector::draw_add_key(const std::string &section) {
  ImGui::PushID("add_key");
  ImGui::TextDisabled("Add key");
  const float spacing = ImGui::GetStyle().ItemSpacing.x;
  const float button = ImGui::CalcTextSize("Add").x +
                       ImGui::GetStyle().FramePadding.x * 2.0F;
  const float half = ImGui::GetContentRegionAvail().x * 0.5F - spacing * 0.5F;
  ImGui::SetNextItemWidth(half);
  edit_text(_new_key, "##name", "name", false, ImVec2(half, 0.0F));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(-FLT_MIN);
  ImGui::Combo("##type", &_new_type, kKeyTypes,
               static_cast<int>(std::size(kKeyTypes)));
  ImGui::SetNextItemWidth(-(button + spacing));
  edit_text(_new_value, "##value", "value", false, ImVec2(0.0F, 0.0F));
  ImGui::SameLine();
  if (ImGui::Button("Add")) {
    add_key(section);
  }
  ImGui::PopID();
}

void Inspector::add_key(const std::string &section) {
  const Section *sec = _editor.doc().find(section);
  if (sec == nullptr) {
    return;
  }
  const std::string name = _new_key;
  if (!Mads::Ini::is_bare_key(name)) {
    _ui.notify("Invalid key name \"" + name +
                   "\": use letters, digits, '_' or '-'",
               true);
    return;
  }
  if (sec->find(name) != nullptr || sec->find_disabled(name) != nullptr) {
    _ui.notify("[" + section + "] already has " + name, true);
    return;
  }
  const bool as_string = _new_type == 0;
  const std::string raw = as_string ? quote(_new_value) : _new_value;
  if (!as_string) {
    const auto parsed = parse_value(raw);
    const toml::node *node = parsed ? parsed->get("v") : nullptr;
    bool matches = node != nullptr;
    if (node != nullptr) {
      switch (_new_type) {
      case 1:
        matches = node->is_integer();
        break;
      case 2:
        matches = node->is_floating_point();
        break;
      case 3:
        matches = node->is_boolean();
        break;
      case 4:
        matches = node->is_array();
        break;
      default:
        break;
      }
    }
    if (!matches) {
      _ui.notify(std::string("Not a valid ") + kKeyTypes[_new_type] +
                     " value: " + raw,
                 true);
      return;
    }
  }
  if (!_editor.doc().set_raw_value(section, name, raw)) {
    _ui.notify("Invalid value for " + name + ": " + raw, true);
    return;
  }
  _editor.commit("Add key");
  _new_key.clear();
  _new_value.clear();
}

void Inspector::draw_sub_tables(const std::string &section) {
  const Section *sec = _editor.doc().find(section);
  if (sec == nullptr || sec->raw_children.empty()) {
    return;
  }
  if (!ImGui::CollapsingHeader("Sub-tables (raw TOML)")) {
    return;
  }
  const std::size_t count = sec->raw_children.size();
  for (std::size_t i = 0; i < count; ++i) {
    const std::string model = sec->raw_children[i];
    ImGui::PushID(static_cast<int>(i));
    TextOptions options;
    options.multiline = true;
    options.height = ImGui::GetTextLineHeight() *
                         static_cast<float>(count_lines(model) + 1) +
                     ImGui::GetStyle().FramePadding.y * 2.0F;
    if (const auto edited = text_input("##child",
                                       section + "/child/" + std::to_string(i),
                                       model, options)) {
      Section *now = _editor.doc().find(section);
      if (now != nullptr && i < now->raw_children.size()) {
        const std::string previous = now->raw_children[i];
        now->raw_children[i] = *edited;
        const auto errors = _editor.doc().validate();
        if (errors.empty()) {
          _editor.commit("Edit sub-table");
        } else {
          now->raw_children[i] = previous;
          _ui.notify("invalid TOML: " + errors.front().message + " (line " +
                         std::to_string(errors.front().line) + ")",
                     true);
        }
      }
    }
    ImGui::PopID();
  }
}

} // namespace MadsIni
