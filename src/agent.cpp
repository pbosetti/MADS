#include "agent.hpp"
#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>
#include <zmqpp/zmqpp.hpp>
#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <filesystem>
#include <regex>
#include <csignal>
#include <snappy.h>
#include <string>
#include <string_view>
#include <future>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include "curve.hpp"
#include "exec_path.hpp"
#include "mads.hpp"

#ifndef MADS_AGENT_NO_INFO
#include <rang.hpp>
using namespace rang;
#endif

using namespace std::string_view_literals;
using namespace std::string_literals;
using namespace zmqpp;
using namespace std;
using namespace std::chrono;

namespace Mads {

/*
 __        ___              __                            _
 \ \      / (_)_ __ ___    / _| ___  _ __ _ __ ___   __ _| |_
  \ \ /\ / /| | '__/ _ \  | |_ / _ \| '__| '_ ` _ \ / _` | __|
   \ V  V / | | | |  __/  |  _| (_) | |  | | | | | | (_| | |_
    \_/\_/  |_|_|  \___|  |_|  \___/|_|  |_| |_| |_|\__,_|\__|

Self-describing frame header (see REFACTOR.md §1.3 / MSGPACK.md §5.1).

A message published in the *extended* format carries a small header part right
after the topic:

  [ topic ] [ header ] [ payload ]            (data)
  [ topic ] [ header ] [ meta ] [ raw bytes ] (blob, has_blob flag set)

The header begins with the 4-byte magic "MADS" and has a fixed size, which makes
it reliably distinguishable from legacy frames:

  - legacy data:  [ topic ] [ snappy(json) ]               (2 parts)
  - legacy blob:  [ topic ] [ json meta ] [ raw bytes ]    (3 parts)

A reader first checks part #1 for the magic + exact size; if absent it falls
back to the legacy part-count interpretation. Legacy peers never see a header
because the header is only emitted for non-default (e.g. MsgPack) formats.
*/
namespace {

constexpr char WIRE_MAGIC[4] = {'M', 'A', 'D', 'S'};
constexpr uint8_t WIRE_HDR_VERSION = 1;
constexpr uint8_t WIRE_FLAG_BLOB = 0x01;
constexpr size_t WIRE_HEADER_SIZE = 4 /*magic*/ + 1 /*ver*/ + 1 /*format*/ +
                                    1 /*compression*/ + 1 /*flags*/ +
                                    4 /*schema*/;

enum class Comp : uint8_t { None = 0, Snappy = 1 };

struct WireHeader {
  uint8_t hdr_version = WIRE_HDR_VERSION;
  uint8_t format = static_cast<uint8_t>(WireFormat::Json);
  uint8_t compression = static_cast<uint8_t>(Comp::None);
  bool has_blob = false;
  uint32_t schema = LIB_VERSION_NUM;
};

string make_wire_header(WireFormat fmt, Comp comp, bool has_blob) {
  string h;
  h.reserve(WIRE_HEADER_SIZE);
  h.append(WIRE_MAGIC, 4);
  h.push_back(static_cast<char>(WIRE_HDR_VERSION));
  h.push_back(static_cast<char>(fmt));
  h.push_back(static_cast<char>(comp));
  h.push_back(static_cast<char>(has_blob ? WIRE_FLAG_BLOB : 0));
  uint32_t schema = LIB_VERSION_NUM;
  h.push_back(static_cast<char>((schema >> 24) & 0xFF));
  h.push_back(static_cast<char>((schema >> 16) & 0xFF));
  h.push_back(static_cast<char>((schema >> 8) & 0xFF));
  h.push_back(static_cast<char>(schema & 0xFF));
  return h;
}

// Returns true and fills `out` if `part` is a valid frame header.
bool parse_wire_header(const string &part, WireHeader &out) {
  if (part.size() != WIRE_HEADER_SIZE)
    return false;
  if (std::memcmp(part.data(), WIRE_MAGIC, 4) != 0)
    return false;
  out.hdr_version = static_cast<uint8_t>(part[4]);
  out.format = static_cast<uint8_t>(part[5]);
  out.compression = static_cast<uint8_t>(part[6]);
  out.has_blob = (static_cast<uint8_t>(part[7]) & WIRE_FLAG_BLOB) != 0;
  out.schema = (static_cast<uint32_t>(static_cast<uint8_t>(part[8])) << 24) |
               (static_cast<uint32_t>(static_cast<uint8_t>(part[9])) << 16) |
               (static_cast<uint32_t>(static_cast<uint8_t>(part[10])) << 8) |
               static_cast<uint32_t>(static_cast<uint8_t>(part[11]));
  return true;
}

// Resolve a compression policy to the concrete codec for a payload of the
// given size. Compression::Auto compresses only at/above the threshold.
Comp resolve_compression(Compression policy, size_t size) {
  switch (policy) {
  case Compression::None:
    return Comp::None;
  case Compression::Snappy:
    return Comp::Snappy;
  case Compression::Auto:
  default:
    return size >= COMPRESSION_AUTO_THRESHOLD ? Comp::Snappy : Comp::None;
  }
}

// Encode a JSON object into the bytes for the given wire format.
string encode_payload(const nlohmann::json &j, WireFormat fmt) {
  if (fmt == WireFormat::MsgPack) {
    auto v = nlohmann::json::to_msgpack(j);
    return string(reinterpret_cast<const char *>(v.data()), v.size());
  }
  return j.dump();
}

// Materialise an encoded payload into JSON *text* (the representation the rest
// of MADS expects). Returns false on any decompression/decoding failure.
bool decode_to_json_text(const string &raw, uint8_t format, uint8_t comp,
                         string &json_text_out) {
  const string *bytes = &raw;
  string uncompressed;
  if (comp == static_cast<uint8_t>(Comp::Snappy)) {
    if (!snappy::Uncompress(raw.data(), raw.size(), &uncompressed))
      return false;
    bytes = &uncompressed;
  }
  if (format == static_cast<uint8_t>(WireFormat::MsgPack)) {
    try {
      nlohmann::json j = nlohmann::json::from_msgpack(*bytes);
      json_text_out = j.dump();
    } catch (...) {
      return false;
    }
  } else {
    // Already JSON text (possibly after decompression).
    json_text_out = *bytes;
  }
  return true;
}

// Decode an encoded payload into a LazyPayload WITHOUT forcing a conversion:
// MsgPack frames keep their decoded object, JSON frames keep their text. The
// other representation is materialised later only if a consumer needs it.
// Returns nullopt on any decompression/decoding failure.
std::optional<LazyPayload> decode_to_payload(const string &raw, uint8_t format,
                                             uint8_t comp) {
  const string *bytes = &raw;
  string uncompressed;
  if (comp == static_cast<uint8_t>(Comp::Snappy)) {
    if (!snappy::Uncompress(raw.data(), raw.size(), &uncompressed))
      return std::nullopt;
    bytes = &uncompressed;
  }
  if (format == static_cast<uint8_t>(WireFormat::MsgPack)) {
    try {
      return LazyPayload::from_doc(nlohmann::json::from_msgpack(*bytes));
    } catch (...) {
      return std::nullopt;
    }
  }
  // JSON: keep the text; move out of the decompression buffer when possible.
  if (bytes == &uncompressed)
    return LazyPayload::from_text(std::move(uncompressed));
  return LazyPayload::from_text(raw);
}

// Process-global signal-handler guard (REFACTOR.md §1.6).
std::once_flag g_signal_once;

} // namespace

unique_ptr<Agent> start_agent(string name, string settings_uri,
                              map<string, string> crypto_settings) {
  bool crypto = !crypto_settings.empty();
  if (crypto) {
    if (!crypto_settings.contains("key_dir") ||
        !crypto_settings.contains("key_client") ||
        !crypto_settings.contains("key_broker")) {
      throw AgentError("Wrong crypto settings: key_dir, key_client, and "
                       "key_broker are required");
    }
  }
  unique_ptr<Agent> agent = make_unique<Agent>(name, settings_uri);
  if (crypto) {
    agent->set_key_dir(crypto_settings.at("key_dir"));
    agent->client_key_name = crypto_settings.at("key_client");
    agent->server_key_name = crypto_settings.at("key_broker");
  }
  agent->init(crypto);
  agent->connect();
  return agent;
}

// Private methods implementations

void Agent::setup_curve_on(zmqpp::socket &socket) {
  if (!_curve_auth)
    return;
  if (_curve_auth->client_public_key().empty() ||
      _curve_auth->client_secret_key().empty() ||
      _curve_auth->server_public_key().empty()) {
    _curve_auth->setup_curve_client(socket, client_key_name, server_key_name);
  } else {
    _curve_auth->setup_curve_client(socket);
  }
}

tuple<string, filesystem::path, double>
Agent::query_broker(string uri, string name, int timeout) {
  // Single REQ socket reused for both the settings and timecode round-trips.
  zmqpp::socket socket(_context, zmqpp::socket_type::req);
  setup_curve_on(socket);
  // Drop any undelivered request on close: with the default infinite linger,
  // a request queued toward an unreachable broker keeps the context alive and
  // context termination (Agent shutdown) blocks forever.
  socket.set(zmqpp::socket_option::linger, 0);
  if (timeout > 0) {
    socket.set(zmqpp::socket_option::receive_timeout, timeout);
    socket.set(zmqpp::socket_option::send_timeout, timeout);
  }
  socket.connect(uri);

  // ---- settings request ----
  message msg_out, msg_in;
  msg_out << LIB_VERSION << "settings" << name;
  if (!socket.send(msg_out)) {
    socket.close();
    throw AgentError("Timed out in sending settings request to broker");
  }
  if (!socket.receive(msg_in)) {
    socket.close();
    throw AgentError("Timed out in receiving settings from broker");
  }
  if (msg_in.parts() < 2) {
    socket.close();
    throw AgentError(
        "Broker refuses to provide settings, check for version mismatch or "
        "missing settings for agent '" +
        name + "'");
  }
  string version_str = msg_in.get(0);
  if (!Mads::check_version(version_str)) {
    socket.close();
    throw AgentError("Received settings from broker with wrong version: " +
                     version_str);
  }
  string raw_settings = msg_in.get(1);
  filesystem::path attachment;
  if (msg_in.parts() == 3) {
    auto tmp_mads_dir = filesystem::temp_directory_path() / "mads";
    if (!filesystem::exists(tmp_mads_dir)) {
      if (!filesystem::create_directory(tmp_mads_dir)) {
        socket.close();
        throw AgentError(
            "Failed to create temporary directory for attachments");
      }
    }
    auto tmp_file = tmp_mads_dir / (name + ".plugin");
    ofstream ofs(tmp_file, ios::out | ios::binary);
    if (!ofs) {
      socket.close();
      throw AgentError(
          "Failed to open temporary file for writing attachment from broker");
    }
    ofs.write(msg_in.get(2).data(), msg_in.get(2).size());
    if (!ofs.good()) {
      socket.close();
      throw AgentError(
          "Failed to write attachment from broker to temporary file");
    }
    ofs.close();
    attachment = tmp_file;
  }

  // ---- timecode request (same socket) ----
  chrono::system_clock::time_point now = chrono::system_clock::now();
  message tc_out, tc_in;
  tc_out << string("v") + LIB_VERSION << "timecode";
  socket.send(tc_out);
  if (!socket.receive(tc_in)) {
    socket.close();
    throw AgentError("Timed out in receiving timecode from broker");
  }
  double broker_tc = std::stod(tc_in.get(0));
  double timecode_offset = broker_tc - timecode(now, timecode_fps);

  socket.disconnect(uri);
  socket.close();
  return make_tuple(raw_settings, attachment, timecode_offset);
}

// Public methods implementations

Agent::Agent(string name, string settings_uri)
    : _settings_uri(settings_uri), _context(),
      _publisher(_context, socket_type::pub),
      _subscriber(_context, socket_type::sub) {

  char hostname[HOST_NAME_MAX];
  if (gethostname(hostname, HOST_NAME_MAX)) {
    _hostname = "unknown";
  } else {
    _hostname = hostname;
  }
#ifdef _WIN32
  _name = name.substr(name.find_last_of("\\") + 1);
  _name = _name.substr(0, _name.find("."));
#else
  _name = name.substr(name.find_last_of("/") + 1);
#endif
  // If the command name has a prefix (e.g. "mads-"), remove it
  size_t pos = _name.rfind('-');
  if (pos != std::string::npos) {
    _name = _name.substr(pos + 1);
  } 
  _key_dir = filesystem::path(Mads::exec_dir()) / "../etc";
}

void Agent::init(string name, string settings_uri, bool crypto, filesystem::path const &key_dir, bool install_watchdog) {
  size_t pos = name.rfind('-');
  if (pos != std::string::npos) {
    _name = name.substr(pos + 1);
  } else {
    _name = name;
  }
  _settings_uri = settings_uri;
  if (!key_dir.empty()) {
    _key_dir = key_dir;
  }
  init(crypto, install_watchdog);
}

void Agent::init(bool crypto, bool install_watchdog) {
  if (crypto) {
    if (!_curve_auth) {
      setup_crypto(auth_verbose);
      _curve_auth->set_key_dir(_key_dir);
      _curve_auth->setup_curve_client(_subscriber, client_key_name, server_key_name);
      _curve_auth->setup_curve_client(_publisher, client_key_name, server_key_name);
    } else {
      _curve_auth->setup_curve_client(_subscriber);
      _curve_auth->setup_curve_client(_publisher);
    }
  }
  if (_settings_uri.empty()) {
    throw AgentError("Settings URI cannot be empty");
  }
  if (_settings_uri == "none") {
    stringstream ss;
    ss << "[" << _name << "]\n";
    ss << "pub_topic = \"" << _name << "\"\n";
    ss << "sub_topic = [\"\"]\n";
    _config = toml::parse(ss.str());
  }
  // Load config URI/file
  else if (settings_are_local()) {
    _config = (toml::table)toml::parse_file(_settings_uri);
  } else {
    auto received = query_broker(_settings_uri, _name, _settings_timeout);
    _raw_settings = get<0>(received);
    _attachment_path = get<1>(received);
    _timecode_offset = get<2>(received);
    _config = (toml::table)toml::parse(_raw_settings);
  }
  // member variables
  auto all_cfg = _config["agents"];
  timecode_fps = all_cfg["timecode_fps"].value_or(MADS_FPS);
  dummy = all_cfg["dummy"].value_or(false);

  if (_config[_name].type() != toml::node_type::table)
    throw AgentError("Invalid settings file: missing '" + _name +
                     "' section");

  auto cfg = _config[_name];
  _pub_endpoint = all_cfg["frontend_address"].value_or(FRONTEND_URI);
  _sub_endpoint = all_cfg["backend_address"].value_or(BACKEND_URI);
  if (!settings_are_local()) {
    // change the localhost to actual IP address
    auto s_uri = split_URL(_settings_uri);
    auto f_uri = split_URL(_pub_endpoint);
    auto b_uri = split_URL(_sub_endpoint);
    _pub_endpoint = get<0>(f_uri) + get<1>(s_uri) + ":" + get<2>(f_uri);
    _sub_endpoint = get<0>(b_uri) + get<1>(s_uri) + ":" + get<2>(b_uri);
  }
  _pub_topic = cfg["pub_topic"].value_or(_name);
  if (cfg["sub_topic"].type() == toml::node_type::string) {
    _sub_topic.push_back(cfg["sub_topic"].value_or(""));
  } else if (cfg["sub_topic"].type() == toml::node_type::array) {
    toml::array *a = cfg["sub_topic"].as_array();
    a->for_each([&](auto &&el) { _sub_topic.push_back(el.value_or("")); });
  } else if (cfg["sub_topic"].type() == toml::node_type::none) {
    // _sub_topic.push_back("-");
    _sub_topic.clear();
  } else {
    throw AgentError("Invalid sub_topic type for " + _name);
  }
  _time_step = chrono::milliseconds(cfg["time_step"].value_or(0));
  // Finer-grained override: if time_step_us is present, it wins over time_step.
  if (cfg["time_step_us"].type() != toml::node_type::none) {
    _time_step = chrono::microseconds(cfg["time_step_us"].value_or<int64_t>(0));
  }
  _high_res_loop = cfg["high_res_loop"].value_or(false);
  _spin_margin =
      chrono::microseconds(cfg["spin_margin_us"].value_or<int64_t>(200));

  // Wire format / compression policy for outgoing messages (opt-in).
  // Both can be set fleet-wide under [agents] and overridden per agent section.
  {
    string default_wf = all_cfg["wire_format"].value_or(string("json"));
    string wf = cfg["wire_format"].value_or(default_wf);
    if (wf == "msgpack" || wf == "MsgPack")
      _wire_format = WireFormat::MsgPack;
    else
      _wire_format = WireFormat::Json;
    string default_comp = all_cfg["compression"].value_or(string("auto"));
    string comp = cfg["compression"].value_or(default_comp);
    if (comp == "none")
      _compression = Compression::None;
    else if (comp == "snappy")
      _compression = Compression::Snappy;
    else
      _compression = Compression::Auto;
  }

  set_high_watermark((int64_t)cfg["queue_size"].value_or<int>(1000));

  // rename attachment if not a plugin
  if (!_attachment_path.empty()) {
    string ext = cfg["attachment_ext"].value_or("plugin");
    if (ext.rfind('.', 0) == 0) {
      ext = ext.substr(1); // remove leading dot
    }
    auto saved_attach = _attachment_path;
    _attachment_path.replace_extension(ext);
    filesystem::rename(saved_attach, _attachment_path);
  }

  if (install_watchdog) {
    install_loop_watchdog();
  }

  load_settings();

  // Cache the JSON projection of the settings: they are effectively immutable
  // after init(), so there is no need to re-serialise TOML -> string -> JSON on
  // every register_event()/info call (REFACTOR.md §2.3).
  if (_config[_name].is_table()) {
    stringstream ss;
    ss << toml::json_formatter{*_config[_name].as_table()};
    _settings_json = nlohmann::json::parse(ss.str());
  }

  _init_done = true;
}

void Agent::load_settings() {}

Agent::~Agent() {
  shutdown();
}

void Agent::shutdown() {
  if (_shutdown_done) return;
  _shutdown_done = true;

  // 0. Stop the watchdog first so that raising the stop request below does
  //    not trip its force-exit countdown during an orderly shutdown.
  _watchdog_stop = true;
  if (_watchdog_thread.joinable()) _watchdog_thread.join();

  // 1. Signal this agent's threads to stop (other agents sharing the
  //    Runtime are unaffected)
  _stopping = true;

  // 1b. Wake and join the delayed startup-event publisher while the sockets
  //     are still open, so it can never touch a dead agent.
  _event_cv.notify_all();
  if (_startup_event_thread.joinable())
    _startup_event_thread.join();

  // 2. Unblock any cv.wait() in receive_raw() LKV mode
  {
    std::lock_guard<std::mutex> lock(_latest_message.mtx);
    _latest_message.cv.notify_all();
  }

  // 3. Join background threads (bounded by their receive timeouts)
  if (_drain_thread.joinable()) _drain_thread.join();
  if (_rc_thread.joinable()) _rc_thread.join();

  // 4. Disconnect sockets
  if (_init_done && _connected) {
    try {
      _publisher.disconnect(_pub_endpoint);
      _subscriber.disconnect(_sub_endpoint);
    } catch (...) {}
    _connected = false;
  }

  // 5. Close sockets and terminate context
  // Set linger to 0 so zmq_ctx_term() doesn't block on undelivered messages.
  // Default linger is -1 (infinite), which causes the context to block forever
  // if any messages remain in the send buffer after disconnect().
  _curve_auth = nullptr;
  try { _publisher.set(zmqpp::socket_option::linger, 0); } catch (...) {}
  try { _subscriber.set(zmqpp::socket_option::linger, 0); } catch (...) {}
  try { _publisher.close(); } catch (...) {}
  try { _subscriber.close(); } catch (...) {}
  try { _context.terminate(); } catch (...) {}
}

void Agent::install_loop_watchdog(uint8_t max_count) {
  // Cooperative failsafe (REFACTOR.md §1.5): when the agent is asked to stop
  // (Runtime stopped or shutdown requested) the main thread is expected to
  // leave loop() and run shutdown(), which sets
  // _watchdog_stop and joins this thread promptly. Only if the orderly path
  // does NOT complete within the bounded grace period do we force-exit, and we
  // use quick_exit() to skip static destructors that might themselves hang.
  _watchdog_stop = false;
  _watchdog_thread = thread([this, max_count]() {
    using namespace std::chrono;
    bool counting = false;
    steady_clock::time_point deadline{};
    while (!_watchdog_stop) {
      if (!keep_running()) {
        if (!counting) {
          counting = true;
          std::signal(SIGINT, SIG_DFL);
          deadline = steady_clock::now() + seconds(2 * max_count);
#ifndef MADS_AGENT_NO_INFO
          cerr << style::italic << "\nWaiting for all resources to close"
               << " (or CTRL-C again to force)..." << style::reset << endl;
#endif
        }
        if (steady_clock::now() >= deadline) {
          // Force exit, skipping static destructors that might themselves hang.
          std::_Exit(EXIT_SUCCESS);
        }
      }
      this_thread::sleep_for(milliseconds(200));
    }
  });
}

nlohmann::json Agent::get_settings() {
  // Return the cached projection computed in init(). Fall back to computing it
  // on demand if called before initialization completed.
  if (!_settings_json.is_null())
    return _settings_json;
  if (_config[_name].is_table()) {
    stringstream ss;
    ss << toml::json_formatter{*_config[_name].as_table()};
    _settings_json = nlohmann::json::parse(ss.str());
  }
  return _settings_json;
}

#ifndef MADS_AGENT_NO_INFO
void Agent::info(ostream &out) {
  if (!_init_done)
    throw AgentError("Agent not initialized");
  out << style::bold << "Agent: " << fg::green << _name << fg::reset
      << style::reset << " (ID: " << _agent_id << ")" << endl;
  if (_crypto) {
    out << fg::cyan << "  CURVE encryption enabled" << endl
        << "    keys dir:         " << _key_dir.string() << endl
        << "    server key name:  " << server_key_name << "(.pub)" << endl
        << "    client key name:  " << client_key_name << "(.key|.pub)"
        << fg::reset << endl;
  }
  if (_cross)
    out << fg::magenta << "  WARNING:" << style::bold
        << "          Cross-socket operation (no broker)" << style::reset
        << fg::reset << endl;
  out << "  Settings file:    " << style::bold << _settings_uri
      << style::reset << endl;
  out << "  Pub endpoint:     " << style::bold
      << (_cross ? _sub_endpoint : _pub_endpoint) << style::reset << endl;
  out << "  Pub topic:        " << style::bold << _pub_topic << style::reset
      << endl;
  out << "  Sub endpoint:     " << style::bold
      << (_cross ? _pub_endpoint : _sub_endpoint) << style::reset << endl;
  out << "  Sub topics:       " << style::bold;
  for (auto &t : _sub_topic) {
    if (t.empty())
      out << fg::magenta << "(all) " << fg::reset;
    else
      out << t << " ";
  }
  out << style::reset << endl;
  // TODO: See down below for conflate not working
  out << "  Queue size:       " << style::bold
      << high_watermark() << " messages" << style::reset << endl;
  out << "  Wire format:      " << style::bold
      << (_wire_format == WireFormat::MsgPack ? "msgpack" : "json")
      << style::reset << " / "
      << (_compression == Compression::None
              ? "no compression"
              : _compression == Compression::Snappy ? "snappy"
                                                    : "snappy (auto)")
      << endl;
  if (!_agent_id.empty()) {
    out << "  Agent ID:         " << style::bold << _agent_id << style::reset
        << endl;
  }
  out << "  Timecode FPS:     " << style::bold << timecode_fps << style::reset
      << endl;
  out << "  Timecode offset:  " << style::bold << _timecode_offset
      << " s" << style::reset << endl;
  if (!_attachment_path.empty()) {
    out << "  Attachment:       " << style::bold
        << _attachment_path.string() << style::reset << endl;
  }
}
#endif

void Agent::connect(chrono::milliseconds delay) {
  if (!_init_done)
    throw AgentError("Agent not initialized");
  // A previous disconnect()/shutdown() must not keep a reconnected agent's
  // loops from running (a process-wide stop still does).
  _stopping = false;
  if (_connected) {
    try {
      _publisher.disconnect(_pub_endpoint);
      _subscriber.disconnect(_sub_endpoint);
    } catch (...) {
      // NOOP
    } 
  }
  if (!_pub_topic.empty()) {
    connect_pub(delay);
    _connected = true;
  }
  if (!_sub_topic.empty()) {
    connect_sub();
    _connected = true;
  }
}

void Agent::disconnect() {
  if (!_init_done)
    throw AgentError("Agent not initialized");
  if (!_connected)
    return;

  // Stop only this agent; other agents sharing the Runtime are unaffected.
  _stopping = true;

  // Unblock LKV cv.wait()
  {
    std::lock_guard<std::mutex> lock(_latest_message.mtx);
    _latest_message.cv.notify_all();
  }

  // Join background threads before touching sockets. The startup-event
  // publisher is woken early (it waits on _event_cv with _stopping as the
  // predicate) so the join is prompt.
  _event_cv.notify_all();
  if (_startup_event_thread.joinable()) _startup_event_thread.join();
  if (_drain_thread.joinable()) _drain_thread.join();
  if (_rc_thread.joinable()) _rc_thread.join();

  try {
    _publisher.disconnect(_pub_endpoint);
    _subscriber.disconnect(_sub_endpoint);
  } catch (...) {
    // NOOP
  }
  _connected = false;
}

void Agent::set_cross(bool cross) { _cross = cross; }

void Agent::set_pub_topic(string topic) { _pub_topic = topic; }

void Agent::register_event(const event_type event, const nlohmann::json &info,
                      const string &info_name) {
  if (!_init_done)
    throw AgentError("Agent not initialized");
  nlohmann::json settings = get_settings();

  auto build_payload = [&]() {
    nlohmann::json payload;
    payload["name"] = _name;
    payload["version"] = LIB_VERSION;
    payload["event"] = event_map.at(event);
    payload["timecode_offset"] = _timecode_offset;
    payload["settings_path"] = _settings_uri;
    payload["settings"] = settings;
    if (!_agent_id.empty()) {
      payload["agent_id"] = _agent_id;
    }
    if (!info.empty()) {
      payload[info_name] = info;
    }
    return payload;
  };

  // Only startup/shutdown need the timing offset that the delayed thread
  // provides. Every other event is published synchronously, avoiding a thread
  // (and a full settings-copy) per event (REFACTOR.md §2.4).
  if (event != event_type::startup && event != event_type::shutdown) {
    publish(build_payload(), METADATA_TOPIC);
    return;
  }

  auto event_body = [info, info_name, event, settings, this]() {
    if (event == event_type::startup) {
      // Delay the startup event, but stay wakeable: shutdown() raises
      // _stopping and notifies _event_cv, so the publish happens (early)
      // while the sockets are still open instead of on a dead agent.
      unique_lock<mutex> lock(_event_mtx);
      _event_cv.wait_for(lock,
                         chrono::milliseconds(STARTUP_SHUTDOWN_DELAY_MS),
                         [this]() { return _stopping.load(); });
    }
    nlohmann::json payload;
    payload["name"] = _name;
    payload["version"] = LIB_VERSION;
    payload["event"] = event_map.at(event);
    payload["timecode_offset"] = _timecode_offset;
    payload["settings_path"] = _settings_uri;
    payload["settings"] = settings;
    if (!_agent_id.empty()) {
      payload["agent_id"] = _agent_id;
    }
    if (!info.empty()) {
      payload[info_name] = info;
    }
    publish(payload, METADATA_TOPIC);
  };
  if (event == event_type::shutdown) {
    thread t(event_body);
    // wait for the thread to publish the message
    this_thread::sleep_for(chrono::milliseconds(STARTUP_SHUTDOWN_DELAY_MS));
    t.join();
  } else {
    // The startup publisher is owned by the agent and joined in shutdown();
    // detaching it here was a use-after-free for agents destroyed within
    // the delay window.
    if (_startup_event_thread.joinable())
      _startup_event_thread.join();
    _startup_event_thread = thread(event_body);
  }
}

void Agent::publish(nlohmann::json payload, string topic) {
  if (!_init_done)
    throw AgentError("Agent not initialized");
  message message;
  int32_t offset = 0;
  if (payload.contains("event"))
    if (payload["event"] == event_map.at(event_type::shutdown) ||
        payload["event"] == event_map.at(event_type::startup)) {
      offset = STARTUP_SHUTDOWN_DELAY_MS;
    }
  chrono::system_clock::time_point now = chrono::system_clock::now();
  // Only stamp fields the caller has not already provided (REFACTOR.md §3.2).
  if (!payload.contains("agent_id")) {
    payload["agent_id"] = _agent_id;
  }
  if (!payload.contains("hostname")) {
    payload["hostname"] = _hostname;
  }
  if (!payload.contains("timestamp")) {
    payload["timestamp"]["$date"] = get_ISODate_time(now, -offset);
  }
  if (!payload.contains("timecode")) {
    payload["timecode"] = timecode(now, timecode_fps) - (offset / 1000.0);
  }
  if (topic.empty()) {
    topic = _pub_topic;
  }
  string body = encode_payload(payload, _wire_format);
  Comp comp = resolve_compression(_compression, body.size());
  string out;
  if (comp == Comp::Snappy) {
    snappy::Compress(body.data(), body.size(), &out);
  } else {
    out = std::move(body);
  }
  // A legacy header-less frame is only safe when it is snappy-compressed JSON
  // (the receiver assumes that for 2-part frames). Any other combination must
  // carry the self-describing header.
  if (_wire_format == WireFormat::MsgPack || comp != Comp::Snappy) {
    message << topic << make_wire_header(_wire_format, comp, false) << out;
  } else {
    message << topic << out; // [topic][snappy(json)]
  }
  {
    std::lock_guard<std::mutex> lock(_publish_mutex);
    _publisher.send(message);
  }
}

void Agent::publish(const char *payload, size_t len,
           nlohmann::json meta, string topic) {
  if (!_init_done)
    throw AgentError("Agent not initialized");
  message message;
  chrono::system_clock::time_point now = chrono::system_clock::now();
  if (!meta.contains("timestamp"))
    meta["timestamp"]["$date"] = get_ISODate_time(now);
  if (!meta.contains("timecode"))
    meta["timecode"] = timecode(now, timecode_fps);
  if (!meta.contains("agent_id"))
    meta["agent_id"] = _agent_id;
  if (!meta.contains("hostname"))
    meta["hostname"] = _hostname;
  if (topic.empty())
    topic = _pub_topic;
  if (_wire_format == WireFormat::MsgPack) {
    // [topic][header(has_blob)][msgpack(meta)][raw bytes]. Metadata is small,
    // so it is left uncompressed.
    message << topic << make_wire_header(WireFormat::MsgPack, Comp::None, true)
            << encode_payload(meta, WireFormat::MsgPack);
  } else {
    // Legacy blob frame: [topic][json meta][raw bytes].
    message << topic << meta.dump();
  }
  message.add_raw(payload, len);
  {
    std::lock_guard<std::mutex> lock(_publish_mutex);
    _publisher.send(message);
  }
}

void Agent::publish(const vector<unsigned char> &payload,
           nlohmann::json meta, string topic) {
  if (!_init_done)
    throw AgentError("Agent not initialized");
  if (!meta.contains("agent_id"))
    meta["agent_id"] = _agent_id;
  if (!meta.contains("hostname"))
    meta["hostname"] = _hostname;
  const char *data = reinterpret_cast<const char *>(payload.data());
  publish(data, payload.size(), meta, topic);
}


inline bool Agent::receive_raw(message &message, bool dont_block) {
  bool r = false;
  // LKV semantic: try and fetch the value from the drain thread
  // behaves as blocking
  if (_last_value_only) {
    std::unique_lock<std::mutex> lock(_latest_message.mtx);
    // TODO: check why the overload with predicate takes 100% CPU
    // _latest_message.cv.wait(lock, [&]() -> bool {
    //   return true;
    // });
    _latest_message.cv.wait(lock, [&] {
       return _latest_message.value.has_value() || !keep_running() || dont_block;
    });
    if (_latest_message.value.has_value()) {
      message = _latest_message.value.value().copy();
      _latest_message.value.reset();
      r = true;
    }
  } 
  // Queued operation (blocking or not)
  else {
    r = _subscriber.receive(message, dont_block);
  }
  return r;
}


message_type Agent::receive(bool dont_block) {
  if (!_init_done)
    throw AgentError("Agent not initialized");
  // Concurrency guard (REFACTOR.md §1.7): when threaded remote control owns the
  // subscriber socket, the application must not also call receive().
  if (_rc_owns_socket)
    throw AgentError("receive() cannot be used while threaded remote control "
                     "owns the subscriber socket");
  message message;
  if (!receive_raw(message, dont_block)) {
    return message_type::none;
  }

  const size_t parts = message.parts();
  // Malformed frames from the network are dropped, never fatal (§1.2).
  if (parts < 2) {
    _dropped_messages++;
    return message_type::none;
  }

  string topic = message.get(0);

  // Extended, self-describing frame? (§1.3)
  WireHeader hdr;
  if (parse_wire_header(message.get(1), hdr)) {
    if (hdr.has_blob) {
      if (parts < 4) {
        _dropped_messages++;
        return message_type::none;
      }
      string meta_text;
      if (!decode_to_json_text(message.get(2), hdr.format, hdr.compression,
                               meta_text)) {
        _dropped_messages++;
        return message_type::none;
      }
      const auto *p =
          static_cast<const unsigned char *>(message.raw_data(3));
      const size_t n = message.size(3);
      std::lock_guard<std::mutex> lock(_message_state_mutex);
      _last_blob = make_tuple(std::move(topic), std::move(meta_text),
                              vector<unsigned char>(p, p + n));
      return message_type::blob;
    }
    // Data frame: [topic][header][payload]
    if (parts < 3) {
      _dropped_messages++;
      return message_type::none;
    }
    auto pl = decode_to_payload(message.get(2), hdr.format, hdr.compression);
    if (!pl) {
      _dropped_messages++;
      return message_type::none;
    }
    if (_remote_controlled && topic == "control") {
      remote_control(pl->text());
      return message_type::json;
    }
    auto lp = std::make_shared<LazyPayload>(std::move(*pl));
    std::lock_guard<std::mutex> lock(_message_state_mutex);
    _status[topic] = lp;
    _last_message = make_tuple(std::move(topic), std::move(lp));
    return message_type::json;
  }

  // Legacy frames, disambiguated by part count.
  switch (parts) {
  case 2: { // [topic][snappy(json)]
    string payload = message.get(1);
    if (payload.empty()) {
      return message_type::none;
    }
    // Not a snappy frame (foreign/corrupt) => decode fails => drop instead of
    // storing garbage and crashing the downstream json::parse (§1.1).
    auto pl = decode_to_payload(payload, static_cast<uint8_t>(WireFormat::Json),
                                static_cast<uint8_t>(Comp::Snappy));
    if (!pl) {
      _dropped_messages++;
      return message_type::none;
    }
    if (_remote_controlled && topic == "control") {
      remote_control(pl->text());
      return message_type::json;
    }
    auto lp = std::make_shared<LazyPayload>(std::move(*pl));
    std::lock_guard<std::mutex> lock(_message_state_mutex);
    _status[topic] = lp;
    _last_message = make_tuple(std::move(topic), std::move(lp));
    return message_type::json;
  }
  case 3: { // [topic][json meta][raw bytes]
    string format = message.get(1);
    const auto *p = static_cast<const unsigned char *>(message.raw_data(2));
    const size_t n = message.size(2);
    std::lock_guard<std::mutex> lock(_message_state_mutex);
    _last_blob = make_tuple(std::move(topic), std::move(format),
                            vector<unsigned char>(p, p + n));
    return message_type::blob;
  }
  default:
    _dropped_messages++;
    return message_type::none;
  }
}

void Agent::install_signal_handlers() {
  // Idempotent and process-global (REFACTOR.md §1.6): handlers are installed
  // once, so repeated loop() calls or multiple agents in one process do not
  // clobber each other's handler state.
  std::call_once(g_signal_once, []() {
    std::signal(SIGINT, [](int signum) {
      UNUSED(signum);
      Mads::Runtime::stop_process();
    });
    std::signal(SIGTERM, [](int signum) {
      UNUSED(signum);
      Mads::Runtime::stop_process();
    });
  });
}

namespace {
// Waits until `deadline`. In high-res mode, sleeps for all but the last
// `spin_margin` of the remaining time, then busy-spins on steady_clock for
// microsecond-accurate wake-up (at the cost of keeping a core busy). In the
// default mode, a single sleep_for is used: nanosecond-accurate in its
// accounting, but the actual wake-up jitter is limited by the OS scheduler.
void wait_until(chrono::steady_clock::time_point deadline, bool high_res,
                chrono::nanoseconds spin_margin) {
  auto now = chrono::steady_clock::now();
  if (now >= deadline)
    return;
  if (high_res) {
    if (deadline - now > spin_margin)
      this_thread::sleep_for(deadline - now - spin_margin);
    while (chrono::steady_clock::now() < deadline) {
      // busy-spin for microsecond-accurate wake-up
    }
  } else {
    this_thread::sleep_for(deadline - now);
  }
}
} // namespace

#ifdef MADS_LOOP_USES_THREADS
void Agent::loop(loop_fun_t const &lambda, chrono::nanoseconds duration) {
  if (!_init_done)
    throw AgentError("Agent not initialized");
  install_signal_handlers();
  chrono::nanoseconds nld(0); // next loop duration
  while (keep_running()) {
    if (duration > 0ns || nld > 0ns) {
      thread t([&]() { this_thread::sleep_for(nld == 0ns ? duration : nld); });
      try {
        nld = lambda();
      } catch (...) {
        _stopping = true;
      }
      t.join();
    } else {
      try {
        nld = lambda();
      } catch (std::exception &e) {
        cerr << "Exception in loop: " << e.what() << endl;
        _stopping = true;
      }
    }
  }
}
#else
void Agent::loop(loop_fun_t const &lambda, chrono::nanoseconds duration) {
  if (!_init_done)
    throw AgentError("Agent not initialized");
  install_signal_handlers();
  chrono::nanoseconds nld(0); // next loop duration
  while (keep_running()) {
    chrono::nanoseconds sleep_duration = nld == 0ns ? duration : nld;
    auto start = chrono::steady_clock::now();
    try {
      nld = lambda();
    } catch (std::exception &e) {
      cerr << "Exception in loop: " << e.what() << endl;
      _stopping = true;
    }
    if (sleep_duration > 0ns) {
      wait_until(start + sleep_duration, _high_res_loop, _spin_margin);
    }
  }
}
#endif

void Agent::loop(loop_fun_t const &lambda) {
  loop(lambda, _time_step);
}

void Agent::enable_high_res_loop(bool on, chrono::nanoseconds spin_margin) {
  _high_res_loop = on;
  _spin_margin = spin_margin;
}

bool Agent::high_res_loop() const {
  return _high_res_loop;
}

void Agent::enable_remote_control(bool threaded) {
  if (!_init_done)
    throw AgentError("Agent not initialized");
  if (_connected)
    throw AgentError("Cannot enable remote control after connecting");
  _remote_controlled = true;
  _sub_topic.push_back("control");
  if (threaded) {
    // The drain thread owns the subscriber socket exclusively (§1.7).
    _rc_owns_socket = true;
    _rc_thread = thread([this]() {
      _subscriber.set(zmqpp::socket_option::receive_timeout, 500);
      message msg;
      while (keep_running()) {
        if (!_subscriber.receive(msg, false))
          continue;
        const size_t parts = msg.parts();
        if (parts < 2)
          continue;
        string topic = msg.get(0);
        if (topic != "control")
          continue;
        string j;
        bool ok = false;
        WireHeader hdr;
        if (parse_wire_header(msg.get(1), hdr) && !hdr.has_blob &&
            parts >= 3) {
          ok = decode_to_json_text(msg.get(2), hdr.format, hdr.compression, j);
        } else if (parts == 2) {
          string payload = msg.get(1);
          ok = snappy::Uncompress(payload.data(), payload.size(), &j);
        }
        if (ok)
          remote_control(j);
        else
          _dropped_messages++;
      }
    });
  }
}


void Agent::remote_control(string payload_str) {
  if (!_init_done)
    throw AgentError("Agent not initialized");
  // auto [topic, payload_str] = last_message();
  nlohmann::json payload = {};
  string command;
  try {
    payload = nlohmann::json::parse(payload_str);
  } catch(...) {
    cout << "Error" << endl;
    payload["cmd"] = "none";
  }
  command = payload["cmd"];
  if (command == "restart") {
    _restart = true;
    Mads::Runtime::stop_process();
  } else if (command == "shutdown") {
    Mads::Runtime::stop_process();
  } else if (command == "info") {
    nlohmann::json response = get_settings();
    response["agent"] = _name;
    publish(response, "info");
  }
}

bool Agent::settings_are_local() const {
  return _settings_uri.find("tcp://") == string::npos;
}

void Agent::save_settings(const string path) {
  if (!_init_done)
    throw AgentError("Agent not initialized");

  if (settings_are_local()) {
    throw AgentError("Can only save settings loaded from broker");
  }
  ofstream out(path);
  out << _raw_settings;
  out.close();
}

void Agent::connect_pub(chrono::milliseconds delay) {
  if (_cross) {
    string port = _sub_endpoint.substr(_sub_endpoint.find_last_of(":") + 1);
    _sub_endpoint = "tcp://*:" + port;
    _publisher.bind(_sub_endpoint);
  } else
    _publisher.connect(_pub_endpoint);
  if (delay.count() > 0)
    this_thread::sleep_for(delay);
}

void Agent::connect_sub() {
  _subscriber.set(zmqpp::socket_option::receive_timeout, _receive_timeout);
  if (_cross) {
    string port = _pub_endpoint.substr(_pub_endpoint.find_last_of(":") + 1);
    _pub_endpoint = "tcp://*:" + port;
    _subscriber.bind(_pub_endpoint);
  } else
    _subscriber.connect(_sub_endpoint);
  for (auto &t : _sub_topic) {
    _subscriber.subscribe(t);
  }
  // Drain thread
  // this keeps the queue updated to the LKV when its size is 1
  if (_last_value_only) {
    _drain_thread = thread([this]() {
      zmqpp::message_t msg;
      while(keep_running() && _connected) {
        try {
          if (!_subscriber.receive(msg, false)) continue;
          std::lock_guard<std::mutex> lock(_latest_message.mtx);
          _latest_message.value = msg.copy();
          _latest_message.cv.notify_one();
        } catch (...) {}
      }
    });
  }
}

tuple<string, string, string> Agent::split_URL(const string &url) {
  static regex re("(\\w+://)([\\w\\.-]+):(\\d+)");
  smatch match;

  if (regex_search(url, match, re) && match.size() > 3) {
    return make_tuple(match.str(1), match.str(2), match.str(3));
  } else {
    throw std::invalid_argument("Invalid URL: " + url);
  }
}

void Agent::set_agent_id(string id) { _agent_id = id; }

string Agent::get_agent_id() { return _agent_id; }

map<string, string> Agent::status() {
  std::lock_guard<std::mutex> lock(_message_state_mutex);
  map<string, string> out;
  for (auto const &[topic, lp] : _status) {
    out[topic] = lp ? lp->text() : string();
  }
  return out;
}

string Agent::name() { return _name; }

tuple<string, string> Agent::last_message() {
  std::lock_guard<std::mutex> lock(_message_state_mutex);
  auto const &lp = get<1>(_last_message);
  return make_tuple(get<0>(_last_message), lp ? lp->text() : string());
}

tuple<string, nlohmann::json> Agent::last_json() {
  std::lock_guard<std::mutex> lock(_message_state_mutex);
  auto const &lp = get<1>(_last_message);
  return make_tuple(get<0>(_last_message),
                    lp ? lp->doc() : nlohmann::json());
}

string Agent::last_topic() {
  std::lock_guard<std::mutex> lock(_message_state_mutex);
  return get<0>(_last_message);
}

tuple<string, string, vector<unsigned char>> Agent::last_blob() {
  std::lock_guard<std::mutex> lock(_message_state_mutex);
  return _last_blob;
}

tuple<string_view, string_view, span<const unsigned char>>
Agent::last_blob_view() const {
  std::lock_guard<std::mutex> lock(_message_state_mutex);
  return make_tuple(string_view(get<0>(_last_blob)),
                    string_view(get<1>(_last_blob)),
                    span<const unsigned char>(get<2>(_last_blob)));
}

size_t Agent::dropped_messages() const { return _dropped_messages.load(); }


bool Agent::is_connected() { return _connected; }

int Agent::settings_timeout() { return _settings_timeout; }

void Agent::set_settings_timeout(int to) {
  if (_init_done)
    throw AgentError("Cannot set timeout after initialization");
  _settings_timeout = to;
}

void Agent::set_settings_timeout(std::chrono::milliseconds to) {
  set_settings_timeout(to.count());
}

int Agent::receive_timeout() { return _receive_timeout; }

void Agent::set_receive_timeout(int to) {
  _receive_timeout = to;
  _subscriber.set(zmqpp::socket_option::receive_timeout, _receive_timeout);
}

void Agent::set_receive_timeout(std::chrono::milliseconds to) {
  set_receive_timeout(to.count());
}

bool Agent::restart() { return _restart; }

void Agent::set_runtime(std::shared_ptr<Mads::Runtime> runtime) {
  if (!runtime)
    throw AgentError("Runtime cannot be null");
  if (_connected)
    throw AgentError("Cannot change the runtime of a connected agent");
  _runtime = std::move(runtime);
}

filesystem::path Agent::attachment_path() {
  return _attachment_path;
}

bool Agent::is_crypto() {
  return _crypto;
}

void Agent::setup_crypto(Mads::auth_verbose verbose) {
  _crypto = true;
  if (!_curve_auth) {
    _curve_auth = make_unique<CurveAuth>(_context);
  }
  _curve_auth->setup_auth(verbose);
}

unique_ptr<CurveAuth> *Agent::curve_auth() {
  return &_curve_auth;
}

filesystem::path Agent::key_dir() {
  return _key_dir;
}

void Agent::set_key_dir(const filesystem::path &path) {
  _key_dir = path;
}

string Agent::settings_uri() { return _settings_uri; }


// Apparently, conflate maked the socket irresponsive
// TODO: investigate
void Agent::set_conflate(bool conflate) {
  if (_connected) throw AgentError("Cannot set_conflate after connection");
  _subscriber.set(socket_option::conflate, conflate);
  _conflate = conflate;
}

bool Agent::conflate() {
  return _conflate;
}


void Agent::set_high_watermark(int i) {
  if (_connected) throw AgentError("Cannot set_high_watermark after connection");
  // i == 0 now means "unlimited" (ZMQ semantics); it no longer silently
  // switches to Last-Known-Value mode as it used to (REFACTOR.md §1.4).
  _subscriber.set(socket_option::receive_high_water_mark, i);
  // Backward-compatible convenience: a queue of exactly 1 selects LKV delivery.
  set_delivery(i == 1 ? Delivery::LastKnownValue : Delivery::Queued);
}

int Agent::high_watermark() {
  int i = 0;
  _subscriber.get(socket_option::receive_high_water_mark, i);
  return i;
}

void Agent::set_delivery(Delivery d) {
  if (_connected) throw AgentError("Cannot set_delivery after connection");
  _last_value_only = (d == Delivery::LastKnownValue);
}

Delivery Agent::delivery() const {
  return _last_value_only ? Delivery::LastKnownValue : Delivery::Queued;
}

void Agent::set_wire_format(WireFormat fmt) { _wire_format = fmt; }

WireFormat Agent::wire_format() const { return _wire_format; }

void Agent::set_compression(Compression c) { _compression = c; }

Compression Agent::compression() const { return _compression; }




} // namespace Mads
