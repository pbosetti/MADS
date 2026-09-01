#include "agent.hpp"
#include <algorithm>
#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>
#include <zmq.hpp>
#include <zmq_addon.hpp>
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
#include "detail/clock_domain.hpp"
#include "detail/plugin_cache.hpp"
#include "detail/socket_options.hpp"
#include "detail/wire_format.hpp"
#include "exec_path.hpp"
#include "mads.hpp"

#ifdef _WIN32
#include <process.h>
#endif

#ifndef MADS_AGENT_NO_INFO
#include <rang.hpp>
using namespace rang;
#endif

using namespace std::string_view_literals;
using namespace std::string_literals;
using namespace std;
using namespace std::chrono;

namespace Mads {

// The self-describing wire frame header (make_wire_header/parse_wire_header/
// WireHeader/Comp) and payload encoding now live in detail/wire_format.hpp,
// so the broker can emit an ordinary MADS-format frame too (ZMQ_DEVELOPMENT.md
// §2.2) without duplicating the format. Brought in unqualified here exactly
// as when they were a local anonymous namespace.
using namespace Mads::detail;

namespace {

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

// ---- Clock offset (clock_offset.hpp) wire-protocol helpers ----------------
// Kept local to agent.cpp rather than in clock_offset.hpp: the wire encoding
// (JSON field names, ClockSource <-> string) is an Agent/CLOCKSYNC_TOPIC
// protocol detail, not part of the pure estimator/consensus math.

// §1.3 chain-safety bounds: reject a peer-source anchor more than this many
// hops out, or whose own measurement is older than this.
constexpr uint8_t CLOCK_MAX_HOPS = 2;
constexpr auto CLOCK_MAX_ANCHOR_AGE = chrono::seconds(60);
// §4.1: at most one ping reply per initiator per this interval, so a
// misbehaving prober cannot turn CLOCKSYNC_TOPIC into a storm.
constexpr int CLOCK_MIN_PROBE_INTERVAL_MS = 200;

string clock_source_name(ClockSource s) {
  switch (s) {
  case ClockSource::Broker:
    return "broker";
  case ClockSource::Peer:
    return "peer";
  default:
    return "none";
  }
}

ClockSource clock_source_from_name(const string &s) {
  if (s == "broker")
    return ClockSource::Broker;
  if (s == "peer")
    return ClockSource::Peer;
  return ClockSource::None;
}

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

void Agent::setup_curve_on(zmq::socket_t &socket) {
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

tuple<string, string, double>
Agent::query_broker(string uri, string name, int timeout) {
  // Single REQ socket reused for both the settings and timecode round-trips.
  zmq::socket_t socket(_context, zmq::socket_type::req);
  setup_curve_on(socket);
  // Drop any undelivered request on close: with the default infinite linger,
  // a request queued toward an unreachable broker keeps the context alive and
  // context termination (Agent shutdown) blocks forever.
  socket.set(zmq::sockopt::linger, 0);
  if (timeout > 0) {
    socket.set(zmq::sockopt::rcvtimeo, timeout);
    socket.set(zmq::sockopt::sndtimeo, timeout);
  }
  socket.connect(uri);

  // ---- settings request ----
  zmq::multipart_t msg_out, msg_in;
  msg_out.addstr(LIB_VERSION);
  msg_out.addstr("settings");
  msg_out.addstr(name);
  if (!msg_out.send(socket)) {
    socket.close();
    throw AgentError("Timed out in sending settings request to broker");
  }
  if (!msg_in.recv(socket)) {
    socket.close();
    throw AgentError("Timed out in receiving settings from broker");
  }
  if (msg_in.size() < 2) {
    socket.close();
    throw AgentError(
        "Broker refuses to provide settings, check for version mismatch or "
        "missing settings for agent '" +
        name + "'");
  }
  string version_str = msg_in.at(0).to_string();
  if (!Mads::check_version(version_str)) {
    socket.close();
    throw AgentError("Received settings from broker with wrong version: " +
                     version_str);
  }
  string raw_settings = msg_in.at(1).to_string();
  // A 3rd part -- and only a 3rd part -- is the attachment. It is carried back
  // as bytes and written to disk by fetch_settings(), which by then knows the
  // `attachment_ext` that decides the cached file's name.
  string attachment;
  if (msg_in.size() == 3) {
    attachment = msg_in.at(2).to_string();
  }

  // ---- timecode request (same socket) ----
  chrono::system_clock::time_point now = chrono::system_clock::now();
  zmq::multipart_t tc_out, tc_in;
  tc_out.addstr(string("v") + LIB_VERSION);
  tc_out.addstr("timecode");
  tc_out.send(socket);
  if (!tc_in.recv(socket)) {
    socket.close();
    throw AgentError("Timed out in receiving timecode from broker");
  }
  double broker_tc = std::stod(tc_in.at(0).to_string());
  double timecode_offset = broker_tc - timecode(now, timecode_fps);

  socket.disconnect(uri);
  socket.close();
  return make_tuple(raw_settings, attachment, timecode_offset);
}

// Public methods implementations

Agent::Agent(string name, string settings_uri)
    : _settings_uri(settings_uri), _context(),
      _publisher(_context, zmq::socket_type::pub),
      _subscriber(_context, zmq::socket_type::sub) {

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

void Agent::fetch_settings(bool crypto) {
  if (_settings_fetched) {
    return;
  }
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
  string attachment;
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
    attachment = get<1>(received);
    _timecode_offset = get<2>(received);
    _config = (toml::table)toml::parse(_raw_settings);
  }

  // Cache the attachment, if the broker served one. The extension has to come
  // from the settings we just parsed, which is why this cannot happen inside
  // query_broker(): the cached file's stem must stay _name (both plugin
  // loaders fall back to it for the driver name), so the extension is part of
  // the final name and has to be known before anything is written.
  if (!attachment.empty()) {
    auto cfg = _config[_name];
    string ext = cfg["attachment_ext"].value_or("plugin");
    if (ext.rfind('.', 0) == 0) {
      ext = ext.substr(1); // remove leading dot
    }
    try {
      _attachment_path = detail::store_attachment(_name, ext, attachment);
    } catch (const std::exception &e) {
      throw AgentError(e.what());
    }
  }

  _settings_fetched = true;
}

void Agent::init(bool crypto, bool install_watchdog) {
  fetch_settings(crypto);
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
  _configure_clock_sync();
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

  _apply_socket_options();

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

  // Initial clock-offset measurement (source A only -- source "peer" needs
  // a connected socket that does not exist until connect(), so its first
  // attempt happens at the top of _start_clock_thread() instead). Skipped
  // when there is no broker to ask (local settings file, or "none").
  if (_clock_source == ClockSource::Broker && !settings_are_local()) {
    measure_clock_offset();
  }
}

void Agent::load_settings() {}

void Agent::_configure_clock_sync() {
  auto all_cfg = _config["agents"];
  auto cfg = _config[_name];

  string default_source = all_cfg["clock_source"].value_or(string("broker"));
  string source = cfg["clock_source"].value_or(default_source);
  if (source == "peer") {
    _clock_source = ClockSource::Peer;
  } else if (source == "none") {
    _clock_source = ClockSource::None;
  } else {
    _clock_source = ClockSource::Broker;
  }

  bool default_responder = all_cfg["clock_sync_responder"].value_or(true);
  _clock_sync_responder =
      cfg["clock_sync_responder"].value_or(default_responder);

  int default_interval = all_cfg["clock_interval_ms"].value_or(0);
  _clock_interval_ms = cfg["clock_interval_ms"].value_or(default_interval);

  int default_announce = all_cfg["clock_announce_ms"].value_or(5000);
  _clock_announce_ms = cfg["clock_announce_ms"].value_or(default_announce);

  bool default_correction = all_cfg["clock_correction"].value_or(false);
  _clock_correction = cfg["clock_correction"].value_or(default_correction);

  // Deliberately NOT appended to _sub_topic here (unlike how
  // enable_remote_control() appends "control"): _sub_topic is public,
  // user-facing state -- sub_topic(), info()'s "Sub topics:" listing, and
  // `mads doctor --graph`'s topology diagram all read it verbatim, and it
  // must keep reflecting exactly what the settings file declared. The raw
  // ZMQ subscription for CLOCKSYNC_TOPIC is issued separately by
  // connect_sub() (see _clock_wants_sync()).
}

bool Agent::_clock_wants_sync() const {
  return _clock_source != ClockSource::None || _clock_sync_responder;
}

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

  // 3. Join the I/O thread (bounded by its receive timeout)
  if (_io_thread.joinable()) _io_thread.join();
  // 3a. Join the clock-sync thread (bounded by its own 200ms poll -- see
  //     _start_clock_thread()); keep_running() already reflects _stopping.
  if (_clock_thread.joinable()) _clock_thread.join();

  // 3b. Stop the socket monitors before the sockets they watch are closed
  //     below (SocketMonitor::stop() detaches while the socket is still
  //     open; doing this after close() would touch a dead handle).
  _pub_monitor.stop();
  _sub_monitor.stop();

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
  try { _publisher.set(zmq::sockopt::linger, 0); } catch (...) {}
  try { _subscriber.set(zmq::sockopt::linger, 0); } catch (...) {}
  try { _publisher.close(); } catch (...) {}
  try { _subscriber.close(); } catch (...) {}
  // context_t::close() is zmq_ctx_term(): the same blocking teardown zmqpp
  // spelled terminate(), not a non-blocking shutdown.
  try { _context.close(); } catch (...) {}
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
  if (_clock_source != ClockSource::None) {
    out << "  Clock domain:     " << style::bold << detail::clock_domain_id()
        << style::reset << endl;
    auto adopted = clock_offset();
    if (adopted.valid) {
      out << "  Clock offset:     " << style::bold
          << (adopted.offset_us / 1000.0) << " ms" << style::reset
          << " (" << clock_source_name(adopted.source) << ", "
          << static_cast<int>(adopted.hops) << " hops, delay "
          << (adopted.delay_us / 1000.0) << " ms, ref "
          << adopted.clock_ref() << ")" << endl;
    } else {
      out << "  Clock offset:     " << style::dim << "not yet measured"
          << style::reset << endl;
    }
  }
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
  // Deliberately NOT forced open for a pure source agent with no declared
  // sub_topic: doing so would silently turn every publish-only agent into
  // a subscriber too, changing link_state()'s publish-only fallback and
  // doubling its socket footprint fleet-wide just to answer clock pings it
  // may never receive. Such an agent still measures/announces its own
  // clock_source == "broker" reading (that REQ round-trip needs no
  // subscriber); it just cannot itself answer pings or hear the domain's
  // other measurements until it has a real reason to subscribe to
  // something. See _clock_wants_sync().
  if (!_sub_topic.empty()) {
    connect_sub();
    _connected = true;
  }
  if (_clock_source != ClockSource::None) {
    _start_clock_thread();
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
  if (_io_thread.joinable()) _io_thread.join();
  if (_clock_thread.joinable()) _clock_thread.join();

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
  zmq::multipart_t message;
  int32_t offset = 0;
  if (payload.contains("event"))
    if (payload["event"] == event_map.at(event_type::shutdown) ||
        payload["event"] == event_map.at(event_type::startup)) {
      offset = STARTUP_SHUTDOWN_DELAY_MS;
    }
  chrono::system_clock::time_point now = chrono::system_clock::now();
  // [agents] clock_correction (opt-in, off by default): step the instant
  // stamped below by this agent's adopted offset, so timestamp/timecode
  // become broker-referenced instead of host-local. Never silent: a
  // corrected message also carries clock_offset_us/clock_ref, so a
  // consumer can recover the raw local time and see which measurement
  // produced the correction.
  ClockOffsetResult clock_adj;
  if (_clock_correction) {
    clock_adj = clock_offset();
    if (clock_adj.valid) {
      now += chrono::microseconds(clock_adj.offset_us);
    }
  }
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
  if (_clock_correction && clock_adj.valid &&
      !payload.contains("clock_offset_us")) {
    payload["clock_offset_us"] = clock_adj.offset_us;
    payload["clock_ref"] = clock_adj.clock_ref();
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
    message.addstr(topic);
    message.addstr(make_wire_header(_wire_format, comp, false));
    message.addstr(out);
  } else {
    // [topic][snappy(json)]
    message.addstr(topic);
    message.addstr(out);
  }
  {
    std::lock_guard<std::mutex> lock(_publish_mutex);
    message.send(_publisher);
  }
}

void Agent::publish(const char *payload, size_t len,
           nlohmann::json meta, string topic) {
  if (!_init_done)
    throw AgentError("Agent not initialized");
  zmq::multipart_t message;
  chrono::system_clock::time_point now = chrono::system_clock::now();
  ClockOffsetResult clock_adj;
  if (_clock_correction) {
    clock_adj = clock_offset();
    if (clock_adj.valid) {
      now += chrono::microseconds(clock_adj.offset_us);
    }
  }
  if (!meta.contains("timestamp"))
    meta["timestamp"]["$date"] = get_ISODate_time(now);
  if (!meta.contains("timecode"))
    meta["timecode"] = timecode(now, timecode_fps);
  if (!meta.contains("agent_id"))
    meta["agent_id"] = _agent_id;
  if (!meta.contains("hostname"))
    meta["hostname"] = _hostname;
  if (_clock_correction && clock_adj.valid &&
      !meta.contains("clock_offset_us")) {
    meta["clock_offset_us"] = clock_adj.offset_us;
    meta["clock_ref"] = clock_adj.clock_ref();
  }
  if (topic.empty())
    topic = _pub_topic;
  if (_wire_format == WireFormat::MsgPack) {
    // [topic][header(has_blob)][msgpack(meta)][raw bytes]. Metadata is small,
    // so it is left uncompressed.
    message.addstr(topic);
    message.addstr(make_wire_header(WireFormat::MsgPack, Comp::None, true));
    message.addstr(encode_payload(meta, WireFormat::MsgPack));
  } else {
    // Legacy blob frame: [topic][json meta][raw bytes].
    message.addstr(topic);
    message.addstr(meta.dump());
  }
  message.addmem(payload, len);
  {
    std::lock_guard<std::mutex> lock(_publish_mutex);
    message.send(_publisher);
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


inline bool Agent::receive_raw(zmq::multipart_t &message, bool dont_block) {
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
      message = _latest_message.value.value().clone();
      _latest_message.value.reset();
      r = true;
    }
  }
  // Queued operation (blocking or not)
  else {
    // Fast path (P2): an agent with no wildcard sub_topic entries never
    // enters the loop body more than once -- same single receive() call,
    // same behaviour, as before this feature existed.
    while (true) {
      r = message.recv(_subscriber, dont_block ? ZMQ_DONTWAIT : 0);
      if (!r || _wildcard_sub_topic.empty() || message.size() == 0)
        break;
      if (_topic_matches_subscription(message.at(0).to_string()))
        break;
      // Non-matching message under a broader wildcard-prefix subscribe:
      // silently drop and try again (bounded by dont_block/receive_timeout
      // on each individual receive() call).
    }
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
  zmq::multipart_t message;
  if (!receive_raw(message, dont_block)) {
    return message_type::none;
  }

  const size_t parts = message.size();
  // Malformed frames from the network are dropped, never fatal (§1.2).
  if (parts < 2) {
    _dropped_messages++;
    return message_type::none;
  }

  string topic = message.at(0).to_string();

  // Extended, self-describing frame? (§1.3)
  WireHeader hdr;
  if (parse_wire_header(message.at(1).to_string(), hdr)) {
    if (hdr.has_blob) {
      if (parts < 4) {
        _dropped_messages++;
        return message_type::none;
      }
      string meta_text;
      if (!decode_to_json_text(message.at(2).to_string(), hdr.format, hdr.compression,
                               meta_text)) {
        _dropped_messages++;
        return message_type::none;
      }
      const auto *p =
          static_cast<const unsigned char *>(message.at(3).data());
      const size_t n = message.at(3).size();
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
    auto pl = decode_to_payload(message.at(2).to_string(), hdr.format, hdr.compression);
    if (!pl) {
      _dropped_messages++;
      return message_type::none;
    }
    if (_remote_controlled && topic == "control") {
      remote_control(pl->text());
      return message_type::json;
    }
    // CLOCKSYNC_TOPIC is reserved and unconditionally swallowed here (not
    // gated on this agent's own clock_source/clock_sync_responder, unlike
    // "control" above): an agent with sub_topic = [""] subscribes to every
    // topic at the ZMQ level regardless of its own clock settings, so this
    // must never leak clock-sync traffic into a caller's normal receive()
    // loop (e.g. `mads feedback`/`mads logger`/`mads top`).
    if (topic == CLOCKSYNC_TOPIC) {
      try {
        _handle_clocksync_message(pl->doc());
      } catch (...) {
      }
      return message_type::none;
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
    string payload = message.at(1).to_string();
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
    // CLOCKSYNC_TOPIC is reserved and unconditionally swallowed here (not
    // gated on this agent's own clock_source/clock_sync_responder, unlike
    // "control" above): an agent with sub_topic = [""] subscribes to every
    // topic at the ZMQ level regardless of its own clock settings, so this
    // must never leak clock-sync traffic into a caller's normal receive()
    // loop (e.g. `mads feedback`/`mads logger`/`mads top`).
    if (topic == CLOCKSYNC_TOPIC) {
      try {
        _handle_clocksync_message(pl->doc());
      } catch (...) {
      }
      return message_type::none;
    }
    auto lp = std::make_shared<LazyPayload>(std::move(*pl));
    std::lock_guard<std::mutex> lock(_message_state_mutex);
    _status[topic] = lp;
    _last_message = make_tuple(std::move(topic), std::move(lp));
    return message_type::json;
  }
  case 3: { // [topic][json meta][raw bytes]
    string format = message.at(1).to_string();
    const auto *p = static_cast<const unsigned char *>(message.at(2).data());
    const size_t n = message.at(2).size();
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

bool Agent::receive_raw_message(string &topic, vector<string> &parts,
                                bool dont_block) {
  if (!_init_done)
    throw AgentError("Agent not initialized");
  // Same concurrency guard as receive() (REFACTOR.md §1.7): threaded remote
  // control owns the subscriber socket exclusively.
  if (_rc_owns_socket)
    throw AgentError("receive_raw_message() cannot be used while threaded "
                     "remote control owns the subscriber socket");
  zmq::multipart_t message;
  if (!receive_raw(message, dont_block))
    return false;

  const size_t n = message.size();
  if (n == 0)
    return false;

  topic = message.at(0).to_string();
  parts.clear();
  parts.reserve(n - 1);
  for (size_t i = 1; i < n; ++i)
    parts.push_back(message.at(i).to_string());
  return true;
}

void Agent::publish_raw_message(const string &topic, const vector<string> &parts) {
  if (!_init_done)
    throw AgentError("Agent not initialized");
  zmq::multipart_t message;
  message.addmem(topic.data(), topic.size());
  for (auto const &part : parts)
    message.addmem(part.data(), part.size());
  {
    std::lock_guard<std::mutex> lock(_publish_mutex);
    message.send(_publisher);
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
    // The unified I/O thread owns the subscriber socket exclusively (§1.7);
    // it is actually started in connect_sub(), once _subscriber exists and
    // is about to be connected. Flagging it here, before connect(), is what
    // this method's precondition guarantees.
    _rc_owns_socket = true;
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

/*
  ____ _            _      ___   __  __          _
 / ___| | ___   ___| | __ / _ \ / _|/ _|___  ___| |_
| |   | |/ _ \ / __| |/ /| | | | |_| |_/ __|/ _ \ __|
| |___| | (_) | (__|   < | |_| |  _|  _\__ \  __/ |_
 \____|_|\___/ \___|_|\_\ \___/|_| |_| |___/\___|\__|

*/

string Agent::_clock_agent_identity() const {
  if (!_agent_id.empty())
    return _agent_id;
#ifdef _WIN32
  const auto pid = static_cast<long long>(_getpid());
#else
  const auto pid = static_cast<long long>(getpid());
#endif
  return _name + "@" + to_string(pid);
}

ClockOffsetResult Agent::_stamp_clock_result(ClockOffsetResult r,
                                             ClockSource source,
                                             uint8_t hops) {
  r.source = source;
  r.hops = hops;
  r.origin_agent_id = _clock_agent_identity();
  r.measured_at = chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(_clock_mtx);
    r.seq = ++_clock_seq;
  }
  return r;
}

void Agent::_announce_clock_offset(const ClockOffsetResult &r) {
  if (!_init_done || !r.valid)
    return;
  nlohmann::json msg;
  msg["type"] = "announce";
  msg["agent_id"] = r.origin_agent_id;
  msg["name"] = _name;
  msg["hostname"] = _hostname;
  msg["domain"] = detail::clock_domain_id();
  msg["offset_us"] = r.offset_us;
  msg["delay_us"] = r.delay_us;
  msg["jitter_us"] = r.jitter_us;
  msg["hops"] = r.hops;
  msg["source"] = clock_source_name(r.source);
  msg["seq"] = r.seq;
  try {
    publish(msg, CLOCKSYNC_TOPIC);
  } catch (...) {
    // A publish failure here (e.g. socket not connected yet) just means
    // this announcement is skipped; the next periodic one will retry.
  }
}

ClockOffsetResult Agent::measure_clock_offset(size_t samples,
                                              int timeout_ms) {
  if (settings_are_local())
    return {}; // no broker settings endpoint to talk to

  ClockOffsetEstimator estimator;
  try {
    zmq::socket_t socket(_context, zmq::socket_type::req);
    setup_curve_on(socket);
    // Same reasoning as query_broker(): drop any undelivered request on
    // close rather than blocking context teardown on an unreachable broker.
    socket.set(zmq::sockopt::linger, 0);
    if (timeout_ms > 0) {
      socket.set(zmq::sockopt::rcvtimeo, timeout_ms);
      socket.set(zmq::sockopt::sndtimeo, timeout_ms);
    }
    socket.connect(_settings_uri);
    for (size_t i = 0; i < samples; ++i) {
      const auto t1_steady = chrono::steady_clock::now();
      const auto t1_wall = chrono::system_clock::now();
      zmq::multipart_t out, in;
      out.addstr(LIB_VERSION);
      out.addstr("clock");
      if (!out.send(socket))
        break;
      if (!in.recv(socket))
        break;
      const auto t4_steady = chrono::steady_clock::now();
      const auto t4_wall = chrono::system_clock::now();
      // Fewer than 3 frames means an old broker that does not know "clock"
      // (it falls into the generic "unexpected command" branch and echoes
      // back a bare [LIB_VERSION]) -- stop and report whatever, if
      // anything, earlier samples in this same call already gathered.
      if (in.size() < 3)
        break;
      ClockSample s;
      s.t1 = epoch_us(t1_wall);
      s.t2 = std::stoll(in.at(1).to_string());
      s.t3 = std::stoll(in.at(2).to_string());
      s.t4 = epoch_us(t4_wall);
      s.local_elapsed_us =
          chrono::duration_cast<chrono::microseconds>(t4_steady - t1_steady)
              .count();
      estimator.add(s);
    }
    socket.disconnect(_settings_uri);
    socket.close();
  } catch (...) {
    // Never throws (per the public contract): fall through with whatever
    // the estimator collected before the failure, if anything.
  }

  auto best = estimator.best();
  if (!best.valid)
    return best; // unreachable broker, too old, or every round-trip failed

  auto r = _stamp_clock_result(best, ClockSource::Broker, /*hops=*/0);
  {
    std::lock_guard<std::mutex> lock(_clock_mtx);
    _own_clock_measurement = r;
  }
  _clock_consensus.record(detail::clock_domain_id(), r,
                          chrono::steady_clock::now());
  _announce_clock_offset(r);
  return r;
}

vector<ClockPeerObservation>
Agent::broadcast_clock_probe(chrono::milliseconds window) {
  if (!_init_done || _pub_topic.empty())
    return {};

  {
    std::lock_guard<std::mutex> lock(_clocksync_mtx);
    _clocksync_pongs.clear();
    _clocksync_probe_sent_at = chrono::steady_clock::now();
  }
  nlohmann::json ping;
  ping["type"] = "ping";
  ping["probe"] = _clock_agent_identity();
  ping["target"] = ""; // broadcast: every responder on the bus answers
  ping["seq"] = ++_clocksync_probe_seq;
  ping["t1"] = epoch_us(chrono::system_clock::now());
  try {
    publish(ping, CLOCKSYNC_TOPIC);
  } catch (...) {
    return {};
  }

  // Poll receive() in short bursts rather than one blocking call: a single
  // receive(dont_block=false) can wait up to _receive_timeout past the
  // caller's requested window.
  const auto deadline = chrono::steady_clock::now() + window;
  while (chrono::steady_clock::now() < deadline) {
    try {
      receive(/*dont_block=*/true);
    } catch (...) {
    }
    this_thread::sleep_for(chrono::milliseconds(5));
  }

  std::lock_guard<std::mutex> lock(_clocksync_mtx);
  return _clocksync_pongs;
}

void Agent::_run_peer_measurement() {
  auto peers = broadcast_clock_probe();
  if (peers.empty())
    return;

  // §1.3 chain safety: an eligible anchor must have a real source of its
  // own, be within the hop budget, and not be stale. Among the eligible
  // ones, prefer fewer hops, then the smaller round-trip delay -- the same
  // priorities ClockConsensus::adopted() uses.
  const ClockPeerObservation *chosen = nullptr;
  ClockOffsetResult chosen_theta;
  const auto now = chrono::steady_clock::now();
  for (auto const &p : peers) {
    if (p.responder_adopted.source == ClockSource::None)
      continue;
    if (p.responder_adopted.hops >= CLOCK_MAX_HOPS)
      continue;
    if (now - p.responder_adopted.measured_at > CLOCK_MAX_ANCHOR_AGE)
      continue;
    auto theta = estimate(p.sample);
    if (!chosen ||
        p.responder_adopted.hops < chosen->responder_adopted.hops ||
        (p.responder_adopted.hops == chosen->responder_adopted.hops &&
         theta.delay_us < chosen_theta.delay_us)) {
      chosen = &p;
      chosen_theta = theta;
    }
  }
  if (!chosen)
    return;

  auto r = _stamp_clock_result(chosen_theta, ClockSource::Peer,
                               static_cast<uint8_t>(
                                   chosen->responder_adopted.hops + 1));
  // clock_offset(mine) = clock_offset(peer) + theta (§1.3 of the design):
  // the peer's own broker-anchored offset composed with the pairwise
  // exchange, not a bare pairwise measurement.
  r.offset_us = chosen->responder_adopted.offset_us + chosen_theta.offset_us;
  {
    std::lock_guard<std::mutex> lock(_clock_mtx);
    _own_clock_measurement = r;
  }
  _clock_consensus.record(detail::clock_domain_id(), r,
                          chrono::steady_clock::now());
  _announce_clock_offset(r);
}

ClockOffsetResult Agent::clock_offset() const {
  const auto now = chrono::steady_clock::now();
  auto adopted = _clock_consensus.adopted(detail::clock_domain_id(), now);
  if (adopted.valid)
    return adopted;
  std::lock_guard<std::mutex> lock(_clock_mtx);
  return _own_clock_measurement;
}

string Agent::clock_domain() const { return detail::clock_domain_id(); }

void Agent::_handle_clocksync_message(const nlohmann::json &msg) {
  if (!msg.is_object())
    return;
  const string type = msg.value("type", "");

  if (type == "ping") {
    if (!_clock_sync_responder)
      return;
    const string initiator = msg.value("probe", "");
    if (initiator.empty())
      return;
    const string target = msg.value("target", "");
    const string my_identity = _clock_agent_identity();
    if (!target.empty() && target != my_identity)
      return;

    const auto now = chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(_clocksync_mtx);
      auto it = _clocksync_last_reply.find(initiator);
      if (it != _clocksync_last_reply.end() &&
          now - it->second <
              chrono::milliseconds(CLOCK_MIN_PROBE_INTERVAL_MS)) {
        return; // §4.1 rate limit: already answered this initiator recently
      }
      _clocksync_last_reply[initiator] = now;
    }

    nlohmann::json pong;
    pong["type"] = "pong";
    pong["probe"] = initiator;
    pong["seq"] = msg.value("seq", static_cast<uint64_t>(0));
    pong["responder"] = my_identity;
    pong["name"] = _name;
    pong["hostname"] = _hostname;
    pong["domain"] = detail::clock_domain_id();
    pong["t1"] = msg.value("t1", static_cast<int64_t>(0));
    pong["t2"] = epoch_us(chrono::system_clock::now());
    auto adopted = clock_offset();
    if (adopted.valid) {
      pong["clock_offset_us"] = adopted.offset_us;
      pong["clock_ref"] = adopted.clock_ref();
      pong["hops"] = adopted.hops;
      pong["source"] = clock_source_name(adopted.source);
      pong["anchor_age_us"] = chrono::duration_cast<chrono::microseconds>(
                                  chrono::steady_clock::now() -
                                  adopted.measured_at)
                                  .count();
    } else {
      pong["source"] = clock_source_name(ClockSource::None);
    }
    pong["t3"] = epoch_us(chrono::system_clock::now());
    try {
      publish(pong, CLOCKSYNC_TOPIC);
    } catch (...) {
    }
    return;
  }

  if (type == "announce") {
    const string agent_id = msg.value("agent_id", "");
    const string domain = msg.value("domain", "");
    if (agent_id.empty() || domain.empty())
      return;
    ClockOffsetResult r;
    r.offset_us = msg.value("offset_us", static_cast<int64_t>(0));
    r.delay_us = msg.value("delay_us", static_cast<int64_t>(0));
    r.jitter_us = msg.value("jitter_us", static_cast<int64_t>(0));
    r.hops = static_cast<uint8_t>(msg.value("hops", 0));
    r.source = clock_source_from_name(msg.value("source", string("none")));
    r.origin_agent_id = agent_id;
    r.seq = msg.value("seq", static_cast<uint64_t>(0));
    r.valid = true;
    _clock_consensus.record(domain, r, chrono::steady_clock::now());
    return;
  }

  if (type == "pong") {
    const string probe = msg.value("probe", "");
    if (probe.empty() || probe != _clock_agent_identity())
      return; // not addressed to this agent's own outstanding probe

    ClockPeerObservation obs;
    obs.responder_agent_id = msg.value("responder", "");
    obs.responder_name = msg.value("name", "");
    obs.responder_hostname = msg.value("hostname", "");
    obs.responder_domain = msg.value("domain", "");
    obs.sample.t1 = msg.value("t1", static_cast<int64_t>(0));
    obs.sample.t2 = msg.value("t2", static_cast<int64_t>(0));
    obs.sample.t3 = msg.value("t3", static_cast<int64_t>(0));
    obs.sample.t4 = epoch_us(chrono::system_clock::now());
    if (msg.contains("clock_offset_us")) {
      obs.responder_adopted.valid = true;
      obs.responder_adopted.offset_us =
          msg.value("clock_offset_us", static_cast<int64_t>(0));
      obs.responder_adopted.hops =
          static_cast<uint8_t>(msg.value("hops", 0));
      obs.responder_adopted.source =
          clock_source_from_name(msg.value("source", string("none")));
      obs.responder_adopted.origin_agent_id = obs.responder_agent_id;
      const int64_t age_us =
          msg.value("anchor_age_us", static_cast<int64_t>(0));
      obs.responder_adopted.measured_at =
          chrono::steady_clock::now() - chrono::microseconds(age_us);
    } else {
      obs.responder_adopted.source = ClockSource::None;
    }

    std::lock_guard<std::mutex> lock(_clocksync_mtx);
    // local_elapsed_us needs THIS agent's own steady_clock send instant,
    // which only broadcast_clock_probe() (the only caller that ever sets
    // an outstanding probe) knows.
    if (_clocksync_probe_sent_at != chrono::steady_clock::time_point{}) {
      obs.sample.local_elapsed_us =
          chrono::duration_cast<chrono::microseconds>(
              chrono::steady_clock::now() - _clocksync_probe_sent_at)
              .count();
    }
    _clocksync_pongs.push_back(std::move(obs));
    return;
  }
}

void Agent::_start_clock_thread() {
  if (_clock_thread.joinable())
    return;
  _clock_thread = thread([this]() {
    // A "peer"-source agent takes its very first measurement here: init()
    // could not attempt it (no connected socket exists that early), and
    // this thread only ever starts from connect(), once one does.
    if (_clock_source == ClockSource::Peer) {
      _run_peer_measurement();
    }
    auto last_announce = chrono::steady_clock::time_point::min();
    auto last_measure = chrono::steady_clock::now();
    while (keep_running()) {
      const auto now = chrono::steady_clock::now();
      if (_clock_announce_ms > 0 &&
          (last_announce == chrono::steady_clock::time_point::min() ||
           now - last_announce >=
               chrono::milliseconds(_clock_announce_ms))) {
        auto current = clock_offset();
        if (current.valid) {
          _announce_clock_offset(current);
        }
        last_announce = now;
      }
      if (_clock_interval_ms > 0 &&
          now - last_measure >= chrono::milliseconds(_clock_interval_ms)) {
        // §2.3: only the domain's current winner (or nobody yet) re-
        // measures, so N agents on one host cost one measurement per
        // interval, not N.
        const string domain = detail::clock_domain_id();
        auto winner = _clock_consensus.adopted(domain, now);
        if (!winner.valid ||
            _clock_consensus.is_winner(domain, _clock_agent_identity(),
                                       now)) {
          if (_clock_source == ClockSource::Broker) {
            measure_clock_offset();
          } else if (_clock_source == ClockSource::Peer) {
            _run_peer_measurement();
          }
        }
        last_measure = now;
      }
      this_thread::sleep_for(chrono::milliseconds(200));
    }
  });
}

void Agent::connect_pub(chrono::milliseconds delay) {
  // Must be attached before connect()/bind(), or libzmq may fire (and this
  // miss) the very first lifecycle event. attach() rather than start(): the
  // agent's own _io_thread drives both monitors (§4.1), so neither needs a
  // thread of its own.
  _pub_monitor.attach(_publisher);
  // Started here, not after connect(): the handshake wait below needs
  // something draining the monitor before it can observe anything.
  _start_io_thread();
  if (_cross) {
    string port = _sub_endpoint.substr(_sub_endpoint.find_last_of(":") + 1);
    _sub_endpoint = "tcp://*:" + port;
    _publisher.bind(_sub_endpoint);
    if (delay.count() > 0)
      this_thread::sleep_for(delay);
  } else {
    _publisher.connect(_pub_endpoint);
    if (delay.count() > 0) {
      // A real ZMQ_EVENT_HANDSHAKE_SUCCEEDED replaces the blind sleep the
      // PUB/SUB slow-joiner problem used to require (ZMQ_DEVELOPMENT.md
      // §2.1), falling back to waiting out the full `delay` -- the previous
      // worst case -- if the handshake never completes.
      //
      // The handshake alone is NOT enough to publish on, which is why this
      // waits on it rather than on wait_for_connection()'s bare
      // ZMQ_EVENT_CONNECTED. Both fire before the broker's XSUB frontend has
      // forwarded the fleet's subscriptions back to this publisher, and a PUB
      // socket discards -- silently -- anything sent while no subscription
      // matches. A long-running agent never notices; a one-shot publisher
      // (`mads-bridge -m`, i.e. every `mads-command` invocation) loses its
      // only message. So the handshake is followed by a settle grace, capped
      // by whatever is left of the caller's own `delay` so the total wall
      // time can never exceed what the blind sleep already cost.
      auto const started = chrono::steady_clock::now();
      _pub_monitor.wait_handshake_succeeded(delay);
      auto const elapsed = chrono::duration_cast<chrono::milliseconds>(
          chrono::steady_clock::now() - started);
      auto const grace = std::min(delay - elapsed, SUBSCRIPTION_SETTLE_DELAY);
      if (grace > 0ms)
        this_thread::sleep_for(grace);
    }
  }
}

bool Agent::wait_for_connection(chrono::milliseconds timeout) {
  return _pub_monitor.wait_connected(timeout);
}

Mads::LinkState Agent::link_state() const {
  // Bound sockets report per-peer departures and no per-peer arrivals, so
  // their events cannot be condensed into one link status; see the header.
  if (_cross)
    return {};
  // The subscriber is the socket over which an agent would actually notice
  // "the broker is gone" (missing traffic), so it takes priority; the
  // publisher is consulted only when the subscriber's monitor has not seen
  // anything link-relevant yet -- which for a publish-only agent is always,
  // since connect_sub() (and with it _sub_monitor.start()) never runs.
  auto sub = _sub_monitor.state();
  if (sub.status != Mads::LinkStatus::Unknown)
    return sub;
  return _pub_monitor.state();
}

void Agent::connect_sub() {
  _sub_monitor.attach(_subscriber);
  _subscriber.set(zmq::sockopt::rcvtimeo, _receive_timeout);
  if (_cross) {
    string port = _pub_endpoint.substr(_pub_endpoint.find_last_of(":") + 1);
    _pub_endpoint = "tcp://*:" + port;
    _subscriber.bind(_pub_endpoint);
  } else
    _subscriber.connect(_sub_endpoint);
  // P2 (MQTT-style wildcards): a sub_topic entry with no '+'/'#' subscribes
  // exactly as before -- identical subscribe() call, identical wire
  // SUBSCRIBE frame. Only entries containing a wildcard token take the
  // two-stage path: subscribe the broader literal_prefix() at the ZMQ layer,
  // then filter with Mads::topic_match() before a message ever reaches
  // receive_raw() (see below and _topic_matches_subscription()).
  _wildcard_sub_topic.clear();
  for (auto &t : _sub_topic) {
    if (Mads::has_wildcard(t)) {
      _wildcard_sub_topic.push_back(t);
      _subscriber.set(zmq::sockopt::subscribe, Mads::literal_prefix(t));
    } else {
      _subscriber.set(zmq::sockopt::subscribe, t);
    }
  }
  // CLOCKSYNC_TOPIC (§"Clock offset"): a ZMQ-level subscribe issued
  // separately from the loop above so it never appears in _sub_topic/
  // sub_topic() -- see _configure_clock_sync()'s comment. Matched in
  // _topic_matches_subscription() under the same _clock_wants_sync() gate,
  // and always intercepted (never surfaced to the app) inside receive().
  if (_clock_wants_sync()) {
    _subscriber.set(zmq::sockopt::subscribe, CLOCKSYNC_TOPIC);
  }
  // Hand _subscriber over to the I/O thread iff LKV delivery and/or threaded
  // remote control need it consumed off the application thread. Published
  // only now, with release ordering, so the thread cannot start polling a
  // socket this function is still subscribing on.
  _io_reads_subscriber.store(_last_value_only || _rc_owns_socket,
                             std::memory_order_release);
  // A subscribe-only agent never went through connect_pub(), so this may be
  // the first chance to bring the thread up; it is a no-op if it is running.
  _start_io_thread();
}

void Agent::_start_io_thread() {
  if (_io_thread.joinable())
    return;
  // One thread for every pollable socket this agent owns (§4.1): the
  // subscriber, when LKV/threaded remote control put it under this thread's
  // exclusive ownership, plus both socket monitors' PAIR sockets, which used
  // to cost a thread each. The loop condition is keep_running() alone --
  // _connected is written by connect() *after* this thread is spawned, so
  // reading it here would be both a data race and a startup race the thread
  // could lose, exiting immediately.
  _io_thread = thread([this]() {
    zmq::pollitem_t items[3];
    zmq::multipart_t msg;
    while (keep_running()) {
      // Rebuilt every iteration: connect_pub() starts this thread, and
      // connect_sub() may attach the subscriber's monitor -- and hand over
      // the subscriber itself -- only afterwards. Three cheap loads.
      int n = 0, sub_slot = -1, pub_mon_slot = -1, sub_mon_slot = -1;
      if (_io_reads_subscriber.load(std::memory_order_acquire)) {
        sub_slot = n;
        items[n++] = {_subscriber.handle(), 0, ZMQ_POLLIN, 0};
      }
      auto pub_mon = _pub_monitor.pollable();
      if (pub_mon.handle() != nullptr) {
        pub_mon_slot = n;
        items[n++] = {pub_mon.handle(), 0, ZMQ_POLLIN, 0};
      }
      auto sub_mon = _sub_monitor.pollable();
      if (sub_mon.handle() != nullptr) {
        sub_mon_slot = n;
        items[n++] = {sub_mon.handle(), 0, ZMQ_POLLIN, 0};
      }
      // Capped independently of _receive_timeout, which an application may
      // set to seconds: a monitor event (and this thread's own exit) should
      // not have to wait that long.
      const auto wait = chrono::milliseconds(std::min(_receive_timeout, 100));
      try {
        if (n == 0) {
          this_thread::sleep_for(wait);
          continue;
        }
        zmq::poll(items, n, wait);
        if (pub_mon_slot >= 0 && (items[pub_mon_slot].revents & ZMQ_POLLIN))
          _pub_monitor.process_pending();
        if (sub_mon_slot >= 0 && (items[sub_mon_slot].revents & ZMQ_POLLIN))
          _sub_monitor.process_pending();

        if (sub_slot < 0 || !(items[sub_slot].revents & ZMQ_POLLIN)) continue;
        if (!msg.recv(_subscriber, ZMQ_DONTWAIT)) continue;
        if (msg.size() == 0) continue;
        const string topic = msg.at(0).to_string();
        // Drop wildcard-subscribed messages that don't actually match
        // (the ZMQ-level subscribe is only a broader prefix).
        if (!_wildcard_sub_topic.empty() && !_topic_matches_subscription(topic))
          continue;

        // "control" and CLOCKSYNC_TOPIC messages are distinct channels,
        // dispatched below, never also stored as an LKV value. This is the
        // path an LKV/threaded-remote-control agent's clocksync traffic
        // takes -- its own receive()/receive_raw() never touches the
        // subscriber socket directly (see _io_reads_subscriber), so
        // without this branch such an agent could neither answer pings nor
        // hear domain announcements.
        if (topic == CLOCKSYNC_TOPIC) {
          if (msg.size() < 2) continue;
          string j;
          bool ok = false;
          WireHeader hdr;
          if (parse_wire_header(msg.at(1).to_string(), hdr) &&
              !hdr.has_blob && msg.size() >= 3) {
            ok = decode_to_json_text(msg.at(2).to_string(), hdr.format,
                                     hdr.compression, j);
          } else if (msg.size() == 2) {
            string payload = msg.at(1).to_string();
            ok = snappy::Uncompress(payload.data(), payload.size(), &j);
          }
          if (ok) {
            try {
              _handle_clocksync_message(nlohmann::json::parse(j));
            } catch (...) {
            }
          } else {
            _dropped_messages++;
          }
          continue;
        }

        if (_remote_controlled && topic == "control") {
          if (msg.size() < 2) continue;
          string j;
          bool ok = false;
          WireHeader hdr;
          if (parse_wire_header(msg.at(1).to_string(), hdr) &&
              !hdr.has_blob && msg.size() >= 3) {
            ok = decode_to_json_text(msg.at(2).to_string(), hdr.format,
                                     hdr.compression, j);
          } else if (msg.size() == 2) {
            string payload = msg.at(1).to_string();
            ok = snappy::Uncompress(payload.data(), payload.size(), &j);
          }
          if (ok)
            remote_control(j);
          else
            _dropped_messages++;
          continue;
        }

        if (_last_value_only) {
          std::lock_guard<std::mutex> lock(_latest_message.mtx);
          _latest_message.value = msg.clone();
          _latest_message.cv.notify_one();
        }
        // else: threaded remote control is active but LKV is not, and this
        // wasn't a control message -- matches the old dedicated
        // remote-control thread's behaviour of silently dropping ordinary
        // traffic in that configuration (receive() is unusable there anyway;
        // see its _rc_owns_socket guard).
      } catch (...) {}
    }
  });
}

bool Agent::_topic_matches_subscription(const string &topic) const {
  // CLOCKSYNC_TOPIC is subscribed at the ZMQ layer outside of _sub_topic
  // (connect_sub()), so it needs the same carve-out here: otherwise an
  // agent with an MQTT wildcard sub_topic entry (P2) would have this
  // wildcard-only filter silently drop every clock-sync frame the raw
  // subscribe just asked for.
  if (topic == CLOCKSYNC_TOPIC && _clock_wants_sync())
    return true;
  // One rule for both entry kinds, shared verbatim with `mads doctor --graph`
  // (see Mads::subscription_match()): literal entries keep the byte-prefix
  // acceptance the raw ZMQ SUBSCRIBE frame already applies, wildcard entries
  // get the full MQTT-style match rather than just the broader
  // literal_prefix() that was actually subscribed at the ZMQ layer.
  for (auto &t : _sub_topic) {
    if (Mads::subscription_matches(t, topic))
      return true;
  }
  return false;
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
  _subscriber.set(zmq::sockopt::rcvtimeo, _receive_timeout);
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
  _subscriber.set(zmq::sockopt::conflate, static_cast<int>(conflate));
  _conflate = conflate;
}

bool Agent::conflate() {
  return _conflate;
}


void Agent::set_high_watermark(int i) {
  if (_connected) throw AgentError("Cannot set_high_watermark after connection");
  // i == 0 now means "unlimited" (ZMQ semantics); it no longer silently
  // switches to Last-Known-Value mode as it used to (REFACTOR.md §1.4).
  _subscriber.set(zmq::sockopt::rcvhwm, i);
  // Backward-compatible convenience: a queue of exactly 1 selects LKV delivery.
  set_delivery(i == 1 ? Delivery::LastKnownValue : Delivery::Queued);
}

int Agent::high_watermark() {
  return _subscriber.get(zmq::sockopt::rcvhwm);
}

void Agent::_apply_socket_options() {
  auto opts = Mads::detail::SocketOptions::resolve(_config["agents"], _config[_name]);
  opts.apply(_publisher);
  opts.apply(_subscriber);
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
