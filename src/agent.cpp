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

// Private methods implementations

tuple<string, filesystem::path> Agent::read_settings(string uri, string name, int timeout) {
  zmqpp::socket socket(_context, zmqpp::socket_type::req);
  if (_curve_auth) {
    _curve_auth->setup_curve_client(socket, client_key_name, server_key_name);
  }

  message msg_out, msg_in;
  tuple<string, filesystem::path> result;
  socket.connect(uri);
  if (timeout > 0) {
    socket.set(zmqpp::socket_option::receive_timeout, timeout);
    socket.set(zmqpp::socket_option::send_timeout, timeout);
  }
  msg_out << LIB_VERSION << "settings" << name;
  if (!socket.send(msg_out)) {
    throw AgentError("Timed out in sending settings request to broker");
  }
  if (!socket.receive(msg_in)) {
    throw AgentError("Timed out in receiving settings from broker");
  }
  socket.disconnect(uri);
  socket.close();
  if (msg_in.parts() < 2) {
    throw AgentError(
        "Broker refuses to provide settings, check for version mismatch or "
        "missing settings for agent '" +
        name + "'");
  }
  string version_str = msg_in.get(0);
  if (!Mads::check_version(version_str)) {
    throw AgentError("Received settings from broker with wrong version: " +
      version_str);
  }
  if (msg_in.parts() == 3) {
    auto tmp_mads_dir = filesystem::temp_directory_path() / "mads";
    if (!filesystem::exists(tmp_mads_dir)) {
      if (!filesystem::create_directory(tmp_mads_dir)) {
        throw AgentError("Failed to create temporary directory for attachments");
      }
    }
    // Save the attachment to a temporary file
    auto tmp_file = tmp_mads_dir / (name + ".plugin");
    ofstream ofs(tmp_file, ios::out | ios::binary);
    if (!ofs) {
      throw AgentError("Failed to open temporary file for writing attachment from broker");
    }
    ofs.write(msg_in.get(2).data(), msg_in.get(2).size());
    ofs.close();
    result = make_tuple(msg_in.get(1), tmp_file);
  } else {
    result = make_tuple(msg_in.get(1), filesystem::path());
  }
  return result;
}

double Agent::get_broker_timecode(string uri, int timeout) {
  zmqpp::socket socket(_context, zmqpp::socket_type::req);
  if (_curve_auth) {
    _curve_auth->setup_curve_client(socket, client_key_name, server_key_name);
  }
  message msg;
  if (timeout > 0)
    socket.set(zmqpp::socket_option::receive_timeout, timeout);
  socket.connect(uri);
  msg << string("v") + LIB_VERSION << "timecode";
  socket.send(msg);
  if (!socket.receive(msg)) {
    socket.disconnect(uri);
    socket.close();
    throw AgentError("Timed out in receiving timecode from broker");
  }
  double timecode = std::stod(msg.get(0));
  socket.disconnect(uri);
  socket.close();
  return timecode;
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

void Agent::init(string name, string settings_uri, bool crypto, filesystem::path const &key_dir) {
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
  init(crypto);
}

void Agent::init(bool crypto) {
  _crypto = crypto;
  if (_crypto) {
    _curve_auth = make_unique<CurveAuth>(_context);
    _curve_auth->set_key_dir(_key_dir);
    _curve_auth->setup_auth(auth_verbose);
    _curve_auth->setup_curve_client(_subscriber, client_key_name, server_key_name);
    _curve_auth->setup_curve_client(_publisher, client_key_name, server_key_name);
  }
  // Load config URI/file
  if (settings_are_local()) {
    _config = (toml::table)toml::parse_file(_settings_uri);
  } else {
    double broker_tc;
    auto received = read_settings(_settings_uri, _name, _settings_timeout);
    _raw_settings = get<0>(received);
    _attachment_path = get<1>(received);
    chrono::system_clock::time_point now = chrono::system_clock::now();
    broker_tc = get_broker_timecode(_settings_uri, _settings_timeout);
    _timecode_offset = broker_tc - timecode(now, timecode_fps);
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

  load_settings();

  _init_done = true;
}

void Agent::load_settings() {}

Agent::~Agent() {
  disconnect();
  _curve_auth = nullptr;
  _publisher.close();
  _subscriber.close();
  _context.terminate();
}

nlohmann::json Agent::get_settings() {
  stringstream ss;
  ss << toml::json_formatter{*_config[_name].as_table()};
  return nlohmann::json::parse(ss.str());
}

#ifndef MADS_AGENT_NO_INFO
void Agent::info(ostream &out) {
  if (!_init_done)
    throw AgentError("Agent not initialized");
  out << "Agent: " << style::bold << fg::green << _name << fg::reset
      << style::reset << endl;
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
  if (_connected) return;
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

void Agent::register_event(const event_type event, const nlohmann::json &info) {
  if (!_init_done)
    throw AgentError("Agent not initialized");
  nlohmann::json settings = get_settings();
  thread t([info, event, settings, this]() {
    nlohmann::json payload;
    if (event == event_type::startup)
      this_thread::sleep_for(chrono::milliseconds(STARTUP_SHUTDOWN_DELAY));
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
      payload["info"] = info;
    }
    publish(payload, METADATA_TOPIC);
  });
  if (event == event_type::shutdown) {
    // wait for the thread to publish the message
    this_thread::sleep_for(chrono::milliseconds(STARTUP_SHUTDOWN_DELAY));
    t.join();
  } else {
    t.detach();
  }
}

void Agent::publish(nlohmann::json payload, string topic) {
  if (!_init_done)
    throw AgentError("Agent not initialized");
  message message;
  string str;
  int32_t offset = 0;
  if (payload.contains("event"))
    if (payload["event"] == event_map.at(event_type::shutdown) ||
        payload["event"] == event_map.at(event_type::startup)) {
      offset = STARTUP_SHUTDOWN_DELAY;
    }
  chrono::system_clock::time_point now = chrono::system_clock::now();
  payload["hostname"] = _hostname;
  payload["timestamp"]["$date"] = get_ISODate_time(now, -offset);
  if (!payload.contains("timecode")) {
    payload["timecode"] = timecode(now, timecode_fps) - offset;
  }
  str = payload.dump();
  if (topic.empty()) {
    topic = _pub_topic;
  }
  string compressed;
  snappy::Compress(str.data(), str.size(), &compressed);
  message << topic << compressed;
  _publisher.send(message);
}

void Agent::publish(const char *payload, size_t len,
           nlohmann::json meta, string topic) {
  if (!_init_done)
    throw AgentError("Agent not initialized");
  message message;
  chrono::system_clock::time_point now = chrono::system_clock::now();
  meta["timestamp"]["$date"] = get_ISODate_time(now);
  meta["timecode"] = timecode(now, timecode_fps);
  if (topic.empty())
    topic = _pub_topic;
  message << topic << meta.dump();
  message.add_raw(payload, len);
  _publisher.send(message);
}

void Agent::publish(const vector<unsigned char> &payload,
           nlohmann::json meta, string topic) {
  char *data = (char *)payload.data();
  publish(data, payload.size(), meta, topic);
}

message_type Agent::receive(bool dont_block) {
  if (!_init_done)
    throw AgentError("Agent not initialized");
  message message;
  message_type result = message_type::none;
  string topic, format, payload, j;
  if (!_subscriber.receive(message, dont_block)) {
    return result;
  }
  switch (message.parts()) {
  case 0:
    throw AgentError("Received message with no parts");
  case 1:
    throw AgentError("Received message with only one part");
  case 2: // Payload is JSON
    message >> topic >> payload;
    if (payload.empty()) {
      result = message_type::none;
      break;
    }
    snappy::Uncompress(payload.data(), payload.size(), &j);
    if (_remote_controlled && topic == "control") {
      remote_control(j);
      break;
    }
    _status[topic] = j;
    _last_message = make_tuple(topic, j);
    result = message_type::json;
    break;
  case 3: // Payload is a binary blob, type is in message[1]
    message >> topic >> format >> payload;
    _last_blob = make_tuple(
        topic, format, vector<unsigned char>(payload.begin(), payload.end()));
    result = message_type::blob;
    break;
  default:
    throw AgentError("Received message with "s + to_string(message.parts()) +
                     " parts"s);
  }
  return result;
}

void Agent::loop(std::function<void()> const &lambda,
          chrono::milliseconds duration) {
  if (!_init_done)
    throw AgentError("Agent not initialized");
  std::signal(SIGINT, [](int signum) {
    UNUSED(signum);
    Mads::running = false;
  });
  std::signal(SIGTERM, [](int signum) {
    UNUSED(signum);
    Mads::running = false;
  });
  while (running) {
    if (duration > chrono::milliseconds(0)) {
      thread t([&]() { this_thread::sleep_for(duration); });
      lambda();
      t.join();
    } else {
      lambda();
    }
  }
}

void Agent::loop(std::function<void()> const &lambda) {
  loop(lambda, _time_step);
}

void Agent::enable_remote_control(bool threaded) {
  if (!_init_done)
    throw AgentError("Agent not initialized");
  if (_connected)
    throw AgentError("Cannot enable remote control after connecting");
  _remote_controlled = true;
  _sub_topic.push_back("control");
  if (threaded)
    thread([&]() {
      _subscriber.set(zmqpp::socket_option::receive_timeout, 500);
      message msg;
      string topic, payload, j;
      while (Mads::running) {
        _subscriber.receive(msg, false);
        if (msg.parts() == 2) {
          msg >> topic >> payload;
          if (topic == "control") {
            snappy::Uncompress(payload.data(), payload.size(), &j);
            remote_control(j);
          }
        }
      }
    }).detach();
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
    Mads::running = false;
  } else if (command == "shutdown") {
    Mads::running = false;
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

map<string, string> Agent::status() { return _status; }

string Agent::name() { return _name; }

tuple<string, string> Agent::last_message() { return _last_message; }

string Agent::last_topic() { return get<0>(_last_message); }

tuple<string, string, vector<unsigned char>> Agent::last_blob() {
  return _last_blob;
}


bool Agent::is_connected() { return _connected; }

int Agent::settings_timeout() { return _settings_timeout; }

void Agent::set_settings_timeout(int to) {
  if (_init_done)
    throw AgentError("Cannot set timeout after initialization");
  _settings_timeout = to;
}

int Agent::receive_timeout() { return _receive_timeout; }

void Agent::set_receive_timeout(int to) {
  _receive_timeout = to;
  _subscriber.set(zmqpp::socket_option::receive_timeout, _receive_timeout);
}

bool Agent::restart() { return _restart; }

filesystem::path Agent::attachment_path() {
  return _attachment_path;
}

bool Agent::is_crypto() {
  return _crypto;
}

void Agent::set_crypto(Mads::auth_verbose verbose) {
  _crypto = true;
  _curve_auth = make_unique<CurveAuth>(_context);
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

} // namespace Mads