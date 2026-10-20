// Application shell of the mads-ini editor: window, menu bar, palette,
// canvas and inspector columns, status footer, and the file workflow
// (open/save/reload, unsaved-changes and external-change handling).
#pragma once

#include "inspector.hpp"
#include "node_canvas.hpp"
#include "ui_state.hpp"

#include "gui_style.hpp"
#include "ini_editor.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct GLFWwindow;

namespace MadsIni {

class IniApp {
public:
  /// `file` is opened at startup; nullopt starts a new document from the
  /// template.
  explicit IniApp(std::optional<std::filesystem::path> file);
  IniApp(const IniApp &) = delete;
  IniApp &operator=(const IniApp &) = delete;

  /// Runs the GUI until the window closes; returns the process exit code.
  int run();

private:
  /// Actions that may discard unsaved changes.
  enum class Action { None, New, Open, Reload, Quit };

  void load_startup_document();
  void new_default_document();
  void request(Action action);
  void perform(Action action);
  bool save_document();
  bool save_as_dialog();
  bool handle_save(const Mads::Ini::SaveResult &result);
  void open_path(const std::filesystem::path &path);
  bool reload_document();
  bool is_node(const std::string &name) const;
  void duplicate_selected();
  void delete_selected();
  void poll_external_changes();
  void sanitize_selection();
  void update_title();
  void handle_shortcuts();
  void draw_menu_bar();
  void draw_banner();
  void draw_palette();
  void draw_problems();
  void draw_modals();
  void draw_footer();
  /// Shift+Space node finder: incremental search over node names, Enter
  /// focuses the canvas on the chosen node.
  void draw_finder();

  std::optional<std::filesystem::path> _file;
  Mads::Ini::Editor _editor;
  UiState _ui;
  NodeCanvas _canvas;
  Inspector _inspector;
  GLFWwindow *_window = nullptr;
  MadsGui::Texture _logo;
  std::string _window_title;
  std::vector<std::string> _categories;
  Action _pending = Action::None;
  bool _quitting = false;
  bool _external_change = false;
  bool _banner_dismissed = false;
  bool _open_unsaved_popup = false;
  bool _open_conflict_popup = false;
  bool _open_about_popup = false;
  bool _open_finder = false;
  char _finder_query[128] = {};
  int _finder_index = 0; // highlighted match
  std::string _conflict_message;
  double _next_disk_poll = 0.0;
};

} // namespace MadsIni
