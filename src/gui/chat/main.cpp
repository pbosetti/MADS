#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>
#include <nlohmann/json.hpp>
#include <portable-file-dialogs.h>
#include <TextEditor.h>

#include "gui_style.hpp"
#include "detail/config_dir.hpp"

#include <agent.hpp>

#if !defined(LIB_VERSION_NUM) || LIB_VERSION_NUM < VERSION_ENCODE(2, 0, 3)
#error "MADS Chat expects MADS C++ library version 2.0.3 or later."
#endif

#if defined(LIB_VERSION_NUM) && defined(VERSION_ENCODE) && LIB_VERSION_NUM >= VERSION_ENCODE(2, 1, 0)
#define MADS_CHAT_HAS_SERVICE_DISCOVERY 1
#include <service_discovery.hpp>
#else
#define MADS_CHAT_HAS_SERVICE_DISCOVERY 0
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <cmath>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr const char *kWindowTitle = "MADS Chat";
constexpr const char *kAppAgentName = "mads_chat";
// Settings live in the per-user config directory (detail::config_dir()). The
// file this app used to write in the working directory is only read once, to
// migrate it, and is never deleted.
constexpr const char *kSettingsFileName = "mads-chat.json";
constexpr const char *kLegacySettingsFileName = "mads_chat_settings.json";

struct AppSettings {
  std::string host = "localhost";
  int pub_port = 9090;
  int sub_port = 9091;
  std::string subscribe_topics = "";
  std::string pub_topic = "";
  std::string client_private_key_path;
  std::string broker_public_key_path;
};

struct TopicState {
  std::string payload;
  std::chrono::steady_clock::time_point last_received_at = std::chrono::steady_clock::now();
  std::optional<std::chrono::steady_clock::time_point> previous_received_at;
  std::vector<double> inter_arrival_seconds;
  bool expanded = false;
  bool json_valid = false;
  json parsed_payload;
};

struct TopicTimingStats {
  double mean_seconds = 0.0;
  double stdev_seconds = 0.0;
  std::size_t sample_count = 0;
};

#if MADS_CHAT_HAS_SERVICE_DISCOVERY
struct DiscoveredRoom {
  std::string name;
  std::string host;
  int pub_port = -1;
  int sub_port = -1;
  bool encrypted = false;
  std::string hostname;
  std::string version;
};
#endif

struct JsonSpan {
  std::string text;
  ImVec4 color;
};

using JsonLine = std::vector<JsonSpan>;

ImVec4 color_text() {
  return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

ImVec4 color_key() {
  return ImVec4(0.95F, 0.77F, 0.35F, 1.0F);
}

ImVec4 color_string() {
  return ImVec4(0.54F, 0.83F, 0.52F, 1.0F);
}

ImVec4 color_number() {
  return ImVec4(0.47F, 0.74F, 0.95F, 1.0F);
}

ImVec4 color_bool() {
  return ImVec4(0.95F, 0.47F, 0.47F, 1.0F);
}

ImVec4 color_null() {
  return ImVec4(0.65F, 0.65F, 0.65F, 1.0F);
}

ImVec4 color_elapsed_normal() {
  return ImVec4(0.35F, 0.78F, 0.36F, 1.0F);
}

ImVec4 color_elapsed_warning() {
  return ImVec4(0.92F, 0.58F, 0.18F, 1.0F);
}

ImVec4 color_elapsed_alert() {
  return ImVec4(0.87F, 0.24F, 0.24F, 1.0F);
}

std::string make_endpoint_uri(const std::string &host, int port) {
  return "tcp://" + host + ":" + std::to_string(port);
}

std::string run_command_capture(const char *command) {
#if defined(_WIN32)
  FILE *pipe = _popen(command, "r");
#else
  FILE *pipe = popen(command, "r");
#endif
  if (pipe == nullptr) {
    return {};
  }

  std::array<char, 256> buffer{};
  std::string output;
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output += buffer.data();
  }

#if defined(_WIN32)
  _pclose(pipe);
#else
  pclose(pipe);
#endif

  while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
    output.pop_back();
  }
  return output;
}

std::string trim_copy(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

#if MADS_CHAT_HAS_SERVICE_DISCOVERY
std::string lower_copy(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const char ch : value) {
    result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return result;
}

std::optional<int> find_port(const std::map<std::string, uint16_t> &ports,
                             std::initializer_list<std::string_view> names) {
  for (const std::string_view name : names) {
    const auto exact = ports.find(std::string(name));
    if (exact != ports.end()) {
      return static_cast<int>(exact->second);
    }
  }

  for (const auto &[key, port] : ports) {
    const std::string normalized_key = lower_copy(key);
    for (const std::string_view name : names) {
      if (normalized_key == lower_copy(name)) {
        return static_cast<int>(port);
      }
    }
  }

  return std::nullopt;
}

DiscoveredRoom make_discovered_room(const Mads::ServiceDiscovery::ServiceInfo &service) {
  DiscoveredRoom room;
  room.name = service.room.empty() ? "(unnamed)" : service.room;
  room.host = service.ip;
  room.encrypted = service.encrypted;
  room.hostname = service.hostname;
  room.version = service.version;

  if (const auto port = find_port(service.ports, {"frontend", "pub", "publish", "publisher"})) {
    room.pub_port = *port;
  }
  if (const auto port = find_port(service.ports, {"backend", "sub", "subscribe", "subscriber"})) {
    room.sub_port = *port;
  }

  return room;
}
#endif

std::vector<std::string> parse_subscribe_topics(const std::string &raw_topics) {
  std::vector<std::string> topics;
  std::stringstream stream(raw_topics);
  std::string item;
  while (std::getline(stream, item, ',')) {
    item = trim_copy(std::move(item));
    if (!item.empty()) {
      topics.push_back(std::move(item));
    }
  }

  if (topics.empty()) {
    topics.push_back("");
  }
  return topics;
}

std::string read_file(const fs::path &path) {
  std::ifstream input(path);
  if (!input) {
    return {};
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

bool write_file(const fs::path &path, const std::string &content) {
  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    return false;
  }
  output << content;
  return static_cast<bool>(output);
}

std::optional<AppSettings> load_settings(const fs::path &path) {
  const std::string raw = read_file(path);
  if (raw.empty()) {
    return std::nullopt;
  }

  try {
    const json parsed = json::parse(raw);
    AppSettings settings;
    settings.host = parsed.value("host", settings.host);
    settings.sub_port = parsed.value("sub_port", settings.sub_port);
    settings.pub_port = parsed.value("pub_port", settings.pub_port);
    settings.subscribe_topics = parsed.value("subscribe_topics", settings.subscribe_topics);
    settings.pub_topic = parsed.value("pub_topic", settings.pub_topic);
    settings.client_private_key_path = parsed.value("client_private_key_path", std::string{});
    settings.broker_public_key_path = parsed.value("broker_public_key_path", std::string{});
    return settings;
  } catch (...) {
    return std::nullopt;
  }
}

bool save_settings(const fs::path &path, const AppSettings &settings) {
  const json serialized = {
      {"host", settings.host},
      {"sub_port", settings.sub_port},
      {"pub_port", settings.pub_port},
      {"subscribe_topics", settings.subscribe_topics},
      {"pub_topic", settings.pub_topic},
      {"client_private_key_path", settings.client_private_key_path},
      {"broker_public_key_path", settings.broker_public_key_path},
  };
  return write_file(path, serialized.dump(2));
}

// Where the settings file lives. If the config directory cannot be used, fall
// back to the legacy file in the working directory.
fs::path resolve_settings_path() {
  try {
    return Mads::detail::config_dir() / kSettingsFileName;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "mads-chat: no config directory (%s); using ./%s\n", error.what(),
                 kLegacySettingsFileName);
    return fs::current_path() / kLegacySettingsFileName;
  }
}

// One-time move of the settings written by older versions from the working
// directory to the config directory. The legacy file is left in place.
void migrate_legacy_settings(const fs::path &settings_path) {
  const fs::path legacy_path = fs::current_path() / kLegacySettingsFileName;
  std::error_code ec;
  if (legacy_path == settings_path || fs::exists(settings_path, ec) || !fs::exists(legacy_path, ec)) {
    return;
  }
  const auto legacy = load_settings(legacy_path);
  if (!legacy.has_value() || !save_settings(settings_path, *legacy)) {
    return;
  }
  std::printf("Settings moved to %s; ./%s is no longer used and can be deleted\n",
              settings_path.string().c_str(), kLegacySettingsFileName);
  std::fflush(stdout); // the GUI may run for hours, or be killed, before exit
}

std::string format_json(const std::string &raw, bool *valid = nullptr, json *parsed_out = nullptr) {
  try {
    json parsed = json::parse(raw);
    if (valid != nullptr) {
      *valid = true;
    }
    if (parsed_out != nullptr) {
      *parsed_out = parsed;
    }
    return parsed.dump(2);
  } catch (...) {
    if (valid != nullptr) {
      *valid = false;
    }
    if (parsed_out != nullptr) {
      *parsed_out = json();
    }
    return raw;
  }
}

void append_line(std::vector<JsonLine> &lines) {
  lines.emplace_back();
}

void append_span(std::vector<JsonLine> &lines, const std::string &text, ImVec4 color) {
  if (lines.empty()) {
    append_line(lines);
  }
  lines.back().push_back(JsonSpan{text, color});
}

std::string indent_string(int depth) {
  return std::string(static_cast<size_t>(depth) * 2U, ' ');
}

void build_json_lines(const json &value, int depth, std::vector<JsonLine> &lines);

void append_primitive(const json &value, std::vector<JsonLine> &lines) {
  if (value.is_string()) {
    append_span(lines, value.dump(), color_string());
    return;
  }
  if (value.is_number()) {
    append_span(lines, value.dump(), color_number());
    return;
  }
  if (value.is_boolean()) {
    append_span(lines, value.dump(), color_bool());
    return;
  }
  if (value.is_null()) {
    append_span(lines, "null", color_null());
    return;
  }
  append_span(lines, value.dump(), color_text());
}

void build_json_object_lines(const json &value, int depth, std::vector<JsonLine> &lines) {
  append_span(lines, "{", color_text());
  if (value.empty()) {
    append_span(lines, "}", color_text());
    return;
  }

  append_line(lines);
  size_t index = 0;
  for (auto it = value.begin(); it != value.end(); ++it, ++index) {
    append_span(lines, indent_string(depth + 1), color_text());
    append_span(lines, json(it.key()).dump(), color_key());
    append_span(lines, ": ", color_text());
    if (it->is_structured()) {
      build_json_lines(*it, depth + 1, lines);
    } else {
      append_primitive(*it, lines);
    }
    if (index + 1 < value.size()) {
      append_span(lines, ",", color_text());
    }
    append_line(lines);
  }
  append_span(lines, indent_string(depth), color_text());
  append_span(lines, "}", color_text());
}

void build_json_array_lines(const json &value, int depth, std::vector<JsonLine> &lines) {
  append_span(lines, "[", color_text());
  if (value.empty()) {
    append_span(lines, "]", color_text());
    return;
  }

  append_line(lines);
  for (size_t index = 0; index < value.size(); ++index) {
    append_span(lines, indent_string(depth + 1), color_text());
    if (value[index].is_structured()) {
      build_json_lines(value[index], depth + 1, lines);
    } else {
      append_primitive(value[index], lines);
    }
    if (index + 1 < value.size()) {
      append_span(lines, ",", color_text());
    }
    append_line(lines);
  }
  append_span(lines, indent_string(depth), color_text());
  append_span(lines, "]", color_text());
}

void build_json_lines(const json &value, int depth, std::vector<JsonLine> &lines) {
  if (value.is_object()) {
    build_json_object_lines(value, depth, lines);
    return;
  }
  if (value.is_array()) {
    build_json_array_lines(value, depth, lines);
    return;
  }
  append_primitive(value, lines);
}

void render_json_lines(const std::vector<JsonLine> &lines) {
  for (const JsonLine &line : lines) {
    bool first = true;
    for (const JsonSpan &span : line) {
      if (!first) {
        ImGui::SameLine(0.0F, 0.0F);
      }
      ImGui::PushStyleColor(ImGuiCol_Text, span.color);
      ImGui::TextUnformatted(span.text.c_str());
      ImGui::PopStyleColor();
      first = false;
    }
    if (line.empty()) {
      ImGui::TextUnformatted("");
    }
  }
}

float calculate_child_height(size_t line_count) {
  const float line_height = ImGui::GetTextLineHeightWithSpacing();
  const float vertical_padding = ImGui::GetStyle().WindowPadding.y * 2.0F;
  return std::max(line_height + vertical_padding, static_cast<float>(line_count) * line_height + vertical_padding);
}

size_t count_lines(std::string_view text) {
  if (text.empty()) {
    return 1U;
  }

  size_t lines = 1U;
  for (char ch : text) {
    if (ch == '\n') {
      ++lines;
    }
  }
  return lines;
}

void render_json_view(const char *id, const json &value) {
  std::vector<JsonLine> lines;
  lines.reserve(64);
  build_json_lines(value, 0, lines);
  ImGui::BeginChild(id,
                    ImVec2(-FLT_MIN, calculate_child_height(lines.size())),
                    true,
                    ImGuiWindowFlags_HorizontalScrollbar);
  render_json_lines(lines);
  ImGui::EndChild();
}

void render_invalid_json_view(const char *id, const std::string &raw) {
  const float height =
      calculate_child_height(count_lines(raw) + 2U);
  ImGui::BeginChild(id, ImVec2(-FLT_MIN, height), true, ImGuiWindowFlags_HorizontalScrollbar);
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90F, 0.43F, 0.43F, 1.0F));
  ImGui::TextWrapped("Invalid JSON");
  ImGui::PopStyleColor();
  ImGui::Separator();
  ImGui::TextUnformatted(raw.c_str());
  ImGui::EndChild();
}

class MadsBridge {
public:
  MadsBridge() = default;

  ~MadsBridge() {
    disconnect();
  }

  bool connect(const AppSettings &settings) {
    clear_error();

    // Stop the receive thread before touching the agent: zmqpp sockets are not
    // thread-safe, so reconfiguring/reconnecting while receive_loop() is calling
    // receive() on the same agent corrupts the heap. join() before, restart after.
    stop_receive_thread();

    try {
      const bool use_crypto = !settings.client_private_key_path.empty() &&
                              !settings.broker_public_key_path.empty();
      if ((!settings.client_private_key_path.empty() || !settings.broker_public_key_path.empty()) &&
          !use_crypto) {
        set_error("Both key files must be selected to enable CURVE.");
        return false;
      }

      std::scoped_lock agent_lock(_agent_mutex);

      if (_agent == nullptr) {
        _agent = std::make_unique<Mads::Agent>(kAppAgentName, "none");
      }

      if (use_crypto) {
        const fs::path client_key_path = fs::path(settings.client_private_key_path);
        const fs::path broker_key_path = fs::path(settings.broker_public_key_path);
        if (!fs::exists(client_key_path) || !fs::exists(broker_key_path)) {
          set_error("Selected key file does not exist.");
          return false;
        }
        if (client_key_path.parent_path() != broker_key_path.parent_path()) {
          set_error("MADS expects client and broker keys in the same directory.");
          return false;
        }
        _agent->set_key_dir(client_key_path.parent_path().string());
        _agent->client_key_name = client_key_path.stem().string();
        _agent->server_key_name = broker_key_path.stem().string();
      }

      if (!_initialized) {
        _agent->init(use_crypto);
        _initialized = true;
      }
      _agent->set_sub_topic(parse_subscribe_topics(settings.subscribe_topics));
      _agent->set_sub_endpoint(make_endpoint_uri(settings.host, settings.sub_port));
      _agent->set_pub_endpoint(make_endpoint_uri(settings.host, settings.pub_port));
      _agent->set_receive_timeout(std::chrono::milliseconds(50));
      _agent->connect();

      _stop_requested.store(false);
      _receive_thread = std::thread(&MadsBridge::receive_loop, this);
      _connected.store(true);
      _ever_connected.store(true);
      return true;
    } catch (const std::exception &error) {
      set_error(error.what());
      _connected.store(false);
      return false;
    }
  }

  void disconnect() {
    stop_receive_thread();

    std::scoped_lock agent_lock(_agent_mutex);
    if (_agent != nullptr) {
      try {
        _agent->disconnect();
      } catch (...) {
      }
      _agent.reset();
    }

    clear_topics();
    _connected.store(false);
    _initialized = false;
  }

  bool is_connected() const {
    return _connected.load();
  }

  bool has_connected_once() const {
    return _ever_connected.load();
  }

  bool publish_message(const std::string &topic, const std::string &payload) {
    // Parse outside the agent lock so a malformed payload never blocks the
    // receive thread, then serialise the publish against receive_loop().
    json parsed;
    try {
      parsed = json::parse(payload);
    } catch (const std::exception &error) {
      set_error(error.what());
      return false;
    }

    std::scoped_lock agent_lock(_agent_mutex);
    if (_agent == nullptr) {
      set_error("Not connected.");
      return false;
    }

    try {
      _agent->publish(std::move(parsed), topic);
      clear_error();
      return true;
    } catch (const std::exception &error) {
      set_error(error.what());
      return false;
    }
  }

  std::map<std::string, TopicState> snapshot_topics() const {
    std::scoped_lock lock(_mutex);
    return _topics;
  }

  void clear_topics() {
    std::scoped_lock lock(_mutex);
    _topics.clear();
  }

  bool delete_topic(const std::string &topic) {
    std::scoped_lock lock(_mutex);
    return _topics.erase(topic) > 0;
  }

  std::optional<std::string> topic_payload(const std::string &topic) const {
    std::scoped_lock lock(_mutex);
    const auto it = _topics.find(topic);
    if (it == _topics.end()) {
      return std::nullopt;
    }
    return it->second.payload;
  }

  void set_topic_expanded(const std::string &topic, bool expanded) {
    std::scoped_lock lock(_mutex);
    if (auto it = _topics.find(topic); it != _topics.end()) {
      it->second.expanded = expanded;
    }
  }

  std::string last_error() const {
    std::scoped_lock lock(_mutex);
    return _last_error;
  }

private:
  void receive_loop() {
    while (!_stop_requested.load()) {
      try {
        std::string topic;
        std::string payload;
        {
          // Serialise agent access against connect()/disconnect()/publish():
          // zmqpp sockets are not thread-safe. receive(true) is non-blocking, so
          // holding the lock here is brief and does not starve the UI thread.
          std::scoped_lock agent_lock(_agent_mutex);
          if (_agent == nullptr) {
            return;
          }
          if (_agent->receive(true) != Mads::message_type::json) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
          }
          std::tie(topic, payload) = _agent->last_message();
        }

        {
          bool valid = false;
          json parsed;
          const std::string pretty = format_json(payload, &valid, &parsed);
          {
            std::scoped_lock lock(_mutex);
            TopicState &state = _topics[topic];
            const auto received_at = std::chrono::steady_clock::now();
            if (state.previous_received_at.has_value()) {
              const double interval_seconds =
                  std::chrono::duration<double>(received_at - *state.previous_received_at).count();
              state.inter_arrival_seconds.push_back(interval_seconds);
              constexpr std::size_t kInterArrivalWindowSize = 10;
              if (state.inter_arrival_seconds.size() > kInterArrivalWindowSize) {
                state.inter_arrival_seconds.erase(state.inter_arrival_seconds.begin());
              }
            }
            state.payload = pretty;
            state.previous_received_at = received_at;
            state.last_received_at = received_at;
            state.json_valid = valid;
            state.parsed_payload = valid ? parsed : json();
          }
          clear_error();
          continue;
        }
      } catch (const std::exception &error) {
        set_error(error.what());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
  }

  // Stops and joins the receive thread. Must be called WITHOUT holding
  // _agent_mutex, since receive_loop() acquires it.
  void stop_receive_thread() {
    _stop_requested.store(true);
    if (_receive_thread.joinable()) {
      _receive_thread.join();
    }
  }

  void set_error(std::string message) {
    std::scoped_lock lock(_mutex);
    _last_error = std::move(message);
  }

  void clear_error() {
    std::scoped_lock lock(_mutex);
    _last_error.clear();
  }

  std::unique_ptr<Mads::Agent> _agent;
  std::thread _receive_thread;
  // Serialises all access to _agent (a zmqpp socket owner, not thread-safe)
  // between the UI thread and the receive thread. Always acquire before _mutex.
  std::mutex _agent_mutex;
  mutable std::mutex _mutex;
  std::map<std::string, TopicState> _topics;
  std::string _last_error;
  std::atomic<bool> _connected{false};
  std::atomic<bool> _ever_connected{false};
  std::atomic<bool> _stop_requested{false};
  bool _initialized = false;
};

#if MADS_CHAT_HAS_SERVICE_DISCOVERY
class RoomDiscovery {
public:
  RoomDiscovery() = default;

  ~RoomDiscovery() {
    stop();
  }

  RoomDiscovery(const RoomDiscovery &) = delete;
  RoomDiscovery &operator=(const RoomDiscovery &) = delete;

  void start() {
    if (_thread.joinable()) {
      return;
    }
    _stop_requested.store(false);
    _thread = std::thread(&RoomDiscovery::discovery_loop, this);
  }

  void stop() {
    _stop_requested.store(true);
    if (_thread.joinable()) {
      _thread.join();
    }
  }

  std::vector<DiscoveredRoom> snapshot_rooms() const {
    std::scoped_lock lock(_mutex);
    return _rooms;
  }

  std::string last_error() const {
    std::scoped_lock lock(_mutex);
    return _last_error;
  }

private:
  void discovery_loop() {
    try {
      Mads::ServiceDiscovery discovery(MADS_SERVICE_PORT);
      while (!_stop_requested.load()) {
        try {
          std::vector<DiscoveredRoom> rooms;
          const auto services = discovery.list_rooms(std::chrono::milliseconds{1000});
          rooms.reserve(services.size());
          for (const auto &[name, service] : services) {
            DiscoveredRoom room = make_discovered_room(service);
            if (room.name.empty() || room.name == "(unnamed)") {
              room.name = name.empty() ? "(unnamed)" : name;
            }
            rooms.push_back(std::move(room));
          }

          {
            std::scoped_lock lock(_mutex);
            _rooms = std::move(rooms);
            _last_error.clear();
          }
        } catch (const std::exception &error) {
          set_error(error.what());
          sleep_in_slices(std::chrono::milliseconds{500});
        }
      }
    } catch (const std::exception &error) {
      set_error(error.what());
    }
  }

  void sleep_in_slices(std::chrono::milliseconds duration) const {
    constexpr auto kSlice = std::chrono::milliseconds{50};
    auto slept = std::chrono::milliseconds{0};
    while (!_stop_requested.load() && slept < duration) {
      std::this_thread::sleep_for(kSlice);
      slept += kSlice;
    }
  }

  void set_error(std::string message) {
    std::scoped_lock lock(_mutex);
    _last_error = std::move(message);
  }

  std::thread _thread;
  mutable std::mutex _mutex;
  std::vector<DiscoveredRoom> _rooms;
  std::string _last_error;
  std::atomic<bool> _stop_requested{false};
};
#endif

struct UiState {
  AppSettings connection_settings;
  MadsBridge bridge;
#if MADS_CHAT_HAS_SERVICE_DISCOVERY
  RoomDiscovery room_discovery;
#endif
  fs::path settings_path = resolve_settings_path();
  std::string publish_topic;
  std::string publish_buffer = "{\n  \"message\": \"hello\"\n}";
  std::string status_message;
#if MADS_CHAT_HAS_SERVICE_DISCOVERY
  std::string selected_room_name;
#endif
  bool publish_json_valid = true;
  json parsed_publish_json;
  TextEditor publish_editor;
  ImFont *monospace_font = nullptr;
  bool settings_dirty = false;
  std::string mads_version;
  std::string mads_prefix;
  MadsGui::Texture logo_texture;
};

void refresh_publish_validation(UiState &ui_state) {
  ui_state.publish_buffer = ui_state.publish_editor.GetText();
  bool valid = false;
  json parsed;
  format_json(ui_state.publish_buffer, &valid, &parsed);
  ui_state.publish_json_valid = valid;
  ui_state.parsed_publish_json = valid ? parsed : json();
}

void configure_publish_editor(UiState &ui_state) {
  ui_state.publish_editor.SetLanguage(TextEditor::Language::Json());
  ui_state.publish_editor.SetTabSize(2);
  ui_state.publish_editor.SetInsertSpacesOnTabs(true);
  ui_state.publish_editor.SetAutoIndentEnabled(true);
  ui_state.publish_editor.SetShowLineNumbersEnabled(false);
  ui_state.publish_editor.SetShowScrollbarMiniMapEnabled(false);
  ui_state.publish_editor.SetShowPanScrollIndicatorEnabled(false);
  ui_state.publish_editor.SetShowMatchingBrackets(true);
  ui_state.publish_editor.SetCompletePairedGlyphs(true);
  ui_state.publish_editor.SetPalette(TextEditor::GetDarkPalette());
  ui_state.publish_editor.SetText(format_json(ui_state.publish_buffer));
  ui_state.publish_editor.SetChangeCallback([&ui_state]() {
    refresh_publish_validation(ui_state);
  }, 100);
  refresh_publish_validation(ui_state);
}

void mark_settings_dirty(UiState &ui_state) {
  ui_state.settings_dirty = true;
}

void persist_settings_if_needed(UiState &ui_state) {
  if (!ui_state.settings_dirty) {
    return;
  }
  save_settings(ui_state.settings_path, ui_state.connection_settings);
  ui_state.settings_dirty = false;
}

#if MADS_CHAT_HAS_SERVICE_DISCOVERY
std::optional<DiscoveredRoom> find_selected_room(const std::vector<DiscoveredRoom> &rooms,
                                                 const std::string &selected_name) {
  if (selected_name.empty()) {
    return std::nullopt;
  }
  const auto it = std::find_if(rooms.begin(), rooms.end(), [&selected_name](const DiscoveredRoom &room) {
    return room.name == selected_name;
  });
  if (it == rooms.end()) {
    return std::nullopt;
  }
  return *it;
}

void apply_room_selection(UiState &ui_state, const DiscoveredRoom &room) {
  AppSettings &settings = ui_state.connection_settings;
  settings.host = room.host;
  if (room.pub_port >= 0) {
    settings.pub_port = room.pub_port;
  }
  if (room.sub_port >= 0) {
    settings.sub_port = room.sub_port;
  }
  ui_state.selected_room_name = room.name;
  mark_settings_dirty(ui_state);

  if (room.encrypted &&
      (settings.client_private_key_path.empty() || settings.broker_public_key_path.empty())) {
    ui_state.status_message =
        "Room uses encrypted traffic. Select the client private key and broker public key.";
    return;
  }
  if (room.pub_port < 0 || room.sub_port < 0) {
    ui_state.status_message = "Selected room, but it did not advertise both frontend and backend ports.";
    return;
  }
  ui_state.status_message = "Selected room '" + room.name + "'.";
}

void draw_room_combo(UiState &ui_state, const std::vector<DiscoveredRoom> &rooms) {
  constexpr ImVec4 kEncryptedRoomColor{0.92F, 0.39F, 0.39F, 1.0F};

  const auto selected_room = find_selected_room(rooms, ui_state.selected_room_name);
  const std::string preview = selected_room.has_value() ? selected_room->name : "Manual";

  if (selected_room.has_value() && selected_room->encrypted) {
    ImGui::PushStyleColor(ImGuiCol_Text, kEncryptedRoomColor);
  }

  const bool opened = ImGui::BeginCombo("##room", preview.c_str());

  if (selected_room.has_value() && selected_room->encrypted) {
    ImGui::PopStyleColor();
  }

  if (!opened) {
    return;
  }

  if (ImGui::Selectable("Manual", ui_state.selected_room_name.empty())) {
    ui_state.selected_room_name.clear();
  }

  if (rooms.empty()) {
    ImGui::BeginDisabled();
    ImGui::Selectable("No rooms found", false);
    ImGui::EndDisabled();
  }

  for (const DiscoveredRoom &room : rooms) {
    ImGui::PushID(room.name.c_str());
    if (room.encrypted) {
      ImGui::PushStyleColor(ImGuiCol_Text, kEncryptedRoomColor);
    }
    const bool selected = ui_state.selected_room_name == room.name;
    if (ImGui::Selectable(room.name.c_str(), selected)) {
      apply_room_selection(ui_state, room);
    }
    if (room.encrypted) {
      ImGui::PopStyleColor();
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s:%d/%d%s",
                        room.host.c_str(),
                        room.pub_port,
                        room.sub_port,
                        room.encrypted ? " encrypted" : "");
    }
    ImGui::PopID();
  }

  ImGui::EndCombo();
}
#endif

void draw_connection_bar(UiState &ui_state) {
  AppSettings &settings = ui_state.connection_settings;
#if MADS_CHAT_HAS_SERVICE_DISCOVERY
  const std::vector<DiscoveredRoom> rooms = ui_state.room_discovery.snapshot_rooms();
  const auto selected_room = find_selected_room(rooms, ui_state.selected_room_name);
#endif

  ImGui::SeparatorText("Connection");
  char host_buffer[256];
  std::snprintf(host_buffer, sizeof(host_buffer), "%s", settings.host.c_str());
  int sub_port = settings.sub_port;
  int pub_port = settings.pub_port;
  char topics_buffer[512];
  std::snprintf(topics_buffer, sizeof(topics_buffer), "%s", settings.subscribe_topics.c_str());

#if MADS_CHAT_HAS_SERVICE_DISCOVERY
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Room:");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(180.0F);
  draw_room_combo(ui_state, rooms);

  ImGui::SameLine();
#endif
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Host:");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(220.0F);
  if (ImGui::InputText("##host", host_buffer, sizeof(host_buffer))) {
    settings.host = host_buffer;
#if MADS_CHAT_HAS_SERVICE_DISCOVERY
    ui_state.selected_room_name.clear();
#endif
    mark_settings_dirty(ui_state);
  }

  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Pub port:");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(110.0F);
  if (ImGui::InputInt("##pub_port", &pub_port, 0, 0)) {
    settings.pub_port = std::max(pub_port, 0);
#if MADS_CHAT_HAS_SERVICE_DISCOVERY
    ui_state.selected_room_name.clear();
#endif
    mark_settings_dirty(ui_state);
  }

  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Sub port:");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(110.0F);
  if (ImGui::InputInt("##sub_port", &sub_port, 0, 0)) {
    settings.sub_port = std::max(sub_port, 0);
#if MADS_CHAT_HAS_SERVICE_DISCOVERY
    ui_state.selected_room_name.clear();
#endif
    mark_settings_dirty(ui_state);
  }

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Subscribe topics:");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::InputText("##subscribe_topics", topics_buffer, sizeof(topics_buffer))) {
    settings.subscribe_topics = topics_buffer;
    mark_settings_dirty(ui_state);
  }

  ImGui::TextDisabled("Sub URI: %s", make_endpoint_uri(settings.host, settings.sub_port).c_str());
  ImGui::SameLine();
  ImGui::TextDisabled("Pub URI: %s", make_endpoint_uri(settings.host, settings.pub_port).c_str());

#if MADS_CHAT_HAS_SERVICE_DISCOVERY
  if (selected_room.has_value() && selected_room->encrypted &&
      (settings.client_private_key_path.empty() || settings.broker_public_key_path.empty())) {
    ImGui::PushStyleColor(ImGuiCol_Text, MadsGui::kErrorColor);
    ImGui::TextWrapped("Room uses encrypted traffic. Select the client private key and broker public key.");
    ImGui::PopStyleColor();
  }
#endif

  if (ImGui::Button("Client Private Key")) {
    const auto result = pfd::open_file("Select client private key", ".", {"Private key", "*.key"}, pfd::opt::none).result();
    if (!result.empty()) {
      settings.client_private_key_path = result.front();
      mark_settings_dirty(ui_state);
    }
  }
  ImGui::SameLine();
  ImGui::TextUnformatted(settings.client_private_key_path.empty() ? "(none)" : settings.client_private_key_path.c_str());

  if (ImGui::Button("Broker Public Key")) {
    const auto result = pfd::open_file("Select broker public key", ".", {"Public key", "*.pub"}, pfd::opt::none).result();
    if (!result.empty()) {
      settings.broker_public_key_path = result.front();
      mark_settings_dirty(ui_state);
    }
  }
  ImGui::SameLine();
  ImGui::TextUnformatted(settings.broker_public_key_path.empty() ? "(none)" : settings.broker_public_key_path.c_str());

  if (ImGui::Button("Unload keys")) {
    if (!settings.client_private_key_path.empty() || !settings.broker_public_key_path.empty()) {
      settings.client_private_key_path.clear();
      settings.broker_public_key_path.clear();
      ui_state.status_message = "Encryption keys unloaded.";
      mark_settings_dirty(ui_state);
    }
  }

  const char *connect_label = ui_state.bridge.has_connected_once() ? "Reconnect" : "Connect";
  if (ImGui::Button(connect_label)) {
    const bool was_connected = ui_state.bridge.is_connected();
    if (ui_state.bridge.connect(settings)) {
      ui_state.status_message = was_connected ? "Reconnected." : "Connected.";
    } else {
      ui_state.status_message = was_connected ? "Reconnect failed." : "Connect failed.";
    }
    persist_settings_if_needed(ui_state);
  }

  ImGui::SameLine();
  ImGui::TextDisabled("%s", ui_state.bridge.is_connected() ? "online" : "offline");

  const std::string bridge_error = ui_state.bridge.last_error();
#if MADS_CHAT_HAS_SERVICE_DISCOVERY
  const std::string discovery_error = ui_state.room_discovery.last_error();
#endif
  if (!bridge_error.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, MadsGui::kErrorColor);
    ImGui::TextWrapped("%s", bridge_error.c_str());
    ImGui::PopStyleColor();
#if MADS_CHAT_HAS_SERVICE_DISCOVERY
  } else if (!discovery_error.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, MadsGui::kErrorColor);
    ImGui::TextWrapped("Room discovery: %s", discovery_error.c_str());
    ImGui::PopStyleColor();
#endif
  } else if (!ui_state.status_message.empty()) {
    ImGui::TextWrapped("%s", ui_state.status_message.c_str());
  }
}

void draw_publish_panel(UiState &ui_state) {
  AppSettings &settings = ui_state.connection_settings;
  ImGui::SeparatorText("Publish");

  char topic_buffer[256];
  std::snprintf(topic_buffer, sizeof(topic_buffer), "%s", settings.pub_topic.c_str());
  ImGui::TextUnformatted("Publish topic:");
  ImGui::SameLine();
  if (ImGui::InputText("##publish_topic", topic_buffer, sizeof(topic_buffer))) {
    settings.pub_topic = topic_buffer;
    ui_state.publish_topic = topic_buffer;
    mark_settings_dirty(ui_state);
  }

  const ImGuiStyle &style = ImGui::GetStyle();
  const float reserved_height =
      ImGui::GetTextLineHeightWithSpacing() +
      ImGui::GetFrameHeightWithSpacing() +
      style.ItemSpacing.y * 2.0F;
  const float editor_height = std::max(140.0F, ImGui::GetContentRegionAvail().y - reserved_height);

  if (ui_state.monospace_font != nullptr) {
    ImGui::PushFont(ui_state.monospace_font);
  }
  ui_state.publish_editor.Render("##publish_json_editor", ImVec2(-FLT_MIN, editor_height), true);

  if (ui_state.monospace_font != nullptr) {
    ImGui::PopFont();
  }

  if (!ui_state.publish_json_valid) {
    ImGui::PushStyleColor(ImGuiCol_Text, MadsGui::kErrorColor);
    ImGui::TextUnformatted("JSON is invalid.");
    ImGui::PopStyleColor();
  } else {
    ImGui::PushStyleColor(ImGuiCol_Text, MadsGui::kOkColor);
    ImGui::TextUnformatted("JSON is valid.");
    ImGui::PopStyleColor();
  }

  const bool can_publish = !settings.pub_topic.empty() &&
                           !ui_state.publish_buffer.empty() &&
                           ui_state.publish_json_valid &&
                           ui_state.bridge.is_connected();

  if (ImGui::Button("Format JSON")) {
    if (ui_state.publish_json_valid) {
      const std::string pretty = format_json(ui_state.publish_buffer);
      ui_state.publish_editor.SetText(pretty);
      refresh_publish_validation(ui_state);
      ui_state.status_message = "JSON formatted.";
    } else {
      ui_state.status_message = "Cannot format invalid JSON.";
    }
  }
  ImGui::SameLine();
  if (!can_publish) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button("Publish")) {
    if (ui_state.bridge.publish_message(settings.pub_topic, ui_state.publish_buffer)) {
      ui_state.status_message = "Message published.";
    } else {
      ui_state.status_message = "Publish failed.";
    }
  }
  if (!can_publish) {
    ImGui::EndDisabled();
  }
}

std::string format_elapsed(std::chrono::steady_clock::time_point time_point) {
  const auto elapsed = std::chrono::steady_clock::now() - time_point;
  const double seconds = std::chrono::duration<double>(elapsed).count();
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(3) << seconds << " s";
  return stream.str();
}

std::string format_seconds(double seconds) {
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(3) << seconds << " s";
  return stream.str();
}

TopicTimingStats compute_topic_timing_stats(const TopicState &state) {
  TopicTimingStats stats;
  stats.sample_count = state.inter_arrival_seconds.size();
  if (stats.sample_count == 0) {
    return stats;
  }

  double sum = 0.0;
  for (const double value : state.inter_arrival_seconds) {
    sum += value;
  }
  stats.mean_seconds = sum / static_cast<double>(stats.sample_count);

  double squared_sum = 0.0;
  for (const double value : state.inter_arrival_seconds) {
    const double delta = value - stats.mean_seconds;
    squared_sum += delta * delta;
  }
  stats.stdev_seconds = std::sqrt(squared_sum / static_cast<double>(stats.sample_count));
  return stats;
}

ImVec4 elapsed_color_for(const TopicTimingStats &stats, double elapsed_seconds) {
  if (stats.sample_count == 0) {
    return color_text();
  }

  if (stats.stdev_seconds <= 0.0) {
    return elapsed_seconds <= stats.mean_seconds ? color_elapsed_normal() : color_elapsed_alert();
  }

  const double sigma_distance = (elapsed_seconds - stats.mean_seconds) / stats.stdev_seconds;
  if (sigma_distance <= 1.5) {
    return color_elapsed_normal();
  }
  if (sigma_distance <= 3.0) {
    return color_elapsed_warning();
  }
  return color_elapsed_alert();
}

void draw_receive_panel(UiState &ui_state) {
  ImGui::SeparatorText("Receive");

  auto topics = ui_state.bridge.snapshot_topics();
  ImGui::BeginChild("receive_topics", ImVec2(-FLT_MIN, -ImGui::GetFrameHeightWithSpacing() * 2.0F), true);
  for (auto &[topic, state] : topics) {
    ImGui::PushID(topic.c_str());
    const char *toggle_label = state.expanded ? "v" : ">";
    const std::string button_label = std::string(toggle_label);
    if (ImGui::SmallButton(button_label.c_str())) {
      state.expanded = !state.expanded;
      ui_state.bridge.set_topic_expanded(topic, state.expanded);
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(topic.c_str());
    bool topic_deleted = false;
    if (ImGui::BeginPopupContextItem("topic_context_menu")) {
      if (ImGui::MenuItem("Copy")) {
        const auto payload = ui_state.bridge.topic_payload(topic);
        if (payload.has_value()) {
          ImGui::SetClipboardText(payload->c_str());
          ui_state.status_message = "Copied topic '" + topic + "'.";
        } else {
          ui_state.status_message = "Topic '" + topic + "' is no longer available.";
        }
      }
      if (ImGui::MenuItem("Delete")) {
        topic_deleted = ui_state.bridge.delete_topic(topic);
        ui_state.status_message =
            topic_deleted ? "Deleted topic '" + topic + "'." : "Topic '" + topic + "' is no longer available.";
      }
      ImGui::EndPopup();
    }
    ImGui::SameLine();
    const double elapsed_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - state.last_received_at).count();
    const TopicTimingStats stats = compute_topic_timing_stats(state);
    const std::string elapsed_label = format_seconds(elapsed_seconds);
    ImGui::TextColored(elapsed_color_for(stats, elapsed_seconds), "%s", elapsed_label.c_str());
    ImGui::SameLine();
    if (stats.sample_count > 0) {
      const std::string stats_label =
          "(" + format_seconds(stats.mean_seconds) + "±" + format_seconds(stats.stdev_seconds) + ")";
      ImGui::TextDisabled("%s", stats_label.c_str());
    } else {
      ImGui::TextDisabled("(n/a±n/a)");
    }

    if (!topic_deleted && state.expanded) {
      ImGui::Indent();
      if (state.json_valid) {
        render_json_view("json_view", state.parsed_payload);
      } else {
        render_invalid_json_view("json_view_invalid", state.payload);
      }
      ImGui::Unindent();
    }
    ImGui::PopID();
  }
  ImGui::EndChild();

  if (ImGui::Button("Clear Topics")) {
    ui_state.bridge.clear_topics();
  }
}

} // namespace

int main() {
  if (!glfwInit()) {
    return 1;
  }

  const char *glsl_version = MadsGui::setup_gl_hints();

  GLFWwindow *window = glfwCreateWindow(MadsGui::kWindowWidth, MadsGui::kWindowHeight, kWindowTitle, nullptr, nullptr);
  if (window == nullptr) {
    glfwTerminate();
    return 1;
  }

  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = "imgui.ini";

  MadsGui::apply_style();

  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init(glsl_version);

  UiState ui_state;
  migrate_legacy_settings(ui_state.settings_path);
  if (const auto stored = load_settings(ui_state.settings_path); stored.has_value()) {
    ui_state.connection_settings = *stored;
  }
  ui_state.publish_topic = ui_state.connection_settings.pub_topic;
  ui_state.monospace_font = MadsGui::load_monospace_font(15.0F);
  ui_state.mads_version = run_command_capture("mads --version");
  ui_state.mads_prefix = run_command_capture("mads --prefix");
  ui_state.logo_texture = MadsGui::load_logo(
      ui_state.mads_prefix.empty() ? fs::path() : fs::path(ui_state.mads_prefix) / "share/images");
  configure_publish_editor(ui_state);
#if MADS_CHAT_HAS_SERVICE_DISCOVERY
  ui_state.room_discovery.start();
#endif

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
    ImGui::SetNextWindowSize(io.DisplaySize);

    ImGui::Begin("MainWindow", nullptr, MadsGui::kMainWindowFlags);
    draw_connection_bar(ui_state);

    const float footer_reserved_height = MadsGui::status_footer_height();
    if (ImGui::BeginChild("content_area", ImVec2(-FLT_MIN, -footer_reserved_height), false)) {
      if (ImGui::BeginTable("main_split", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableNextColumn();
        draw_publish_panel(ui_state);

        ImGui::TableNextColumn();
        draw_receive_panel(ui_state);

        ImGui::EndTable();
      }
    }
    ImGui::EndChild();

    MadsGui::draw_status_footer(
        ui_state.logo_texture,
        "Version: " + (ui_state.mads_version.empty() ? std::string("(unavailable)") : ui_state.mads_version),
        "Prefix: " + (ui_state.mads_prefix.empty() ? std::string("(unavailable)") : ui_state.mads_prefix));
    ImGui::End();

    persist_settings_if_needed(ui_state);

    ImGui::Render();
    int display_width = 0;
    int display_height = 0;
    glfwGetFramebufferSize(window, &display_width, &display_height);
    glViewport(0, 0, display_width, display_height);
    glClearColor(MadsGui::kClearColor.x, MadsGui::kClearColor.y, MadsGui::kClearColor.z, MadsGui::kClearColor.w);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window);
  }

  ui_state.bridge.disconnect();
#if MADS_CHAT_HAS_SERVICE_DISCOVERY
  ui_state.room_discovery.stop();
#endif
  save_settings(ui_state.settings_path, ui_state.connection_settings);
  MadsGui::free_texture(ui_state.logo_texture);

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
