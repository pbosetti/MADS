#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include "ini_app.hpp"

#include "exec_path.hpp"
#include "ini_cli.hpp"
#include "mads.hpp"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imnodes.h>

#include <GLFW/glfw3.h>

#include <portable-file-dialogs.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cfloat>
#include <exception>
#include <iostream>
#include <string>
#include <utility>

namespace MadsIni {

namespace {

constexpr const char *kWindowTitleBase = "MADS ini editor";
constexpr const char *kUnsavedPopup = "Unsaved changes";
constexpr const char *kConflictPopup = "File changed on disk";
constexpr const char *kAboutPopup = "About MADS ini editor";
constexpr const char *kFinderPopup = "##find_node";
constexpr ImVec4 kWarningColor{0.95F, 0.75F, 0.30F, 1.0F};
constexpr int kFinderMaxResults = 12;

#ifdef __APPLE__
constexpr const char *kModifier = "Cmd+";
#else
constexpr const char *kModifier = "Ctrl+";
#endif

std::vector<std::string> ini_filters() {
  return {"MADS settings", "*.ini *.toml", "All files", "*"};
}

// How well `name` matches the finder `query`, case-insensitively: 0 for a
// prefix, 1 for a substring, 2 for the query's letters in order (fuzzy, e.g.
// "fla" in "fmu_linear_axis"), nullopt for no match. An empty query matches
// everything as a prefix.
std::optional<int> match_rank(const std::string &name, const std::string &query) {
  const auto lower = [](std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
  };
  const std::string n = lower(name);
  const std::string q = lower(query);
  const size_t pos = n.find(q);
  if (pos == 0) {
    return 0;
  }
  if (pos != std::string::npos) {
    return 1;
  }
  size_t i = 0;
  for (char c : n) {
    if (i < q.size() && c == q[i]) {
      ++i;
    }
  }
  if (i == q.size()) {
    return 2;
  }
  return std::nullopt;
}

void print_gui_hint(const char *what) {
  std::cerr << "mads ini: cannot " << what
            << " (no display?). Use `mads ini --print` or `mads ini --check FILE` instead.\n";
}

} // namespace

IniApp::IniApp(std::optional<std::filesystem::path> file)
    : _file(std::move(file)), _canvas(_editor, _ui), _inspector(_editor, _ui) {
  std::snprintf(_address, sizeof(_address), "%s", _prefs.data.manual_address.c_str());
  _use_keys = _prefs.data.use_keys;
  _local_port = _prefs.data.local_port > 0 ? _prefs.data.local_port : Mads::Ini::Remote::kDefaultPort;
  for (const auto &type : _editor.schema().types()) {
    if (type.document) {
      continue;
    }
    if (std::find(_categories.begin(), _categories.end(), type.category) == _categories.end()) {
      _categories.push_back(type.category);
    }
  }
}

int IniApp::run() {
  if (!glfwInit()) {
    print_gui_hint("start the GUI");
    return 1;
  }

  const char *glsl_version = MadsGui::setup_gl_hints();
  _window = glfwCreateWindow(MadsGui::kWindowWidth, MadsGui::kWindowHeight, kWindowTitleBase, nullptr, nullptr);
  if (_window == nullptr) {
    glfwTerminate();
    print_gui_hint("open a window");
    return 1;
  }

  glfwMakeContextCurrent(_window);
  glfwSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImNodes::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  // No keyboard navigation: Space is the canvas pan key (Space+drag), and
  // with navigation on it would also press whatever widget has nav focus.
  // Tab still moves between text fields.
  io.IniFilename = nullptr;

  MadsGui::apply_style();

  ImGui_ImplGlfw_InitForOpenGL(_window, true);
  ImGui_ImplOpenGL3_Init(glsl_version);

  // Same font as mads-chat and mads-director: as the first font added it
  // becomes ImGui's default, for the whole UI (canvas included).
  MadsGui::load_monospace_font(15.0F);

  _logo = MadsGui::load_logo(std::filesystem::path(Mads::exec_dir("../share/images")));
  load_startup_document();
  show_prefs_notice();
  _rooms.start();

  while (!glfwWindowShouldClose(_window)) {
    glfwPollEvents();

    // Closing a dirty document asks first; the modal quits after Save/Discard.
    if (glfwWindowShouldClose(_window) && !_quitting && _editor.dirty()) {
      glfwSetWindowShouldClose(_window, GLFW_FALSE);
      if (_pending == Action::None) {
        request(Action::Quit);
      }
    }

    poll_connect();
    poll_external_changes();
    show_prefs_notice();
    update_title();
    sanitize_selection();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F), ImGuiCond_Always);
    ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
    ImGui::Begin("MadsIni", nullptr, MadsGui::kMainWindowFlags | ImGuiWindowFlags_MenuBar);

    handle_shortcuts();
    draw_menu_bar();

    const float footer_height = MadsGui::status_footer_height();
    ImGui::BeginChild("main_content", ImVec2(0.0F, -footer_height), false);
    draw_banner();

    if (ImGui::BeginTable("main_split", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0.0F, ImGui::GetContentRegionAvail().y))) {
      ImGui::TableSetupColumn("palette", ImGuiTableColumnFlags_WidthFixed, 220.0F);
      ImGui::TableSetupColumn("canvas", ImGuiTableColumnFlags_WidthStretch, 1.0F);
      ImGui::TableSetupColumn("inspector", ImGuiTableColumnFlags_WidthFixed, 400.0F);

      ImGui::TableNextColumn();
      ImGui::BeginChild("palette_panel", ImVec2(0.0F, 0.0F), ImGuiChildFlags_Borders);
      draw_palette();
      ImGui::EndChild();

      ImGui::TableNextColumn();
      ImGui::BeginChild("canvas_panel", ImVec2(0.0F, 0.0F), false);
      _canvas.draw();
      ImGui::EndChild();

      ImGui::TableNextColumn();
      ImGui::BeginChild("inspector_panel", ImVec2(0.0F, 0.0F), ImGuiChildFlags_Borders);
      _inspector.draw();
      ImGui::EndChild();

      ImGui::EndTable();
    }
    ImGui::EndChild();

    draw_footer();
    draw_modals();
    draw_finder();
    ImGui::End();

    ImGui::Render();
    int display_width = 0;
    int display_height = 0;
    glfwGetFramebufferSize(_window, &display_width, &display_height);
    glViewport(0, 0, display_width, display_height);
    glClearColor(MadsGui::kClearColor.x, MadsGui::kClearColor.y, MadsGui::kClearColor.z, MadsGui::kClearColor.w);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(_window);
  }

  _poller.stop();
  _rooms.stop();
  MadsGui::free_texture(_logo);
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImNodes::DestroyContext();
  ImGui::DestroyContext();
  glfwDestroyWindow(_window);
  _window = nullptr;
  glfwTerminate();
  return 0;
}

// ------------------------------------------------------------ documents

void IniApp::load_startup_document() {
  if (_file) {
    std::string error;
    if (!_editor.open(*_file, &error)) {
      _ui.notify("Cannot open " + _file->string() + ": " + error, true);
      new_default_document();
    } else {
      _prefs.add_recent(std::filesystem::absolute(*_file));
    }
  } else {
    new_default_document();
  }
  _editor.auto_layout();
}

void IniApp::new_default_document() {
  try {
    _editor.new_document(Mads::Ini::render_template());
  } catch (const std::exception &error) {
    _ui.notify(std::string("Template unavailable: ") + error.what(), true);
    _editor.new_document("[agents]\n\n[broker]\n");
  }
}

void IniApp::request(Action action) {
  if (_editor.dirty()) {
    _pending = action;
    _open_unsaved_popup = true;
  } else {
    perform(action);
  }
}

void IniApp::perform(Action action) {
  switch (action) {
  case Action::None:
    break;
  case Action::New:
    new_default_document();
    _editor.auto_layout();
    forget_remote();
    _ui.selected.clear();
    _external_change = false;
    _banner_dismissed = false;
    _ui.notify("New document");
    break;
  case Action::Open: {
    const std::string default_dir =
        _editor.has_source() ? std::filesystem::path(_editor.location()).parent_path().string() : std::string();
    const auto chosen = pfd::open_file("Open mads.ini", default_dir, ini_filters(), pfd::opt::none).result();
    if (!chosen.empty()) {
      open_path(chosen.front());
    }
    break;
  }
  case Action::OpenRecent:
    open_path(_queued_path);
    break;
  case Action::OpenRemote:
    if (_connect.valid()) {
      _ui.notify("Already connecting", true);
    } else {
      begin_connect(_queued_target, std::nullopt, false, AuthPurpose::Open);
      _connect_kind = _queued_kind;
    }
    break;
  case Action::Reload:
    if (reload_document()) {
      _ui.notify("Reloaded " + _editor.location());
    }
    break;
  case Action::Quit:
    _quitting = true;
    glfwSetWindowShouldClose(_window, GLFW_TRUE);
    break;
  }
}

bool IniApp::save_document() {
  if (!_editor.has_source()) {
    return save_as_dialog();
  }
  return handle_save(_editor.save());
}

bool IniApp::save_as_dialog() {
  // A remote document's location is an endpoint ("tcp://host:9094
  // (remote)"), not a usable file name: suggest a plain mads.ini then.
  const std::string current =
      _editor.has_source() && !_remote_target ? _editor.location() : std::string("mads.ini");
  const auto chosen = pfd::save_file("Save mads.ini as", current, ini_filters(), pfd::opt::none).result();
  if (chosen.empty()) {
    return false;
  }
  const bool saved = handle_save(_editor.save_as(chosen));
  if (saved) {
    forget_remote();
    _prefs.add_recent(std::filesystem::absolute(chosen));
  }
  return saved;
}

bool IniApp::handle_save(const Mads::Ini::SaveResult &result) {
  switch (result.status) {
  case Mads::Ini::SaveResult::Status::Ok:
    _external_change = false;
    _banner_dismissed = false;
    _ui.notify("Saved " + _editor.location());
    return true;
  case Mads::Ini::SaveResult::Status::Conflict:
    _conflict_message = result.message;
    _open_conflict_popup = true;
    return false;
  case Mads::Ini::SaveResult::Status::Error:
    if (result.auth_required && _remote_target) {
      drop_remembered(_remote_target->endpoint);
      ask_password(*_remote_target, AuthPurpose::Save);
      return false;
    }
    _ui.notify(result.message.empty() ? std::string("Save failed") : result.message, true);
    return false;
  }
  return false;
}

void IniApp::open_path(const std::filesystem::path &path) {
  std::string error;
  if (_editor.open(path, &error)) {
    forget_remote();
    _prefs.add_recent(std::filesystem::absolute(path));
    _ui.selected.clear();
    _external_change = false;
    _banner_dismissed = false;
    _editor.auto_layout();
    _ui.notify("Opened " + path.string());
  } else {
    _ui.notify("Cannot open " + path.string() + ": " + error, true);
  }
}

bool IniApp::reload_document() {
  if (!_editor.has_source()) {
    _ui.notify("Nothing to reload: the document has no file", true);
    return false;
  }
  std::string error;
  try {
    if (!_editor.reload(&error)) {
      _ui.notify("Reload failed: " + error, true);
      return false;
    }
  } catch (const Mads::Ini::Remote::AuthError &) {
    if (_remote_target) {
      drop_remembered(_remote_target->endpoint);
      ask_password(*_remote_target, AuthPurpose::Reload);
    }
    return false;
  }
  _external_change = false;
  _banner_dismissed = false;
  _editor.auto_layout();
  return true;
}

void IniApp::poll_external_changes() {
  const double now = ImGui::GetTime();
  if (now < _next_disk_poll) {
    return;
  }
  _next_disk_poll = now + 1.0;
  // A remote document is never polled from here: a dead broker would stall
  // the UI for the whole timeout. The worker in _poller does it.
  const bool changed = _remote_target ? remote_changed() : _editor.changed_on_disk();
  if (!changed) {
    return;
  }
  if (!_editor.dirty()) {
    if (reload_document()) {
      _ui.notify("Reloaded: file changed on disk");
    }
    return;
  }
  if (!_banner_dismissed) {
    _external_change = true;
  }
}

// ------------------------------------------------------------- helpers

bool IniApp::is_node(const std::string &name) const {
  const auto &nodes = _editor.nodes();
  return std::find(nodes.begin(), nodes.end(), name) != nodes.end();
}

void IniApp::sanitize_selection() {
  if (!_ui.selected.empty() && !is_node(_ui.selected)) {
    _ui.selected.clear();
  }
}

void IniApp::update_title() {
  std::string title = std::string(kWindowTitleBase) + " - " + _editor.location();
  if (_editor.dirty()) {
    title += " *";
  }
  if (title != _window_title) {
    glfwSetWindowTitle(_window, title.c_str());
    _window_title = title;
  }
}

void IniApp::duplicate_selected() {
  if (!is_node(_ui.selected)) {
    return;
  }
  const std::string copy = _editor.duplicate_node(_ui.selected);
  if (copy.empty()) {
    _ui.notify("Cannot duplicate [" + _ui.selected + "]", true);
    return;
  }
  _ui.notify("Duplicated as [" + copy + "]");
  _ui.selected = copy;
  _ui.focus_request = copy;
}

void IniApp::delete_selected() {
  if (!is_node(_ui.selected)) {
    return;
  }
  const std::string name = _ui.selected;
  if (_editor.delete_node(name)) {
    _ui.selected.clear();
    _ui.notify("Deleted [" + name + "]");
  } else {
    _ui.notify("Cannot delete [" + name + "]", true);
  }
}

// ------------------------------------------------------------------ UI

void IniApp::handle_shortcuts() {
  // Shortcuts stay off while a modal is open, so they cannot stack dialogs.
  if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup)) {
    return;
  }
  const ImGuiInputFlags route = ImGuiInputFlags_RouteGlobal;
  const bool typing = ImGui::GetIO().WantTextInput;

  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_N, route)) {
    request(Action::New);
  }
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, route)) {
    request(Action::Open);
  }
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, route)) {
    save_document();
  }
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, route)) {
    save_as_dialog();
  }
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Q, route)) {
    request(Action::Quit);
  }
  if (!typing) {
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, route) && _editor.can_undo()) {
      _editor.undo();
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, route) ||
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, route)) {
      if (_editor.can_redo()) {
        _editor.redo();
      }
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D, route)) {
      duplicate_selected();
    }
    if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_Space, route)) {
      _open_finder = true;
    }
  }
}

void IniApp::draw_menu_bar() {
  if (!ImGui::BeginMenuBar()) {
    return;
  }
  const std::string mod = kModifier;
  const bool has_selected_node = is_node(_ui.selected);

  if (ImGui::BeginMenu("File")) {
    if (ImGui::MenuItem("New", (mod + "N").c_str())) {
      request(Action::New);
    }
    if (ImGui::MenuItem("Open...", (mod + "O").c_str())) {
      request(Action::Open);
    }
    draw_recent_menu();
    if (ImGui::MenuItem("Open broker settings...")) {
      _expand_open_panel = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Save", (mod + "S").c_str())) {
      save_document();
    }
    if (ImGui::MenuItem("Save As...", (mod + "Shift+S").c_str())) {
      save_as_dialog();
    }
    if (ImGui::MenuItem("Reload", nullptr, false, _editor.has_source())) {
      request(Action::Reload);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Quit", (mod + "Q").c_str())) {
      request(Action::Quit);
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Edit")) {
    const std::string undo_label = _editor.undo_label();
    const std::string redo_label = _editor.redo_label();
    if (ImGui::MenuItem((undo_label.empty() ? std::string("Undo") : "Undo " + undo_label).c_str(),
                        (mod + "Z").c_str(), false, _editor.can_undo())) {
      _editor.undo();
    }
    if (ImGui::MenuItem((redo_label.empty() ? std::string("Redo") : "Redo " + redo_label).c_str(),
                        (mod + "Shift+Z").c_str(), false, _editor.can_redo())) {
      _editor.redo();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Duplicate node", (mod + "D").c_str(), false, has_selected_node)) {
      duplicate_selected();
    }
    if (ImGui::MenuItem("Delete node", nullptr, false, has_selected_node)) {
      delete_selected();
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("View")) {
    ImGui::MenuItem("Show catch-all links", nullptr, &_ui.show_catch_all);
    ImGui::MenuItem("Minimap", nullptr, &_ui.show_minimap);
    ImGui::Separator();
    if (ImGui::MenuItem("Find node...", "Shift+Space")) {
      _open_finder = true;
    }
    if (ImGui::MenuItem("Auto layout")) {
      _editor.auto_layout(true);
    }
    if (ImGui::MenuItem("Document settings")) {
      _ui.selected.clear();
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Help")) {
    if (ImGui::MenuItem("About")) {
      _open_about_popup = true;
    }
    ImGui::EndMenu();
  }

  ImGui::EndMenuBar();
}

void IniApp::draw_banner() {
  if (!_external_change) {
    return;
  }
  ImGui::TextColored(kWarningColor, "The file changed on disk. Reload discards your unsaved changes.");
  ImGui::SameLine();
  if (ImGui::Button("Reload")) {
    if (reload_document()) {
      _ui.notify("Reloaded from disk");
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Keep mine")) {
    _external_change = false;
    _banner_dismissed = true;
  }
  ImGui::Spacing();
}

void IniApp::draw_palette() {
  draw_open_panel();
  ImGui::Spacing();
  if (ImGui::Button("Document settings", ImVec2(-FLT_MIN, 0.0F))) {
    _ui.selected.clear();
  }

  for (const auto &category : _categories) {
    ImGui::SeparatorText(category.c_str());
    for (const auto &type : _editor.schema().types()) {
      if (type.document || type.category != category) {
        continue;
      }
      ImGui::PushID(type.id.c_str());
      if (ImGui::Button(type.label.c_str(), ImVec2(-FLT_MIN, 0.0F))) {
        float x = 0.0F;
        float y = 0.0F;
        _canvas.view_center(x, y);
        const std::string name = _editor.add_node(type.id, x, y);
        if (!name.empty()) {
          _ui.selected = name;
          _ui.focus_request = name;
        }
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        std::string tip;
        if (!type.executable.empty()) {
          tip += "executable: " + type.executable;
        }
        if (type.custom_keys) {
          tip += std::string(tip.empty() ? "" : "\n") + "custom keys allowed";
        }
        if (!tip.empty()) {
          ImGui::SetTooltip("%s", tip.c_str());
        }
      }
      ImGui::PopID();
    }
  }

  draw_problems();
}

void IniApp::draw_problems() {
  const auto &diagnostics = _editor.diagnostics();
  ImGui::Spacing();
  const std::string title = "Problems (" + std::to_string(diagnostics.size()) + ")";
  if (!ImGui::CollapsingHeader(title.c_str())) {
    return;
  }
  if (diagnostics.empty()) {
    ImGui::TextDisabled("No problems.");
    return;
  }

  int index = 0;
  for (const auto &diagnostic : diagnostics) {
    ImGui::PushID(index++);
    std::string text;
    if (!diagnostic.section.empty()) {
      text += "[" + diagnostic.section + "] ";
    }
    if (!diagnostic.key.empty()) {
      text += diagnostic.key + ": ";
    }
    text += diagnostic.message;

    ImVec4 color = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    if (diagnostic.severity == Mads::Ini::Diagnostic::Severity::Error) {
      color = MadsGui::kErrorColor;
    } else if (diagnostic.severity == Mads::Ini::Diagnostic::Severity::Warning) {
      color = kWarningColor;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    if (ImGui::Selectable(text.c_str())) {
      if (diagnostic.section == "agents" || diagnostic.section == "broker") {
        _ui.selected.clear();
      } else if (is_node(diagnostic.section)) {
        _ui.selected = diagnostic.section;
      }
    }
    ImGui::PopStyleColor();
    ImGui::PopID();
  }
}

void IniApp::draw_footer() {
  std::size_t errors = 0;
  std::size_t warnings = 0;
  for (const auto &diagnostic : _editor.diagnostics()) {
    if (diagnostic.severity == Mads::Ini::Diagnostic::Severity::Error) {
      ++errors;
    } else if (diagnostic.severity == Mads::Ini::Diagnostic::Severity::Warning) {
      ++warnings;
    }
  }

  const std::string line1 = _editor.location() + (_editor.dirty() ? " (modified)" : "") + "  |  " +
                            std::to_string(_editor.nodes().size()) + " agents, " +
                            std::to_string(_editor.edges().size()) + " links, " + std::to_string(errors) +
                            " errors, " + std::to_string(warnings) + " warnings";

  // Second line: an active toast, else the first error of the document (both
  // in red when they are errors), else a usage hint.
  std::string line2 = "Drag from an output to an input to subscribe; select a node to edit it";
  bool error = false;
  if (!_ui.toast.empty() && ImGui::GetTime() < _ui.toast_until) {
    line2 = _ui.toast;
    error = _ui.toast_error;
  } else if (errors > 0) {
    for (const auto &diagnostic : _editor.diagnostics()) {
      if (diagnostic.severity == Mads::Ini::Diagnostic::Severity::Error) {
        line2 = (diagnostic.section.empty() ? "" : "[" + diagnostic.section + "] ") + diagnostic.message;
        break;
      }
    }
    error = true;
  }
  MadsGui::draw_status_footer(_logo, line1, line2, error ? &MadsGui::kErrorColor : nullptr);
}

void IniApp::draw_modals() {
  // Unsaved changes: Save / Discard / Cancel, then the pending action.
  if (_open_unsaved_popup) {
    ImGui::OpenPopup(kUnsavedPopup);
    _open_unsaved_popup = false;
  }
  if (ImGui::BeginPopupModal(kUnsavedPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("You have unsaved changes.");
    ImGui::TextDisabled("%s", _editor.location().c_str());
    ImGui::Spacing();
    if (ImGui::Button("Save")) {
      if (save_document()) {
        const Action action = _pending;
        _pending = Action::None;
        ImGui::CloseCurrentPopup();
        perform(action);
      } else if (_open_conflict_popup || _open_auth_popup) {
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard")) {
      const Action action = _pending;
      _pending = Action::None;
      ImGui::CloseCurrentPopup();
      perform(action);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      _pending = Action::None;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  // The file changed on disk while a save was pending.
  if (_open_conflict_popup) {
    ImGui::OpenPopup(kConflictPopup);
    _open_conflict_popup = false;
  }
  if (ImGui::BeginPopupModal(kConflictPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("The file changed on disk since it was loaded or saved.");
    if (!_conflict_message.empty()) {
      ImGui::TextDisabled("%s", _conflict_message.c_str());
    }
    ImGui::Spacing();
    if (ImGui::Button("Reload (discard mine)")) {
      const Action action = _pending;
      _pending = Action::None;
      ImGui::CloseCurrentPopup();
      if (reload_document()) {
        _ui.notify("Reloaded from disk");
        perform(action);
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Overwrite")) {
      const bool saved = handle_save(_editor.save_overwrite());
      const Action action = _pending;
      _pending = Action::None;
      ImGui::CloseCurrentPopup();
      if (saved) {
        perform(action);
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      _pending = Action::None;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  draw_auth_modal();

  if (_open_about_popup) {
    ImGui::OpenPopup(kAboutPopup);
    _open_about_popup = false;
  }
  if (ImGui::BeginPopupModal(kAboutPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("MADS ini editor");
    ImGui::Text("Version %s", Mads::version().c_str());
    ImGui::Spacing();
    ImGui::TextUnformatted("Nodes are agents, links are topics.");
    ImGui::Spacing();
    if (ImGui::Button("Close")) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

void IniApp::draw_finder() {
  if (_open_finder) {
    _open_finder = false;
    _finder_query[0] = '\0';
    _finder_index = 0;
    ImGui::OpenPopup(kFinderPopup);
  }
  // Upper middle of the window, like a launcher.
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5F,
                                 viewport->WorkPos.y + viewport->WorkSize.y * 0.25F),
                          ImGuiCond_Always, ImVec2(0.5F, 0.0F));
  ImGui::SetNextWindowSize(ImVec2(std::min(520.0F, viewport->WorkSize.x * 0.8F), 0.0F));
  if (!ImGui::BeginPopup(kFinderPopup, ImGuiWindowFlags_NoMove)) {
    return;
  }

  // Matches, best first: rank, then file order.
  struct Match {
    int rank;
    size_t order;
    const std::string *name;
  };
  std::vector<Match> matches;
  const auto &nodes = _editor.nodes();
  for (size_t i = 0; i < nodes.size(); ++i) {
    if (auto rank = match_rank(nodes[i], _finder_query)) {
      matches.push_back({*rank, i, &nodes[i]});
    }
  }
  std::stable_sort(matches.begin(), matches.end(),
                   [](const Match &a, const Match &b) { return a.rank < b.rank; });
  if (matches.size() > static_cast<size_t>(kFinderMaxResults)) {
    matches.resize(kFinderMaxResults);
  }
  const int count = static_cast<int>(matches.size());

  if (ImGui::IsWindowAppearing()) {
    ImGui::SetKeyboardFocusHere();
  }
  ImGui::SetNextItemWidth(-FLT_MIN);
  const bool entered = ImGui::InputTextWithHint("##query", "Find node...", _finder_query, sizeof(_finder_query),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
  if (ImGui::IsItemEdited()) {
    _finder_index = 0;
  }
  // The text field keeps the keyboard: arrows move the highlight, Enter
  // picks it, Escape closes.
  if (count > 0) {
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
      _finder_index = (_finder_index + 1) % count;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
      _finder_index = (_finder_index + count - 1) % count;
    }
    _finder_index = std::clamp(_finder_index, 0, count - 1);
  }
  const std::string *chosen = nullptr;
  if (entered && count > 0) {
    chosen = matches[static_cast<size_t>(_finder_index)].name;
  }

  if (count == 0) {
    ImGui::TextDisabled("No matching node");
  }
  for (int i = 0; i < count; ++i) {
    const std::string &name = *matches[static_cast<size_t>(i)].name;
    ImGui::PushID(i);
    if (ImGui::Selectable(name.c_str(), i == _finder_index)) {
      chosen = &name;
    }
    // The mouse moves the highlight only when it actually moves, so that a
    // pointer resting on the list does not fight the arrow keys.
    const ImVec2 delta = ImGui::GetIO().MouseDelta;
    if (ImGui::IsItemHovered() && (delta.x != 0.0F || delta.y != 0.0F)) {
      _finder_index = i;
    }
    // Type, right-aligned and dimmed.
    const Mads::Ini::NodeType *type = _editor.schema().type(_editor.type_of(name));
    const std::string label = type ? (type->executable.empty() ? type->label : type->executable)
                                   : _editor.type_of(name);
    ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(label.c_str()).x);
    ImGui::TextDisabled("%s", label.c_str());
    ImGui::PopID();
  }

  if (chosen != nullptr) {
    _ui.selected = *chosen;
    _ui.focus_request = *chosen;
    ImGui::CloseCurrentPopup();
  } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

} // namespace MadsIni
