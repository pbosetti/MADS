// The remote half of IniApp: the "Open broker settings" panel, the asynchronous
// connection, the password prompt, the recent-files menu and the polling of a
// remote document for changes. Nothing here blocks the UI thread on the
// network except the explicit user actions save and reload (bounded by the
// source's timeout).
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include "ini_app.hpp"

#include "detail/broker_ipc.hpp"

#include <imgui.h>

#include <portable-file-dialogs.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstring>
#include <exception>
#include <utility>

namespace MadsIni {

namespace Remote = Mads::Ini::Remote;

namespace {

constexpr const char *kAuthPopup = "Password required###broker_password";
constexpr const char *kRemoteEditOffTip = "remote editing is off on this broker (needs [broker] remote_edit and "
                                          "--crypto or an editing password)";
constexpr const char *kKeyHint = "Create a key pair with `mads --keypair NAME`, copy NAME.pub to the broker's "
                                 "editors folder.";

void wipe(char *buffer, std::size_t size) {
  volatile char *p = buffer;
  for (std::size_t i = 0; i < size; ++i) {
    p[i] = 0;
  }
}

std::string base_name(const std::string &path) {
  return path.empty() ? std::string("(none)") : std::filesystem::path(path).filename().string();
}

} // namespace

// ------------------------------------------------------------ the panel

void IniApp::draw_open_panel() {
  const int has_source = _editor.has_source() ? 1 : 0;
  if (_expand_open_panel) {
    ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    _expand_open_panel = false;
  } else if (has_source != _panel_had_source) {
    // Open while the document has no file or broker behind it, closed once it has.
    ImGui::SetNextItemOpen(has_source == 0, ImGuiCond_Always);
  }
  _panel_had_source = has_source;
  if (!ImGui::CollapsingHeader("Open broker settings")) {
    return;
  }

  const bool busy = _connect.valid();
  if (busy) {
    ImGui::TextWrapped("Connecting to %s...", _connect_target.endpoint.c_str());
  }
  ImGui::BeginDisabled(busy);

  // ---- discovered rooms
  ImGui::SeparatorText("Rooms");
  const auto rooms = _rooms.snapshot_rooms();
  if (rooms.empty()) {
    ImGui::TextDisabled("No rooms found");
  }
  bool any_encrypted = false;
  int index = 0;
  for (const auto &room : rooms) {
    ImGui::PushID(index++);
    any_encrypted = any_encrypted || room.encrypted;
    const bool can_edit = room.edit_port > 0;
    const std::string label = room.hostname.empty() ? room.name : room.name + " - " + room.hostname;
    const std::string endpoint = "tcp://" + room.host + ":" + std::to_string(room.edit_port);
    ImGui::BeginDisabled(!can_edit);
    const bool clicked = ImGui::Selectable(label.c_str());
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort)) {
      if (can_edit) {
        ImGui::SetTooltip("%s%s", endpoint.c_str(), room.encrypted ? " (CURVE)" : "");
      } else {
        ImGui::SetTooltip("%s", kRemoteEditOffTip);
      }
    }
    if (clicked) {
      RemoteTarget target;
      target.endpoint = endpoint;
      target.use_keys = room.encrypted;
      target.client_key_file = _prefs.data.client_key_file;
      target.broker_key_file = _prefs.data.broker_key_file;
      request_open_remote(std::move(target), OpenKind::Room);
    }
    ImGui::PopID();
  }

  // ---- broker on this host
  ImGui::SeparatorText("Local broker");
  _local_port = std::clamp(_local_port, 1, 65535);
  if (_local_checked_port != _local_port || ImGui::GetTime() >= _next_local_check) {
    _local_checked_port = _local_port;
    _next_local_check = ImGui::GetTime() + 1.0;
    const std::string no_broker =
        "no local broker with remote_edit on port " + std::to_string(_local_port);
    _local_tip.clear();
    try {
      _local_endpoint = Remote::local_endpoint(_local_port);
      std::string path = _local_endpoint;
      if (path.rfind("ipc://", 0) == 0) {
        path.erase(0, 6);
      }
      // Only a socket that still accepts connections is a running broker: a
      // stale file left by a crash must not enable the button.
      switch (Mads::detail::probe_ipc_socket(path)) {
      case Mads::detail::IpcSocketState::Live:
        _local_found = true;
        break;
      case Mads::detail::IpcSocketState::Stale:
        _local_found = false;
        _local_tip = "stale socket at " + path + ": the broker is not running";
        break;
      case Mads::detail::IpcSocketState::Foreign:
        _local_found = false;
        _local_tip = path + " is not a socket";
        break;
      case Mads::detail::IpcSocketState::Absent:
        _local_found = false;
        _local_tip = no_broker;
        break;
      }
    } catch (const std::exception &error) {
      _local_found = false;
      _local_endpoint.clear();
      _local_tip = no_broker + ": " + error.what();
    }
  }
  ImGui::BeginDisabled(!_local_found);
  const bool open_local = ImGui::Button("Local broker");
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort)) {
    if (_local_found) {
      ImGui::SetTooltip("%s", _local_endpoint.c_str());
    } else {
      ImGui::SetTooltip("%s", _local_tip.c_str());
    }
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70.0F);
  ImGui::InputInt("##local_port", &_local_port, 0, 0);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
    ImGui::SetTooltip("the broker's edit port");
  }
  if (open_local) {
    RemoteTarget target;
    target.endpoint = _local_endpoint;
    request_open_remote(std::move(target), OpenKind::Local);
  }

  // ---- manual address
  ImGui::SeparatorText("Address");
  ImGui::SetNextItemWidth(-FLT_MIN);
  const bool entered = ImGui::InputTextWithHint("##address", "host[:port], tcp://... or ipc://...", _address,
                                                sizeof(_address), ImGuiInputTextFlags_EnterReturnsTrue);
  const std::string endpoint = normalize_address(_address);
  const bool is_ipc = is_ipc_endpoint(endpoint);
  ImGui::BeginDisabled(is_ipc);
  ImGui::Checkbox("Use CURVE keys", &_use_keys);
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort)) {
    ImGui::SetTooltip(is_ipc ? "an ipc:// endpoint is plaintext, local to this host"
                             : "for a broker started with --crypto");
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(endpoint.empty());
  if (ImGui::Button("Open") || (entered && !endpoint.empty())) {
    RemoteTarget target;
    target.endpoint = endpoint;
    target.use_keys = _use_keys && !is_ipc;
    target.client_key_file = _prefs.data.client_key_file;
    target.broker_key_file = _prefs.data.broker_key_file;
    request_open_remote(std::move(target), OpenKind::Manual);
  }
  ImGui::EndDisabled();

  // ---- CURVE key files
  if ((_use_keys && !is_ipc && !endpoint.empty()) || any_encrypted) {
    draw_key_pickers();
  }
  ImGui::EndDisabled();
}

void IniApp::draw_key_pickers() {
  ImGui::SeparatorText("Keys");
  if (ImGui::Button("Secret key...")) {
    const auto chosen = pfd::open_file("Editor secret key", _prefs.data.client_key_file,
                                       {"Secret key", "*.key"}, pfd::opt::none).result();
    if (!chosen.empty()) {
      _prefs.data.client_key_file = chosen.front();
      _prefs.save();
    }
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%s", base_name(_prefs.data.client_key_file).c_str());
  if (ImGui::IsItemHovered() && !_prefs.data.client_key_file.empty()) {
    ImGui::SetTooltip("%s", _prefs.data.client_key_file.c_str());
  }
  if (ImGui::Button("Broker public key...")) {
    const auto chosen = pfd::open_file("Broker public key", _prefs.data.broker_key_file,
                                       {"Public key", "*.pub"}, pfd::opt::none).result();
    if (!chosen.empty()) {
      _prefs.data.broker_key_file = chosen.front();
      _prefs.save();
    }
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%s", base_name(_prefs.data.broker_key_file).c_str());
  if (ImGui::IsItemHovered() && !_prefs.data.broker_key_file.empty()) {
    ImGui::SetTooltip("%s", _prefs.data.broker_key_file.c_str());
  }
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("%s", kKeyHint);
  ImGui::PopStyleColor();
}

void IniApp::draw_recent_menu() {
  if (!ImGui::BeginMenu("Open Recent")) {
    return;
  }
  const auto &recent = _prefs.data.recent_files;
  if (recent.empty()) {
    ImGui::MenuItem("(none)", nullptr, false, false);
  }
  int index = 0;
  for (const auto &entry : recent) {
    ImGui::PushID(index++);
    std::error_code ec;
    const bool exists = std::filesystem::exists(entry, ec);
    if (ImGui::MenuItem(entry.c_str(), nullptr, false, exists)) {
      _queued_path = entry;
      request(Action::OpenRecent);
    }
    ImGui::PopID();
  }
  ImGui::Separator();
  if (ImGui::MenuItem("Clear", nullptr, false, !recent.empty())) {
    _prefs.clear_recent();
  }
  ImGui::EndMenu();
}

// ------------------------------------------------------------- connecting

void IniApp::request_open_remote(RemoteTarget target, OpenKind kind) {
  if (_connect.valid()) {
    return;
  }
  if (target.use_keys && (target.client_key_file.empty() || target.broker_key_file.empty())) {
    _ui.notify("Select the editor's secret key and the broker's public key first", true);
    return;
  }
  _queued_target = std::move(target);
  _queued_kind = kind;
  request(Action::OpenRemote);
}

void IniApp::begin_connect(RemoteTarget target, std::optional<std::string> password, bool remember,
                           AuthPurpose purpose) {
  std::optional<Remote::ClientSecret> remembered;
  if (!password) {
    const auto it = _prefs.data.remembered_secrets.find(target.endpoint);
    if (it != _prefs.data.remembered_secrets.end()) {
      remembered = Remote::ClientSecret::parse(it->second);
    }
  }
  _connect_had_remembered = remembered.has_value();
  _connect_remember = remember && password.has_value();
  _connect_purpose = purpose;
  _connect_snapshot = _editor.doc().serialize();
  _connect_target = target;
  _connect = std::async(std::launch::async, [target = std::move(target), remembered = std::move(remembered),
                                              password = std::move(password)]() mutable {
    return connect_remote(target, std::move(remembered), std::move(password));
  });
}

void IniApp::poll_connect() {
  if (!_connect.valid() || _connect.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
    return;
  }
  ConnectOutcome outcome = _connect.get();
  const RemoteTarget target = _connect_target;

  if (outcome.auth_required) {
    if (_connect_had_remembered) {
      drop_remembered(target.endpoint);
    } else if (_connect_purpose == AuthPurpose::Open && !outcome.error.empty()) {
      _ui.notify(outcome.error, true);
    }
    ask_password(target, _connect_purpose);
    return;
  }
  if (!outcome.source) {
    _pending = Action::None;
    _ui.notify("Cannot open " + target.endpoint + ": " + outcome.error, true);
    return;
  }
  if (_connect_purpose == AuthPurpose::Open) {
    finish_open(outcome);
  } else {
    finish_secret(outcome);
  }
}

void IniApp::finish_open(ConnectOutcome &outcome) {
  const RemoteTarget target = _connect_target;
  if (_editor.doc().serialize() != _connect_snapshot) {
    _ui.notify("Not opened: the document was modified while connecting", true);
    return;
  }
  // Taken now: open_source() consumes the source.
  const std::optional<Remote::ClientSecret> secret = outcome.secret;
  std::string error;
  bool ok = false;
  try {
    ok = _editor.open_source(std::move(outcome.source), &error);
  } catch (const Remote::AuthError &auth) {
    error = auth.what();
  }
  if (!ok) {
    _ui.notify("Cannot open " + target.endpoint + ": " + error, true);
    return;
  }
  _remote_target = target;
  _remote_secret = secret;
  _ui.selected.clear();
  _external_change = false;
  _banner_dismissed = false;
  _editor.auto_layout();
  _ui.notify("Opened " + _editor.location());

  if (_connect_remember && secret) {
    remember_secret(target.endpoint, *secret);
  }
  switch (_connect_kind) {
  case OpenKind::Manual:
    _prefs.data.manual_address = _address;
    break;
  case OpenKind::Local:
    _prefs.data.local_port = _local_port;
    break;
  case OpenKind::Room:
    break;
  }
  _prefs.data.use_keys = _use_keys;
  _prefs.save();
  start_poller();
}

// A password was accepted for the document that is already open (save or
// reload): give its source the derived secret and retry.
void IniApp::finish_secret(ConnectOutcome &outcome) {
  auto *source = dynamic_cast<Remote::RemoteSource *>(_editor.source());
  if (source == nullptr || !outcome.secret) {
    _pending = Action::None;
    _ui.notify("The remote document is no longer open", true);
    return;
  }
  source->set_secret(*outcome.secret);
  _remote_secret = outcome.secret;
  if (_connect_remember) {
    remember_secret(_connect_target.endpoint, *outcome.secret);
  }
  start_poller();
  if (_connect_purpose == AuthPurpose::Reload) {
    if (reload_document()) {
      _ui.notify("Reloaded " + _editor.location());
    }
    return;
  }
  const bool saved = handle_save(_editor.save());
  const Action action = _pending;
  if (saved) {
    _pending = Action::None;
    perform(action);
  }
}

// -------------------------------------------------------------- password

void IniApp::ask_password(const RemoteTarget &target, AuthPurpose purpose) {
  _auth_target = target;
  _auth_purpose = purpose;
  _auth_remember = false;
  wipe(_auth_password, sizeof(_auth_password));
  _open_auth_popup = true;
}

void IniApp::draw_auth_modal() {
  if (_open_auth_popup) {
    ImGui::OpenPopup(kAuthPopup);
    _open_auth_popup = false;
  }
  if (!ImGui::BeginPopupModal(kAuthPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    return;
  }
  ImGui::Text("Password for %s", _auth_target.endpoint.c_str());
  ImGui::Spacing();
  if (ImGui::IsWindowAppearing()) {
    ImGui::SetKeyboardFocusHere();
  }
  ImGui::SetNextItemWidth(320.0F);
  const bool entered = ImGui::InputText("##broker_password", _auth_password, sizeof(_auth_password),
                                        ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::Checkbox("Remember on this computer", &_auth_remember);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
    ImGui::SetTooltip("stores a key derived from the password, not the password: it opens this broker only");
  }
  ImGui::Spacing();
  const bool has_text = _auth_password[0] != '\0';
  ImGui::BeginDisabled(!has_text);
  if (ImGui::Button("OK") || (entered && has_text)) {
    std::string password = _auth_password;
    wipe(_auth_password, sizeof(_auth_password));
    ImGui::CloseCurrentPopup();
    begin_connect(_auth_target, std::move(password), _auth_remember, _auth_purpose);
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) {
    wipe(_auth_password, sizeof(_auth_password));
    _pending = Action::None;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void IniApp::drop_remembered(const std::string &endpoint) {
  if (_prefs.data.remembered_secrets.erase(endpoint) > 0) {
    _prefs.save();
    _ui.notify("The remembered password for " + endpoint + " is no longer valid", true);
  }
}

void IniApp::remember_secret(const std::string &endpoint, const Remote::ClientSecret &secret) {
  _prefs.data.remembered_secrets[endpoint] = secret.to_string();
  _prefs.save();
}

// --------------------------------------------------------------- polling

void IniApp::forget_remote() {
  _poller.stop();
  _remote_target.reset();
  _remote_secret.reset();
  _poller_editor_etag.clear();
}

void IniApp::start_poller() {
  if (_remote_target) {
    _poller_editor_etag = _editor.etag();
    _poller.start(*_remote_target, _remote_secret);
  }
}

bool IniApp::remote_changed() {
  if (!_poller.running()) {
    start_poller();
  }
  // Our own load or save moved the editor's etag: samples taken before it
  // say nothing about outside changes.
  if (_editor.etag() != _poller_editor_etag) {
    _poller_editor_etag = _editor.etag();
    _poller.invalidate();
    return false;
  }
  const auto sample = _poller.sample();
  return sample && *sample != _editor.etag();
}

void IniApp::show_prefs_notice() {
  const std::string notice = _prefs.take_notice();
  if (!notice.empty()) {
    _ui.notify(notice, true);
  }
}

} // namespace MadsIni
