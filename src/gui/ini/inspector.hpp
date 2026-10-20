// Right-hand panel of the mads-ini editor: every setting of the selected
// section (or of [agents]/[broker] when nothing is selected).
#pragma once

#include "ui_state.hpp"

#include "ini_editor.hpp"
#include "ini_schema.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace MadsIni {

class Inspector {
public:
  Inspector(Mads::Ini::Editor &editor, UiState &ui);
  /// Draws into the current content region.
  void draw();

private:
  /// Layout of one text field drawn by text_input().
  struct TextOptions {
    float width = 0.0F;         ///< 0 = fill the row
    float height = 0.0F;        ///< multiline only; 0 = four lines
    bool multiline = false;
    bool check_value = false;   ///< apply only text that is_valid_value()
    const char *hint = nullptr; ///< shown while the text is empty
  };
  /// One key of a keys table (see draw_table()).
  struct KeyRow;

  bool is_node(const std::string &name) const;
  void draw_node(const std::string &name);
  /// Name, type, diagnostics and comment of a node. True when the node was
  /// renamed: the caller must then stop, the section pointers are stale.
  bool draw_node_header(const std::string &name);
  void draw_document();
  /// Every key of `section` (typed as `type_id`), grouped, then the other
  /// keys, the add-key row and the sub-tables.
  void draw_keys(const std::string &section, const std::string &type_id);
  void draw_table(const std::string &section, const std::vector<KeyRow> &rows,
                  const char *id);
  void draw_key_row(const std::string &section, const KeyRow &row);
  void draw_other_keys(const std::string &section, const std::string &type_id,
                       bool custom_keys);
  void draw_add_key(const std::string &section);
  void add_key(const std::string &section);
  void draw_sub_tables(const std::string &section);
  /// The value widget of a key, by its schema type. Returns the new raw TOML
  /// when the user finished an edit.
  std::optional<std::string> draw_value(const std::string &section,
                                        const std::string &key,
                                        const Mads::Ini::KeySpec *spec,
                                        const std::string &shown);
  std::optional<std::string> draw_raw(const std::string &field,
                                      const std::string &shown);
  std::optional<std::string> draw_string_list(const std::string &field,
                                              std::vector<std::string> items);
  /// Text field whose text lives in an edit buffer keyed by `id` (see
  /// buffer_for()). Returns the text when the user finished editing it.
  std::optional<std::string> text_input(const char *label,
                                        const std::string &id,
                                        const std::string &model,
                                        const TextOptions &options);
  /// The edit buffer of field `id`: refreshed from `model` unless that field
  /// is being edited, so undo, reload and selection changes show up.
  std::string &buffer_for(const std::string &id, const std::string &model);
  /// Writes a raw TOML value and commits; reports invalid values.
  void apply_raw(const std::string &section, const std::string &key,
                 const std::string &raw);

  Mads::Ini::Editor &_editor;
  UiState &_ui;
  // Edit buffers of the text fields, by stable field id.
  std::map<std::string, std::string> _buffers;
  // Id of the text field that was active last frame, and whether it was
  // drawn this frame (see buffer_for()).
  std::string _editing;
  bool _editing_seen = false;
  // Add-key form of custom-key sections.
  std::string _new_key;
  std::string _new_value;
  int _new_type = 0;
};

} // namespace MadsIni
