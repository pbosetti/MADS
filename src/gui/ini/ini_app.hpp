// Application shell of the mads-ini editor: window, menu bar, palette,
// canvas and inspector columns, status footer, and the file workflow
// (open/save/reload, unsaved-changes and external-change handling).
#pragma once

#include "inspector.hpp"
#include "node_canvas.hpp"
#include "prefs.hpp"
#include "remote_client.hpp"
#include "ui_state.hpp"

#include "gui_style.hpp"
#include "ini_editor.hpp"
#include "room_discovery.hpp"

#include <filesystem>
#include <future>
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
  enum class Action { None, New, Open, OpenRecent, OpenRemote, Reload, Quit };

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

  // ---- remote documents (ini_app_remote.cpp)

  /// What a password is asked for; the request is retried afterwards.
  enum class AuthPurpose { Open, Save, Reload };
  /// Which part of the Open panel started a connection.
  enum class OpenKind { Room, Local, Manual };

  void draw_open_panel();
  void draw_key_pickers();
  void draw_recent_menu();
  void draw_auth_modal();
  /// Opens `target` after the unsaved-changes check.
  void request_open_remote(RemoteTarget target, OpenKind kind);
  void begin_connect(RemoteTarget target, std::optional<std::string> password, bool remember,
                     AuthPurpose purpose);
  void poll_connect();
  void finish_open(ConnectOutcome &outcome);
  void finish_secret(ConnectOutcome &outcome);
  void ask_password(const RemoteTarget &target, AuthPurpose purpose);
  void drop_remembered(const std::string &endpoint);
  void remember_secret(const std::string &endpoint, const Mads::Ini::Remote::ClientSecret &secret);
  void forget_remote();
  void start_poller();
  bool remote_changed();
  void show_prefs_notice();

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

  PrefsStore _prefs;
  MadsGui::RoomDiscovery _rooms;
  // Open panel.
  char _address[256] = {};
  bool _use_keys = false;
  int _local_port = 0;
  bool _expand_open_panel = false;
  int _panel_had_source = -1; // -1: not drawn yet
  std::string _local_endpoint;
  std::string _local_tip; // tooltip when the local broker is not reachable
  bool _local_found = false;
  int _local_checked_port = -1;
  double _next_local_check = 0.0;
  // Connection in progress (worker thread) and its parameters.
  std::future<ConnectOutcome> _connect;
  RemoteTarget _connect_target;
  OpenKind _connect_kind = OpenKind::Manual;
  AuthPurpose _connect_purpose = AuthPurpose::Open;
  bool _connect_remember = false;
  bool _connect_had_remembered = false;
  std::string _connect_snapshot; // document text when the connection began
  RemoteTarget _queued_target;   // waits for the unsaved-changes answer
  OpenKind _queued_kind = OpenKind::Manual;
  std::filesystem::path _queued_path;
  // The open remote document, if any.
  std::optional<RemoteTarget> _remote_target;
  std::optional<Mads::Ini::Remote::ClientSecret> _remote_secret;
  RemotePoller _poller;
  std::string _poller_editor_etag;
  // Password modal.
  bool _open_auth_popup = false;
  RemoteTarget _auth_target;
  AuthPurpose _auth_purpose = AuthPurpose::Open;
  char _auth_password[256] = {};
  bool _auth_remember = false;
};

} // namespace MadsIni
