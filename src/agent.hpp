/*
     _                    _          _
    / \   __ _  ___ _ __ | |_    ___| | __ _ ___ ___
   / _ \ / _` |/ _ \ '_ \| __|  / __| |/ _` / __/ __|
  / ___ \ (_| |  __/ | | | |_  | (__| | (_| \__ \__ \
 /_/   \_\__, |\___|_| |_|\__|  \___|_|\__,_|___/___/
         |___/

Base class for all agents. This class is used to define the basic
functionalities provided by all agents. Each agent subclass must implement the
pure virtual functions defined in this class (currently none)

Author(s): Paolo Bosetti
*/

#ifndef AGENT_HPP
#define AGENT_HPP

#if defined _WIN32 && !defined NOMINMAX
#define NOMINMAX
#endif

#include "mads.hpp"
#include <nlohmann/json.hpp>
#ifdef _WIN32
#include <winsock.h>
#else
#include <unistd.h>
#endif
#ifndef _MSC_VER
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-attributes"
#endif
#include <toml++/toml.hpp>
#ifndef _MSC_VER
#pragma GCC diagnostic pop
#endif
#include <csignal>
#include <iostream>
#include <regex>
#include <snappy.h>
#include <string>
#include <string_view>
#include <thread>
#include <zmqpp/zmqpp.hpp>

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 255
#endif

#ifndef MADS_AGENT_NO_INFO
#include <rang.hpp>
using namespace rang;
#endif

using namespace std::string_view_literals;
using namespace std::string_literals;
using namespace zmqpp;
using namespace std;

#define STARTUP_SHUTDOWN_DELAY 500

namespace Mads {

/**
 * @brief The Agent class represents an agent in the mads system.
 *
 * An agent is an entity that can send and receive messages through ZeroMQ
 * sockets. It is responsible for loading settings, connecting to the
 * appropriate endpoints, and publishing and receiving messages.
 * @example
 *  // suppose that you have the derived class MyAgent
 * // Create a MyAgent object with the name "myagent" and the settings file
 * MyAgent myagent("myagent", "settings.ini");
 * myagent.init(); // may throw an error is ini file has errors
 * // either of the two or bothMyAgent:
 * myagent.connect_pub();
 * myagent.connect_sub();
 * // get info about the myagent
 * myagent.info();
 * // Start a main loop with a lambda function:
 * myagent.loop([&]() {
 *   // receive a message
 *   myagent.receive();
 *   // get the last message received
 *   myagent.last_message();
 *   // get the status of the myagent agent, i.e. a map of all last messages by
 *   //  topics
 *   auto status = myagent.status();
 *   string msg = status["topic1"];
 *   // publish a message
 *   myagent.publish(json_object.dump());
 * });
 */
class Agent {

/*
  ____  _        _   _      
 / ___|| |_ __ _| |_(_) ___ 
 \___ \| __/ _` | __| |/ __|
  ___) | || (_| | |_| | (__ 
 |____/ \__\__,_|\__|_|\___|
                            
*/

public:
  /**
   * @brief Static function to read settings from broker.
   *
   * @param uri The URI of the broker.
   * @param name The name of the agent.
   * @param timeout The timeout in milliseconds (default 2000).
   * @return The settings as a string.
   * @throws AgentError if timed out in reading settings from broker.
   */
  static string read_settings(string uri, string name, int timeout = 2000) {
    zmqpp::context context;
    zmqpp::socket socket(context, zmqpp::socket_type::req);
    message msg;
    if (timeout > 0)
      socket.set(zmqpp::socket_option::receive_timeout, timeout);
    socket.connect(uri);
    msg << "settings" << name;
    socket.send(msg);
    if (!socket.receive(msg)) {
      socket.disconnect(uri);
      socket.close();
      throw AgentError("Timed out in receiving settings from broker");
    }
    socket.disconnect(uri);
    socket.close();
    context.terminate();
    return msg.get(0);
  }


/*
  _     _  __                      _      
 | |   (_)/ _| ___  ___ _   _  ___| | ___ 
 | |   | | |_ / _ \/ __| | | |/ __| |/ _ \
 | |___| |  _|  __/ (__| |_| | (__| |  __/
 |_____|_|_|  \___|\___|\__, |\___|_|\___|
                        |___/             
*/

  /**
   * @brief Constructs an Agent object with the given name and settings path.
   * If the settings path is a URI, the agent will connect to the broker with
   * a REQ/REP socket and request the same settings file loaded by the broker.
   *
   * @param name The name of the agent. If it is a path, it take the filename.
   * @param settings_path The path or URI to the settings file for the agent.
   */
  Agent(string name, string settings_uri)
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
  }


  /**
   * @brief Initializes the agent.
   *
   * This function loads the settings file and sets the member variables of the
   * agent.
   *
   * @param name The name of the agent (as it is).
   * @param settings_path The path or URI to the settings file for the agent.
   * @throws AgentError if timed out in reading settings from broker.
   */
  void init(string name, string settings_uri) {
    size_t pos = name.rfind('-');
    if (pos != std::string::npos) {
      _name = name.substr(pos + 1);
    } else {
      _name = name;
    }
    _settings_uri = settings_uri;
    init();
  }


  /**
   * @brief Initializes the agent.
   *
   * This function loads the settings file and sets the member variables of the
   * agent.
   *
   * @throws AgentError if timed out in reading settings from broker.
   */
  void init() {
    // Load config URI/file
    if (settings_are_local()) {
      _config = (toml::table)toml::parse_file(_settings_uri);
    } else {
      _raw_settings = read_settings(_settings_uri, _name, _settings_timeout);
      _config = (toml::table)toml::parse(_raw_settings);
    }

    // member variables
    auto all_cfg = _config["agents"];
    _compress = all_cfg["compress"].value_or(false);
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

    load_settings();

    _init_done = true;
  }

  // Destructor
  ~Agent() {
    disconnect();
    _publisher.close();
    _subscriber.close();
    _context.terminate();
  }


/*
  ____       _   _   _                 
 / ___|  ___| |_| |_(_)_ __   __ _ ___ 
 \___ \ / _ \ __| __| | '_ \ / _` / __|
  ___) |  __/ |_| |_| | | | | (_| \__ \
 |____/ \___|\__|\__|_|_| |_|\__, |___/
                             |___/     
*/

  /**
   * @brief Additional settings to be loaded. Virtual function to be
   * implemented by the derived class.
   *
   * This function is automatically called by init() to load additional
   * settings.
   */
  virtual void load_settings(){};


  /**
   * @brief Save settings read from broker to file.
   *
   * @param path
   * @throws AgentError if not initialized or settings are local.
   */
  void save_settings(const string path = SETTINGS_PATH) {
    if (!_init_done)
      throw AgentError("Agent not initialized");

    if (settings_are_local()) {
      throw AgentError("Can only save settings loaded from broker");
    }
    ofstream out(path);
    out << _raw_settings;
    out.close();
  }


  /**
   * @brief Get all settings as JSON
   *
   */
  nlohmann::json get_settings() {
    stringstream ss;
    ss << toml::json_formatter{*_config[_name].as_table()};
    return nlohmann::json::parse(ss.str());
  }


#ifndef MADS_AGENT_NO_INFO
  /**
   * @brief Prints information about the agent.
   *
   * This function prints the agent's name, settings file path, publish
   * endpoint, publish topic, subscribe endpoint, and subscribe topics.
   *
   * @param out The output stream to be used (default is cout).
   * @throws AgentError if not initialized
   */
  virtual void info(ostream &out = cout) {
    if (!_init_done)
      throw AgentError("Agent not initialized");
    out << "Agent: " << style::bold << fg::green << _name << fg::reset
        << style::reset << endl;
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
    out << "  Compression:      " << style::bold;
    if (_compress)
      out << "enabled" << style::reset << endl;
    else
      out << fg::red << "disabled" << fg::reset << style::reset << endl;
  }
#endif


/*
   ____                            _   _             
  / ___|___  _ __  _ __   ___  ___| |_(_) ___  _ __  
 | |   / _ \| '_ \| '_ \ / _ \/ __| __| |/ _ \| '_ \ 
 | |__| (_) | | | | | | |  __/ (__| |_| | (_) | | | |
  \____\___/|_| |_|_| |_|\___|\___|\__|_|\___/|_| |_|
                                                     
*/

  /**
   * @brief Connects the agent to the publish and subscribe endpoints.
   *
   * Optionally, this function accepts a delay parameter, which is the delay
   * after connecting. This is needed to ensure that the agent is connected
   * before sending messages. The default delay is 250 milliseconds.
   *
   * @param type The type of connection to be established.
   * @param delay The delay in milliseconds after connecting.
   * @throws AgentError if not initialized
   */
  void connect(chrono::milliseconds delay = chrono::milliseconds(0)) {
    if (!_init_done)
      throw AgentError("Agent not initialized");
    if (!_pub_topic.empty())
      connect_pub(delay);
    if (!_sub_topic.empty())
      connect_sub();
    _connected = true;
  }


  /**
   * @brief Disconnects the agent from the publish and subscribe endpoints.
   */
  void disconnect() {
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


  /**
   * @brief Sets the cross flag.
   *
   * If the cross flag is set, the agent will bind to the publish endpoint and
   * connect to the subscribe endpoint. This is useful for testing purposes.
   *
   * @param cross The value of the cross flag.
   */
  void set_cross(bool cross) { _cross = cross; }


  /**
   * @brief Sets the publish topic.
   *
   * @param topic The topic to be used for publishing messages.
   */
  void set_pub_topic(string topic) { _pub_topic = topic; }


  /**
   * @brief Enables remote control for the agent.
   *
   * This function subscribes the agent to the "control" topic. If the agent
   * is not a subscriber starts a thread to handle remote control commands.
   *
   * If the agent is also a subscriber, then the remote control is handled
   * in the main loop by calling the remote_control() function.
   *
   * @throws AgentError if not initialized
   * @throws AgentError if already connected
   */
  void enable_remote_control() {
    if (!_init_done)
      throw AgentError("Agent not initialized");
    if (_connected)
      throw AgentError("Cannot enable remote control after connecting");
    bool subscriber = !_sub_topic.empty();
    _sub_topic.push_back("control");
    if (!subscriber)
      control_thread = new thread([&]() {
        _subscriber.set(zmqpp::socket_option::receive_timeout, 100);
        while (Mads::running) {
          message_type type;
          try {
            type = receive();
          } catch (const std::exception &e) {
            cerr << "Error receiving message: " << e.what() << endl;
            continue;
          }
          if (type != message_type::json)
            continue;
          remote_control();
        }
      });
  }


  /**
   * @brief Registers an event.
   *
   * This function registers an event with the broker. The event is sent after
   * 500 milliseconds.
   *
   * @param event The event to be registered.
   * @throws AgentError if not initialized
   */
  void register_event(const event_type event = event_type::marker,
                      const nlohmann::json &info = nlohmann::json()) {
    if (!_init_done)
      throw AgentError("Agent not initialized");
    nlohmann::json settings = get_settings();
    thread t([=, this]() {
      nlohmann::json payload;
      if (event == event_type::startup)
        this_thread::sleep_for(chrono::milliseconds(STARTUP_SHUTDOWN_DELAY));
      payload["name"] = _name;
      payload["version"] = LIB_VERSION;
      payload["event"] = event_map.at(event);
      payload["settings_path"] = _settings_uri;
      payload["settings"] = settings;
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


  /**
   * @brief Publishes a message with the given JSON payload.
   *
   * @param payload The JSON payload of the message.
   * @throws AgentError if not initialized
   */
  void publish(nlohmann::json payload, string topic = "") {
    if (!_init_done)
      throw AgentError("Agent not initialized");
    message message;
    string str;
    uint32_t offset = 0;
    if (payload.contains("event"))
      if (payload["event"] == event_map.at(event_type::shutdown) ||
          payload["event"] == event_map.at(event_type::startup)) {
        offset = STARTUP_SHUTDOWN_DELAY;
      }
    chrono::system_clock::time_point now = chrono::system_clock::now();
    payload["hostname"] = _hostname;
    payload["timestamp"]["$date"] = get_ISODate_time(now, -offset);
    payload["timecode"] = timecode(now, MADS_FPS) - offset;
    str = payload.dump();
    if (topic.empty())
      topic = _pub_topic;
    if (_compress) {
      string compressed;
      snappy::Compress(str.data(), str.size(), &compressed);
      message << topic << compressed;
    } else {
      message << topic << str;
    }
    _publisher.send(message);
  }


  /**
   * @brief Publishes a message with the given binary blob payload.
   *
   * @param payload The binary blob payload of the message.
   * @param format The format of the blob (a string, default "raw").
   * @param topic The topic of the message.
   * @throws AgentError if not initialized
   */
  void publish(const char *payload, size_t len,
               nlohmann::json meta = nlohmann::json{{"format", "raw"}},
               string topic = "") {
    if (!_init_done)
      throw AgentError("Agent not initialized");
    message message;
    chrono::system_clock::time_point now = chrono::system_clock::now();
    meta["timestamp"]["$date"] = get_ISODate_time(now);
    meta["timecode"] = timecode(now, MADS_FPS);
    if (topic.empty())
      topic = _pub_topic;
    message << topic << meta.dump();
    message.add_raw(payload, len);
    _publisher.send(message);
  }

  
  /**
   * @brief Publishes a message with the given binary blob payload.
   *
   * @param payload The binary blob payload of the message.
   * @param metadata The metadata for the blob.
   * @param topic The topic of the message.
   * @throws AgentError if not initialized
   */
  void publish(const vector<unsigned char> &payload,
               nlohmann::json meta = nlohmann::json{{"format", "raw"}},
               string topic = "") {
    char *data = (char *)payload.data();
    publish(data, payload.size(), meta, topic);
  }


  /**
   * @brief Receives a message from the subscribe socket.
   *
   * This function receives a message from the subscribe socket and updates the
   * agent's status and last received message.
   *
   * @throws AgentError if the received message has only one part or more than
   * two parts.
   * @throws AgentError if not initialized
   */
  message_type receive() {
    if (!_init_done)
      throw AgentError("Agent not initialized");
    message message;
    message_type result = message_type::none;
    string topic, format, payload, *j;
    if (!_subscriber.receive(message)) {
      return result;
    }
    switch (message.parts()) {
    case 0:
      throw AgentError("Received message with no parts");
    case 1:
      throw AgentError("Received message with only one part");
    case 2: // Payload is JSON
      message >> topic >> payload;
      if (_compress) {
        j = new string;
        snappy::Uncompress(payload.data(), payload.size(), j);
      } else {
        j = &payload;
      }
      _status[topic] = *j;
      _last_message = make_tuple(topic, *j);
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


  /*
    _                      
   | |    ___   ___  _ __  
   | |   / _ \ / _ \| '_ \ 
   | |__| (_) | (_) | |_) |
   |_____\___/ \___/| .__/ 
                    |_|    
  */

  /**
   * @brief Enters the main loop of the agent. It also sets a signal handler for
   * SIGNINT, which will set the running flag to false.
   *
   * @param lambda the function to be executed in the main loop
   * @param duration the duration of the loop (default 0, max speed)
   * @throws AgentError if not initialized
   */
  void loop(std::function<void()> const &lambda,
            chrono::milliseconds duration) {
    if (!_init_done)
      throw AgentError("Agent not initialized");
    std::signal(SIGINT, [](int signum) {
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


  /**
   * @brief Enters the main loop of the agent. It also sets a signal handler for
   * SIGNINT, which will set the running flag to false.
   * The loop duration is set to the time step of the agent, as pased in the 
   * ini file with the time_step parameter. It it is zero or not set,
   * it will run at max speed.
   *
   * @param lambda the function to be executed in the main loop
   * @throws AgentError if not initialized
   */
  void loop(std::function<void()> const &lambda) {
    loop(lambda, _time_step);
  }


  /**
   * @brief Handles remote control commands.
   *
   * This function is called by the main loop to handle remote control commands.
   * It checks if the last received message is a control message and acts
   * accordingly.
   */
  void remote_control() {
    auto msg = last_message();
    if (get<0>(msg) == "control") {
      nlohmann::json j = nlohmann::json::parse(get<1>(msg));
      if (j["cmd"] == "shutdown") {
        Mads::running = false;
      } else if (j["cmd"] == "restart") {
        _restart = true;
        Mads::running = false;
      }
    }
  }


  /*
       _                                        
      / \   ___ ___ ___  ___ ___  ___  _ __ ___ 
     / _ \ / __/ __/ _ \/ __/ __|/ _ \| '__/ __|
    / ___ \ (_| (_|  __/\__ \__ \ (_) | |  \__ \
   /_/   \_\___\___\___||___/___/\___/|_|  |___/
                                                
  */

  /**
   * @brief Returns the status of the system.
   *
   * @return A map containing the last messages for each subscribed topic.
   */
  map<string, string> status() { return _status; }


  /**
   * @brief Returns the name of the agent.
   *
   * @return The name of the agent.
   */
  string name() { return _name; }


  /**
   * @brief Returns the last received message by the agent.
   *
   * @return A tuple containing the topic and payload of the last received
   * message.
   */
  tuple<string, string> last_message() { return _last_message; }


  /**
   * @brief Returns the topic of the last received message by the agent.
   *
   * @return The topic of the last received message.
   */
  string last_topic() { return get<0>(_last_message); }


  /**
   * @brief Returns the last received blob by the agent.
   *
   * @return A tuple containing the topic, format, and payload of the last
   * received blob.
   */
  tuple<string, string, vector<unsigned char>> last_blob() {
    return _last_blob;
  }


  /**
   * @brief Detects if settings are local or loaded from URI.
   *
   * @return true or false.
   */
  bool settings_are_local() { return _settings_uri.rfind("tcp://", 0) != 0; }


  /**
   * @brief Detects if agent is connected.
   *
   * @return true or false.
   */
  bool is_connected() { return _connected; }


  /**
   * @brief Returns the value of timeout in loading settings from URI.
   *
   * @return the timeout in ms (default to 0, no timeout).
   */
  int settings_timeout() { return _settings_timeout; }


  /**
   * @brief Sets the value of timeout in loading settings from URI. Set to
   * for no timeout.
   *
   * @param to the timeout in ms.
   * @throws AgentError if already initialized
   */
  void set_settings_timeout(int to) {
    if (_init_done)
      throw AgentError("Cannot set timeout after initialization");
    _settings_timeout = to;
  }


  /**
   * @brief Returns wheter a restart has been requested.
   *
   * @return the restart flag.
   */
  bool restart() { return _restart; }


  /*
    ____       _            _
   |  _ \ _ __(_)_   ____ _| |_ ___
   | |_) | '__| \ \ / / _` | __/ _ \
   |  __/| |  | |\ V / (_| | ||  __/
   |_|   |_|  |_| \_/ \__,_|\__\___|

  */

protected:
  /**
   * @brief Connects the agent to the publish endpoint.
   *
   * @param delay The delay in milliseconds after connecting.
   */
  void connect_pub(chrono::milliseconds delay = chrono::milliseconds(0)) {
    if (_cross) {
      string port = _sub_endpoint.substr(_sub_endpoint.find_last_of(":") + 1);
      _sub_endpoint = "tcp://*:" + port;
      _publisher.bind(_sub_endpoint);
    } else
      _publisher.connect(_pub_endpoint);
    if (delay.count() > 0)
      this_thread::sleep_for(delay);
  }


  /**
   * @brief Connects the agent to the subscribe endpoint and subscribes to the
   * topics.
   */
  void connect_sub() {
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


  static tuple<string, string, string> split_URL(const string &url) {
    static regex re("(\\w+://)([\\w\\.-]+):(\\d+)");
    smatch match;

    if (regex_search(url, match, re) && match.size() > 3) {
      return make_tuple(match.str(1), match.str(2), match.str(3));
    } else {
      throw std::invalid_argument("Invalid URL: " + url);
    }
  }

  // Member variables
  string _hostname;
  string _name;
  string _settings_uri;
  string _raw_settings;
  toml::table _config;
  string _pub_endpoint, _sub_endpoint;
  string _pub_topic;
  vector<string> _sub_topic;
  context _context;
  zmqpp::socket _publisher;
  zmqpp::socket _subscriber;
  map<string, string> _status;
  tuple<string, string> _last_message;
  tuple<string, string, vector<unsigned char>> _last_blob;
  bool _compress = false;
  bool _cross = false;
  bool _connected = false;
  int _receive_timeout = 200;
  int _settings_timeout = 2000;
  bool _init_done = false;
  thread *control_thread = nullptr;
  bool _restart = false;
  chrono::milliseconds _time_step = chrono::milliseconds(0);

public:
  bool dummy = false;
};

} // namespace Mads

#endif // AGENT_HPP
