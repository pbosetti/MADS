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
constexpr ImVec4 kWarningColor{0.95F, 0.75F, 0.30F, 1.0F};

#ifdef __APPLE__
constexpr const char *kModifier = "Cmd+";
#else
constexpr const char *kModifier = "Ctrl+";
#endif

std::vector<std::string> ini_filters() {
  return {"MADS settings", "*.ini *.toml", "All files", "*"};
}

void print_gui_hint(const char *what) {
  std::cerr << "mads ini: cannot " << what
            << " (no display?). Use `mads ini --print` or `mads ini --check FILE` instead.\n";
}

} // namespace

IniApp::IniApp(std::optional<std::filesystem::path> file)
    : _file(std::move(file)), _canvas(_editor, _ui), _inspector(_editor, _ui) {
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
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = nullptr;

  MadsGui::apply_style();

  ImGui_ImplGlfw_InitForOpenGL(_window, true);
  ImGui_ImplOpenGL3_Init(glsl_version);

  _logo = MadsGui::load_logo(std::filesystem::path(Mads::exec_dir("../share/images")));
  load_startup_document();

  while (!glfwWindowShouldClose(_window)) {
    glfwPollEvents();

    // Closing a dirty document asks first; the modal quits after Save/Discard.
    if (glfwWindowShouldClose(_window) && !_quitting && _editor.dirty()) {
      glfwSetWindowShouldClose(_window, GLFW_FALSE);
      if (_pending == Action::None) {
        request(Action::Quit);
      }
    }

    poll_external_changes();
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
  const std::string current = _editor.has_source() ? _editor.location() : std::string("mads.ini");
  const auto chosen = pfd::save_file("Save mads.ini as", current, ini_filters(), pfd::opt::none).result();
  if (chosen.empty()) {
    return false;
  }
  return handle_save(_editor.save_as(chosen));
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
    _ui.notify(result.message.empty() ? std::string("Save failed") : result.message, true);
    return false;
  }
  return false;
}

void IniApp::open_path(const std::filesystem::path &path) {
  std::string error;
  if (_editor.open(path, &error)) {
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
  if (!_editor.reload(&error)) {
    _ui.notify("Reload failed: " + error, true);
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
  if (!_editor.changed_on_disk()) {
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

  std::string line2 = "Drag from an output to an input to subscribe; select a node to edit it";
  if (!_ui.toast.empty() && ImGui::GetTime() < _ui.toast_until) {
    line2 = (_ui.toast_error ? "Error: " : "") + _ui.toast;
  }
  MadsGui::draw_status_footer(_logo, line1, line2);
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
      } else if (_open_conflict_popup) {
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

} // namespace MadsIni
