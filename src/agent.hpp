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
#include <winsock2.h>
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
#include <iostream>
#include <regex>
#include <string>
#include <string_view>
#include <thread>
#include <future>
#include <zmqpp/zmqpp.hpp>
#include <mutex>
#include <condition_variable>
#include <optional>
#include "curve.hpp"
#include "exec_path.hpp"

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 255
#endif

#ifndef MADS_AGENT_NO_INFO
#include <rang.hpp>
using namespace rang;
#endif

#define STARTUP_SHUTDOWN_DELAY 500

namespace Mads {

template<typename T> struct SharedLatest {
  std::mutex mtx;
  std::condition_variable cv;
  std::optional<T> value;
};

class Agent; // forward declaration

/**
 * @brief Quick Agent initialization function.
 *
 * @param name Agent's name
 * @param settings_uri URI for the settings file (e.g. "/path/to/mads.ini" or
 * "tcp://broker:5555")
 * @param crypto_settings A map with crypto settings. If empty, crypto is
 * disabled. If not empty, it must contain the following keys: key_dir,
 * key_client, key_broker.
 * @return Agent A unique pointer to the initialized Agent object. The agent is
 * already connected and ready to use.
 * @throws AgentError if there is an error in the initialization (e.g. settings
 * file
 */
std::unique_ptr<Agent> start_agent(std::string name, std::string settings_uri,
                                   std::map<std::string, std::string> crypto_settings = {});

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

private:
  /**
   * @brief Static function to read settings from broker.
   *
   * @param uri The URI of the broker.
   * @param name The name of the agent.
   * @param crypto Whether to use CURVE encryption (default false).
   * @param timeout The timeout in milliseconds (default 2000).
   * @return a tuple with the settings and possibly the path of the attachemnt.
   * @throws AgentError if timed out in reading settings from broker.
   */
  std::tuple<std::string, std::filesystem::path> read_settings(std::string uri, std::string name, int timeout = 2000);

  double get_broker_timecode(std::string uri, int timeout = 2000);

public:
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
  Agent(std::string name, std::string settings_uri);


  /**
   * @brief Initializes the agent.
   *
   * This function loads the settings file and sets the member variables of the
   * agent.
   * 
   * If the `settings_uri` is "none", the agent is initialized with default 
   * settings and does not attempt to load any settings file. This is useful 
   * for testing purposes.
   *
   * @param name The name of the agent (as it is).
   * @param settings_uri The path or URI to the settings file for the agent.
   * @param crypto Whether to use CURVE encryption (default false).
   * @param key_dir The directory where the CURVE keys are stored.
   * @throws AgentError if timed out in reading settings from broker.
   */
  void init(std::string name, std::string settings_uri, bool crypto = false, std::filesystem::path const &key_dir = "", bool install_watchdog = true);


  /**
   * @brief Initializes the agent.
   *
   * This function loads the settings file and sets the member variables of the
   * agent.
   *
   * @param crypto Whether to use CURVE encryption (default false).
   * @throws AgentError if timed out in reading settings from broker.
   */
  void init(bool crypto = false, bool install_watchdog = true);

  // Destructor
  virtual ~Agent();

  /**
   * @brief Install a watch thread to ensure exit from loops
   * 
   * This starts a low-frequency thread that forces and exit when 
   * `Mads::running` remains false for more than 3 seconds
   */
  void install_loop_watchdog(uint8_t max_count = 3);


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
  virtual void load_settings();


  /**
   * @brief Save settings read from broker to file.
   *
   * @param path
   * @throws AgentError if not initialized or settings are local.
   */
  void save_settings(const std::string path = SETTINGS_PATH);


  /**
   * @brief Get all settings as JSON
   *
   */
  nlohmann::json get_settings();


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
  virtual void info(std::ostream &out = std::cout);
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
  void connect(std::chrono::milliseconds delay = std::chrono::milliseconds(250));


  /**
   * @brief Disconnects the agent from the publish and subscribe endpoints.
   */
  void disconnect();


  /**
   * @brief Performs a coordinated shutdown of the agent.
   *
   * This ensures all background threads (drain, remote control) are properly
   * joined before closing sockets and terminating the ZMQ context.
   * Called automatically by the destructor.
   */
  void shutdown();


  /**
   * @brief Sets the cross flag.
   *
   * If the cross flag is set, the agent will bind to the publish endpoint and
   * connect to the subscribe endpoint. This is useful for testing purposes.
   *
   * @param cross The value of the cross flag.
   */
  void set_cross(bool cross);
  

  /**
   * @brief Enables remote control for the agent.
   *
   * This function subscribes the agent to the "control" topic. If the agent
   * is not a subscriber starts a thread to handle remote control commands.
   *
   * If the agent is also a subscriber, then the remote control is handled
   * in the main loop by calling the remote_control() function.
   *
   * @param threaded If true, the agent is NOT supposed to receive data (it is
   *        a pure sink) so messages are read on a searate thread
   * @throws AgentError if not initialized
   * @throws AgentError if already connected
   */
  void enable_remote_control(bool threaded = false);
  
  inline void enable_threaded_remote_control() {
    enable_remote_control(true);
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
                      const nlohmann::json &info = nlohmann::json(),
                      const std::string &info_name = "info");


  /**
   * @brief Publishes a message with the given JSON payload.
   *
   * @param payload The JSON payload of the message.
   * @throws AgentError if not initialized
   */
  void publish(nlohmann::json payload, std::string topic = "");


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
               std::string topic = "");

  
  /**
   * @brief Publishes a message with the given binary blob payload.
   *
   * @param payload The binary blob payload of the message.
   * @param metadata The metadata for the blob.
   * @param topic The topic of the message.
   * @throws AgentError if not initialized
   */
  void publish(const std::vector<unsigned char> &payload,
               nlohmann::json meta = nlohmann::json{{"format", "raw"}},
               std::string topic = "");


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
  message_type receive(bool dont_block = false);


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
   * @param lambda the function to be executed in the main loop; it can return 
   *  the requested duration of the next loop: if this is 0, the default loop 
   *  time is used, otherwise the value returned by the lambda
   * @param duration the duration of the loop (default 0, max speed)
   * @throws AgentError if not initialized
   */
  using loop_fun_t = std::function<std::chrono::milliseconds()>;
  void loop(loop_fun_t const &lambda,
            std::chrono::milliseconds duration);


  /**
   * @brief Enters the main loop of the agent. It also sets a signal handler for
   * SIGNINT, which will set the running flag to false.
   * The loop duration is set to the time step of the agent, as pased in the 
   * ini file with the time_step parameter. It it is zero or not set,
   * it will run at max speed.
   *
   * @param lambda the function to be executed in the main loop; it can return 
   *  the requested duration of the next loop: if this is 0, the default loop 
   *  time is used, otherwise the value returned by the lambda
   * @throws AgentError if not initialized
   */
  void loop(loop_fun_t const &lambda);


  /**
   * @brief Handles remote control commands.
   *
   * This function is called by the main loop to handle remote control commands.
   * It checks if the last received message is a control message and acts
   * accordingly.
   */
  void remote_control(std::string payload_str);


  /*
       _                                        
      / \   ___ ___ ___  ___ ___  ___  _ __ ___ 
     / _ \ / __/ __/ _ \/ __/ __|/ _ \| '__/ __|
    / ___ \ (_| (_|  __/\__ \__ \ (_) | |  \__ \
   /_/   \_\___\___\___||___/___/\___/|_|  |___/
                                                
  */

  /**
   * @brief Set the agent ID field
   * 
   * @param id The agent ID
   */
   void set_agent_id(std::string id);


  
  /**
   * @brief Get the agent ID
   * 
   * @return The agent ID
   */
  std::string get_agent_id();

  /**
   * @brief Sets the publish topic.
   *
   * @param topic The topic to be used for publishing messages.
   */
  void set_pub_topic(std::string topic);

  /**
   * @brief Gets the publish topic.
   *
   * @return The publish topic.
   */
  std::string pub_topic() const { return _pub_topic; }

  /**
   * @brief Sets the subscribe topics.
   *
   * @param topics The topics to be used for subscribing to messages.
   */
  void set_sub_topic(std::vector<std::string> topics) { _sub_topic = topics; }
  
  /**
   * @brief Gets the subscribe topics.
   *
   * @return The subscribe topics.
   */
  std::vector<std::string> sub_topic() const { return _sub_topic; }


  /**
   * @brief Returns the status of the system.
   *
   * @return A map containing the last messages for each subscribed topic.
   */
  std::map<std::string, std::string> status();


  /**
   * @brief Returns the name of the agent.
   *
   * @return The name of the agent.
   */
  std::string name();


  /**
   * @brief Returns the last received message by the agent.
   *
   * @return A tuple containing the topic and payload of the last received
   * message.
   */
  std::tuple<std::string, std::string> last_message();


  /**
   * @brief Returns the topic of the last received message by the agent.
   *
   * @return The topic of the last received message.
   */
  std::string last_topic();


  /**
   * @brief Returns the last received blob by the agent.
   *
   * @return A tuple containing the topic, format, and payload of the last
   * received blob.
   */
  std::tuple<std::string, std::string, std::vector<unsigned char>> last_blob();


  /**
   * @brief Detects if settings are local or loaded from URI.
   *
   * @return true or false.
   */
  bool settings_are_local() const;


  /**
   * @brief Detects if agent is connected.
   *
   * @return true or false.
   */
  bool is_connected();


  /**
   * @brief Returns the value of timeout in loading settings from URI.
   *
   * @return the timeout in ms (default to 0, no timeout).
   */
  int settings_timeout();


  /**
   * @brief Sets the value of timeout in loading settings from URI. Set to
   * for no timeout.
   *
   * @param to the timeout in ms.
   * @throws AgentError if already initialized
   */
  void set_settings_timeout(int to);


  /**
   * @brief Returns the value of timeout in receiving messages.
   *
   * @return the timeout in ms (default to 2000).
   */
  int receive_timeout();


  /**
   * @brief Sets the value of timeout in receiving messages. Set to 0 for no
   * timeout.
   *
   * @param to the timeout in ms.
   * @throws AgentError if already initialized
   */
  void set_receive_timeout(int to);

  /**
   * @brief Sets the value of timeout in receiving messages. Set to 0 for no
   * timeout.
   *
   * @param to the timeout.
   * @throws AgentError if already initialized
   */
  void set_receive_timeout(std::chrono::milliseconds to);


  /**
   * @brief Returns wheter a restart has been requested.
   *
   * @return the restart flag.
   */
  bool restart();


  /**
   * @brief Returns the path to the attachment file.
   *
   * This is the file that was sent by the broker when reading settings from
   * URI. It is used to load additional plugins or resources.
   *
   * @return The path to the attachment file.
   */
  std::filesystem::path attachment_path();


  /**
   * @brief Returns whether CURVE encryption is enabled.
   *
   * @return true if CURVE encryption is enabled, false otherwise.  
   */
  bool is_crypto();


  /**
   * @brief Set the use of CURVE encryption and enables authentication.
   * 
   * @param verbose 
   */
  void set_crypto(auth_verbose verbose = auth_verbose::off);


  /**
   * @brief Returns a pointer to the CurveAuth object.
   * 
   * @return unique_ptr<CurveAuth>* 
   */
  std::unique_ptr<CurveAuth> *curve_auth();


  /**
   * @brief Returns the path to the etc directory.
   * 
   * @return filesystem::path 
   */
  std::filesystem::path key_dir();


  /**
   * @brief Sets the path to the etc directory.
   * 
   * @param path 
   */
  void set_key_dir(const std::filesystem::path &path);

  std::string settings_uri();

  
  void set_conflate(bool conflate);
  bool conflate();

  /**
   * @brief Set the high watermark object
   * 
   * @param i The queue size. I set to 1 or 0, the agent will only keep the last message received, implementing a LastKnown Value (LKV) semantic. 
   */
  void set_high_watermark(int i = 1000);
  int high_watermark();


  double timecode_fps = MADS_FPS;
  Mads::auth_verbose auth_verbose = auth_verbose::off;
  std::string server_key_name = "broker";
  std::string client_key_name = "client";

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
  void connect_pub(std::chrono::milliseconds delay = std::chrono::milliseconds(0));


  /**
   * @brief Connects the agent to the subscribe endpoint and subscribes to the
   * topics.
   */
  void connect_sub();


  /**
   * @brief Internal use wrapping the receive step both for normal operations and for LastKnown Value (LKV) semantic, when high_watermark is 1.
   * 
   * @param message 
   * @param dont_block 
   * @return true new message
   * @return false no new message (when non blocking or timeout)
   */
  bool receive_raw(zmqpp::message &message, bool dont_block = false);

  static std::tuple<std::string, std::string, std::string> split_URL(const std::string &url);

  // Member variables
  std::string _hostname;
  std::string _name;
  std::string _settings_uri;
  std::string _raw_settings;
  toml::table _config;
  std::string _pub_endpoint, _sub_endpoint;
  std::string _pub_topic;
  std::string _agent_id;
  std::vector<std::string> _sub_topic;
  zmqpp::context _context;
  zmqpp::socket _publisher;
  zmqpp::socket _subscriber;
  std::map<std::string, std::string> _status;
  std::tuple<std::string, std::string> _last_message;
  std::tuple<std::string, std::string, std::vector<unsigned char>> _last_blob;
  mutable std::mutex _message_state_mutex;
  bool _cross = false;
  bool _connected = false;
  int _receive_timeout = 500;
  int _settings_timeout = 0;
  bool _init_done = false;
  bool _restart = false;
  bool _remote_controlled = false;
  std::chrono::milliseconds _time_step = std::chrono::milliseconds(0);
  double _timecode_offset = 0.0;
  std::filesystem::path _attachment_path;
  bool _crypto = false;
  bool _conflate = false;
  std::unique_ptr<CurveAuth> _curve_auth = nullptr;
  std::filesystem::path _key_dir;
  bool _last_value_only = false;
  bool _shutdown_done = false;
  SharedLatest<zmqpp::message_t> _latest_message;
  std::thread _drain_thread;
  std::thread _rc_thread;
public:
  bool dummy = false;
};

} // namespace Mads

#endif // AGENT_HPP
