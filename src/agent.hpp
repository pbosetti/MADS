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
#include <zmq.hpp>
#include <zmq_addon.hpp>
#include <mutex>
#include <condition_variable>
#include <array>
#include <optional>
#include <span>
#include <atomic>
#include <memory>
#include "clock_offset.hpp"
#include "curve.hpp"
#include "exec_path.hpp"
#include "mads_shm.hpp"
#include "socket_monitor.hpp"
#include "topic_match.hpp"

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 255
#endif

#ifndef MADS_AGENT_NO_INFO
#include <rang.hpp>
using namespace rang;
#endif

namespace Mads {

namespace shm {
class SharedRing;
class SegmentCache;
} // namespace shm

template<typename T> struct SharedLatest {
  std::mutex mtx;
  std::condition_variable cv;
  std::optional<T> value;
};

/**
 * @brief A received message payload that lazily holds either its JSON text or
 * its parsed nlohmann::json form, caching whichever is produced on first use.
 *
 * receive() stores whichever representation it already has cheaply — the decoded
 * object for MsgPack frames, the raw text for JSON frames — and the other form
 * is produced only if a consumer asks for it. This avoids the
 * decode -> dump -> re-parse round-trip on the hot receive path.
 *
 * Conversions mutate cached state, so callers must serialise access (the Agent
 * does so under its message-state mutex).
 */
class LazyPayload {
public:
  LazyPayload() = default;
  static LazyPayload from_text(std::string text) {
    LazyPayload p;
    p._text = std::move(text);
    return p;
  }
  static LazyPayload from_doc(nlohmann::json doc) {
    LazyPayload p;
    p._doc = std::move(doc);
    return p;
  }
  /// JSON text form (dumps the cached object once if only the object exists).
  const std::string &text() const {
    if (!_text)
      _text = _doc ? _doc->dump() : std::string();
    return *_text;
  }
  /// Parsed object form (parses the cached text once if only text exists).
  const nlohmann::json &doc() const {
    if (!_doc) {
      if (_text && !_text->empty())
        _doc = nlohmann::json::parse(*_text);
      else
        _doc = nlohmann::json();
    }
    return *_doc;
  }

private:
  mutable std::optional<std::string> _text;
  mutable std::optional<nlohmann::json> _doc;
};

/**
 * @brief One responder's answer to Agent::broadcast_clock_probe()'s ping
 * (clock-offset source B, see clock_offset.hpp): the raw four-timestamp
 * sample plus the responder's identity and its own currently adopted clock
 * offset, which is what lets an initiator compose
 * `clock_offset(mine) = responder_adopted.offset_us + estimate(sample).offset_us`
 * rather than treating a peer exchange as an unrelated measurement.
 */
struct ClockPeerObservation {
  std::string responder_agent_id;
  std::string responder_name;
  std::string responder_hostname;
  std::string responder_domain;
  ClockSample sample;
  /// The responder's own clock_offset() at reply time; source() ==
  /// ClockSource::None if the responder had no valid measurement of its own.
  ClockOffsetResult responder_adopted;
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
   * @brief Apply CURVE client credentials to a socket if crypto is enabled.
   *
   * Factors out the (previously duplicated) curve-client setup used by every
   * REQ socket talking to the broker.
   *
   * @param socket The socket to configure.
   */
  void setup_curve_on(zmq::socket_t &socket);

  /**
   * @brief What query_broker() brings back from the broker.
   */
  struct BrokerReply {
    /// Raw TOML settings text.
    std::string settings;
    /// Attachment bytes (empty if the broker served none).
    std::string attachment;
    /// Broker timecode minus local timecode, in seconds.
    double timecode_offset = 0.0;
    /// The one-frame reply to the `transport` command (JSON text), or empty
    /// when the broker predates the command or did not answer it.
    std::string transport;
  };

  /**
   * @brief Read settings, timecode and transport info from the broker over a
   * single REQ socket.
   *
   * Performs the `settings`, `timecode` and `transport` round-trips on one
   * connection, avoiding separate socket open/connect/close cycles.
   *
   * The attachment is returned as raw bytes rather than a path: where it gets
   * cached depends on the `attachment_ext` setting, which is only known once
   * the settings this same call fetched have been parsed. fetch_settings()
   * persists it (see detail/plugin_cache.hpp).
   *
   * @param uri The URI of the broker.
   * @param name The name of the agent.
   * @param timeout The timeout in milliseconds.
   * @return the settings, attachment, timecode offset and transport reply.
   * @throws AgentError if timed out or the broker refuses to provide settings.
   *         A missing or unanswered `transport` reply is not an error.
   */
  BrokerReply query_broker(std::string uri, std::string name,
                           int timeout = DEFAULT_SETTINGS_TIMEOUT_MS);

  /**
   * @brief Resolves the transport mode and sets _pub_endpoint/_sub_endpoint
   * accordingly: the broker's ipc:// endpoints when they can be used, the
   * [agents] tcp addresses (pointed at the settings host) otherwise. Called
   * by init().
   *
   * @throws AgentError if `transport = "ipc"` is requested (or settings were
   *         fetched over ipc) but ipc cannot be used.
   */
  void _select_transport();

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

  /**
   * @brief Acquires settings (and any broker-served attachment) without
   * binding the agent to a settings section.
   *
   * This performs the settings acquisition portion of init() only: CURVE
   * setup, resolving the local-file/broker-URI settings source, and (if
   * settings come from a broker) saving any served attachment and renaming
   * it per the section's `attachment_ext`. It does not validate or bind the
   * `_name` section, install the loop watchdog, or call load_settings().
   *
   * Idempotent: a second call (including the one made internally by init(),
   * if not already fetched) is a no-op. Useful for callers that need to
   * inspect settings or a served attachment (e.g. to select a plugin file)
   * before committing to init()'s section binding.
   *
   * @param crypto Whether to use CURVE encryption (default false).
   * @throws AgentError if timed out in reading settings from broker.
   */
  void fetch_settings(bool crypto = false);

  /**
   * @brief Adds a default section for this agent if the fetched settings have
   * none.
   *
   * Call it between fetch_settings() and init() to let an agent run with
   * settings that do not mention it: the added section is the one the
   * zero-config `"none"` settings URI builds (`pub_topic` = agent name,
   * `sub_topic = [""]`, i.e. subscribe to everything), while the rest of the
   * fetched settings (e.g. `[agents]` endpoints) still applies. Used by
   * read-only observers such as mads-top and mads-echo.
   *
   * @return true if the section was missing and has been added, false if the
   * settings already had one (left untouched).
   * @throws AgentError if settings have not been fetched yet.
   */
  bool add_default_section();

  // Destructor
  virtual ~Agent();

  /**
   * @brief Install a watch thread to ensure exit from loops
   * 
   * This starts a low-frequency thread that forces an exit when the agent
   * keeps looping for more than 3 seconds after it was asked to stop (its
   * Runtime was stopped or shutdown/disconnect was requested)
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
   * Optionally, this function accepts a delay parameter, which bounds how
   * long connecting may take before publishing is safe. This is needed to
   * ensure that the agent is connected before sending messages. The default
   * delay is 250 milliseconds.
   *
   * `delay` is an upper bound, not a fixed cost: the publisher waits for its
   * ZMTP handshake and then for SUBSCRIPTION_SETTLE_DELAY of slow-joiner
   * grace, and returns as soon as both are done. Pass 0 only when the agent
   * does not publish immediately -- a first message sent with no grace at all
   * is dropped by the PUB socket, unsent and unreported.
   *
   * @param type The type of connection to be established.
   * @param delay Upper bound, in milliseconds, on the connect-and-settle wait.
   * @throws AgentError if not initialized
   */
  void connect(std::chrono::milliseconds delay = std::chrono::milliseconds(250));

  /**
   * @brief Blocks until the publisher socket's connection is confirmed by a
   * real ZMQ_EVENT_CONNECTED/ZMQ_EVENT_HANDSHAKE_SUCCEEDED event, or
   * `timeout` elapses (ZMQ_DEVELOPMENT.md §2.1).
   *
   * @warning This is a transport-liveness signal, not a licence to publish.
   * ZMQ_EVENT_CONNECTED fires at the TCP level, before the broker's XSUB
   * frontend has forwarded the fleet's subscriptions back to this publisher,
   * and a PUB socket silently discards anything sent while no subscription
   * matches it. connect() therefore does *not* use this: it waits on the ZMTP
   * handshake and then adds SUBSCRIPTION_SETTLE_DELAY of slow-joiner grace.
   * Callers waiting by hand before a one-shot publish need that same grace.
   *
   * @param timeout upper bound on how long to wait.
   * @return true if the connection was observed within the timeout.
   */
  bool wait_for_connection(std::chrono::milliseconds timeout);

  /**
   * @brief The current state of this agent's link to the broker: up or down,
   * why (`last_handshake` -- e.g. a ZMQ_EVENT_HANDSHAKE_FAILED_AUTH turns a
   * CURVE rejection into a fact instead of a bare receive timeout), since
   * when, and how often it has dropped and recovered.
   *
   * Reported from the subscriber socket, since that is the one over which an
   * agent would otherwise silently stop hearing from a departed broker;
   * publish-only agents (no `sub_topic`, so no subscriber connection) fall
   * back to the publisher's. Both connect to the same broker, so in practice
   * they rise and fall together.
   *
   * A `set_cross()` agent binds rather than connects, and gets one
   * ZMQ_EVENT_DISCONNECTED per departing peer with no matching handshake
   * event on arrival -- events that would add up to "down" as soon as any one
   * peer left. There is no broker link to describe in that topology, so this
   * reports Mads::LinkStatus::Unknown throughout.
   */
  Mads::LinkState link_state() const;

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

  /**
   * @brief Receives a message without any JSON/blob (de)serialization (P3).
   *
   * Exposes the wire frame exactly as received: the topic (part 0) and every
   * subsequent part verbatim as bytes (embedded NUL bytes included), with no
   * frame-header parsing, no decompression, and no JSON decode -- whatever
   * was published (JSON text, a MsgPack frame, or a blob's meta+bytes parts)
   * comes back unchanged. Intended for mads-record, so a bag file gets
   * exactly what was on the wire without round-tripping through
   * nlohmann::json or re-copying blob bytes.
   *
   * Goes through the same receive_raw() path as receive() (wildcard
   * sub_topic filtering (P2) and Last-Known-Value delivery both still
   * apply); this is purely a difference in how the received frame is
   * exposed to the caller, not in what gets delivered.
   *
   * One exception to "verbatim": a shared-memory reference (whose bytes live
   * in the producer's ring, and mean nothing once recorded) is materialized
   * into an ordinary MsgPack blob frame [header][meta][bytes]. If the
   * reference cannot be resolved, or the slot is overwritten while copying,
   * the message is dropped (counted in shm_failed()/shm_torn()) and this
   * returns false.
   *
   * @param topic Out param: the topic frame.
   * @param parts Out param: every frame after the topic, in wire order.
   *   Cleared and repopulated on a successful receive; left untouched if
   *   nothing was received.
   * @param dont_block Whether to return immediately if no message is pending.
   * @return true if a message was received, false if nothing arrived
   *   (dont_block, or the receive timeout elapsed).
   * @throws AgentError if not initialized, or while threaded remote control
   *   owns the subscriber socket.
   */
  bool receive_raw_message(std::string &topic, std::vector<std::string> &parts,
                           bool dont_block = false);

  /**
   * @brief Publishes a raw multi-part message with no JSON encoding, no
   * automatic field-stamping (agent_id/hostname/timestamp/...), no
   * compression, and no frame header (P3).
   *
   * Sends `topic` followed by `parts`, byte for byte, exactly as given.
   * Intended for mads-play, to republish a recorded frame unchanged --
   * pairs with receive_raw_message() and the parts BagReader hands back, so
   * a JSON message or a blob's meta+bytes parts are never re-parsed,
   * re-dumped, or re-copied through the JSON path.
   *
   * @param topic The topic frame.
   * @param parts The frames to send after the topic, in order.
   * @throws AgentError if not initialized.
   */
  void publish_raw_message(const std::string &topic,
                           const std::vector<std::string> &parts);

  /*
   ____  _                        _
  / ___|| |__   __ _ _ __ ___  __| |  _ __ ___   ___ _ __ ___
  \___ \| '_ \ / _` | '__/ _ \/ _` | | '_ ` _ \ / _ \ '_ ` _ \
   ___) | | | | (_| | | |  __/ (_| | | | | | | |  __/ | | | | |
  |____/|_| |_|\__,_|_|  \___|\__,_| |_| |_| |_|\___|_| |_| |_|

  Shared-memory data plane (see mads_shm.hpp). A producer agent with shared
  output enabled writes blobs into a ring of slots in a shared-memory segment
  and publishes only a small descriptor; consumers on the same host read the
  bytes in place (accept_shared_views(true)) or get an ordinary blob copy.
  */

  /**
   * @brief Enables shared-memory output: creates a ring of `slots` slots of
   * `slot_size` bytes each. From then on, blob publishes that fit a slot are
   * sent as shared-memory references.
   *
   * Called by init() when the agent's section (or [agents]) has
   * `shm = true`, with `shm_slots` and `shm_slot_size`.
   *
   * Only takes effect when the broker runs on this host: transport() is
   * "ipc"; or the broker's `transport` reply carries this host's token; or,
   * with no such reply, the tcp broker address is loopback or this host's
   * name. Otherwise it prints a warning and blobs stay inline. A ring that
   * cannot be created (OS error) is also reported with a warning. Check
   * shared_output_enabled() for the outcome. Calling it again replaces the
   * ring (consumers still holding views of the old one keep them).
   *
   * @param slots Number of ring slots (>= 2).
   * @param slot_size Bytes per slot (> 0).
   * @throws AgentError if not initialized, or on invalid geometry.
   */
  void enable_shared_output(uint32_t slots, size_t slot_size);

  /**
   * @brief Whether blob publishes go through the shared-memory ring.
   */
  bool shared_output_enabled() const { return _shm_ring != nullptr; }

  /**
   * @brief Capacity in bytes of each shared-memory slot, or 0 if shared
   * output is disabled.
   */
  size_t shared_slot_size() const;

  /**
   * @brief Takes the next ring slot for writing (zero-copy publishing).
   *
   * Write at most `slot.capacity` bytes at `slot.data`, then hand the slot to
   * publish_slot(), or give it back with abandon_slot() (nothing written)
   * or discard_slot(). Slots are taken in round-robin order: a slot is
   * overwritten after `slots` further
   * acquisitions, so publish it before acquiring that many more. Blob
   * publishing with shared output enabled is meant for one thread.
   *
   * @throws AgentError if shared output is not enabled.
   */
  WritableSlot acquire_slot();

  /**
   * @brief Publishes the first `size` bytes written into `slot` as a
   * shared-memory reference: [topic][header(blob|shm)][meta + "shm"
   * descriptor][empty] (see detail/wire_format.hpp).
   *
   * `meta` is stamped like a blob's (timestamp, timecode, agent_id,
   * hostname). The slot is consumed.
   *
   * @throws AgentError if not initialized, if shared output is not enabled,
   *   if `size` exceeds the slot capacity (the slot is then discarded), or if
   *   `slot` is not being written (already published or abandoned, or
   *   overwritten by a wrap).
   */
  void publish_slot(WritableSlot &slot, size_t size,
                    nlohmann::json meta = nlohmann::json{{"format", "raw"}},
                    std::string topic = "");

  /**
   * @brief Gives back a slot taken with acquire_slot() into which nothing
   * was written; the next acquire_slot() reuses it. The payload previously
   * published in the slot becomes valid again (see SharedRing::abandon()).
   * If anything may have been written, use discard_slot() instead. No-op if
   * shared output is disabled or the slot was already consumed.
   */
  void abandon_slot(WritableSlot &slot);

  /**
   * @brief Gives back a slot taken with acquire_slot() that may have been
   * written but is not to be published: views of its previous contents stay
   * invalid. No-op if shared output is disabled or the slot was already
   * consumed.
   */
  void discard_slot(WritableSlot &slot);

  /**
   * @brief Opts this consumer into zero-copy shared-memory views (default
   * false).
   *
   * When true, receive() returns message_type::shared for a shared-memory
   * reference and the payload is available as last_shared(). When false,
   * the payload is copied and receive() returns message_type::blob exactly
   * as for an inline blob.
   */
  void accept_shared_views(bool accept) { _accept_shared_views = accept; }

  /**
   * @brief Whether receive() hands out shared-memory views.
   */
  bool accepts_shared_views() const { return _accept_shared_views; }

  /**
   * @brief The last shared-memory view received (receive() returned
   * message_type::shared). Its meta() is the producer's blob meta.
   *
   * The referenced object is overwritten by the next receive() of a shared
   * message; copy the SharedView (cheap, and the copy keeps the mapping
   * alive) to keep it. As for any view, check valid() after reading the
   * bytes.
   */
  SharedView const &last_shared() const { return _last_shared; }

  /**
   * @brief Topic of the message last_shared() came with.
   */
  std::string last_shared_topic() const;

  /**
   * @brief Shared-memory payloads dropped because the producer overwrote
   * the slot while it was being read (torn reads): copies made by a
   * non-opted-in receive() or by receive_raw_message(), and views reported
   * with record_torn_view().
   */
  size_t shm_torn() const { return _shm_torn.load(); }

  /**
   * @brief Shared-memory references that could not be resolved (foreign
   * host, producer gone, slot already overwritten, bad descriptor);
   * receive() returns message_type::none for them.
   */
  size_t shm_failed() const { return _shm_failed.load(); }

  /**
   * @brief Counts (and logs, rate-limited) a torn read noticed by the caller:
   * a view from last_shared() that was no longer valid() after use.
   */
  void record_torn_view();

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
   * The loop period is expressed in nanoseconds internally, so that any
   * std::chrono::duration (milliseconds, microseconds, ...) can be passed or
   * returned by the lambda: e.g. `agent.loop(cb, 500us);` opts into
   * microsecond-resolution pacing, while existing millisecond-based code
   * (`agent.loop(cb, 100ms);`, `return 0ms;`) keeps compiling and behaving as
   * before (the conversion to nanoseconds is implicit and lossless).
   *
   * By default, the wait between iterations uses this_thread::sleep_for,
   * whose actual wake-up jitter is limited by the OS scheduler (typically
   * within a fraction of a millisecond). For tighter, microsecond-accurate
   * pacing, opt into enable_high_res_loop(), which busy-spins the tail of
   * each interval instead of sleeping through it, trading CPU time for
   * precision: the fraction of each interval spent spinning (and so the
   * extra CPU load) scales with spin_margin relative to the loop period, so
   * it is most noticeable on short periods and/or a large spin_margin.
   *
   * @param lambda the function to be executed in the main loop; it can return
   *  the requested duration of the next loop: if this is 0, the default loop
   *  time is used, otherwise the value returned by the lambda
   * @param duration the duration of the loop (default 0, max speed)
   * @throws AgentError if not initialized
   */
  using loop_fun_t = std::function<std::chrono::nanoseconds()>;
  void loop(loop_fun_t const &lambda,
            std::chrono::nanoseconds duration);


  /**
   * @brief Enters the main loop of the agent. It also sets a signal handler for
   * SIGNINT, which will set the running flag to false.
   * The loop duration is set to the time step of the agent, as pased in the
   * ini file with the time_step parameter (or the finer time_step_us). It it
   * is zero or not set, it will run at max speed.
   *
   * @param lambda the function to be executed in the main loop; it can return
   *  the requested duration of the next loop: if this is 0, the default loop
   *  time is used, otherwise the value returned by the lambda
   * @throws AgentError if not initialized
   */
  void loop(loop_fun_t const &lambda);


  /**
   * @brief Opt into (or out of) high-resolution loop pacing.
   *
   * When enabled, the wait between loop iterations sleeps for most of the
   * remaining interval and then busy-spins on a steady_clock check for the
   * last `spin_margin`, trading CPU usage for microsecond-accurate wake-up
   * timing: the extra CPU cost is roughly proportional to spin_margin
   * divided by the loop period (e.g. a 200us margin on a 500us period keeps
   * a core busy on the order of ~20-40% of the time; a 200us margin on a
   * 100ms period costs a negligible fraction). Leave disabled (the default)
   * for millisecond-granular loops, where plain sleep_for is sufficient and
   * cheaper.
   *
   * @param on whether to enable high-resolution pacing (default true).
   * @param spin_margin the portion of each interval to busy-spin instead of
   *  sleep (default 200us); only relevant when `on` is true.
   */
  void enable_high_res_loop(bool on = true,
                            std::chrono::nanoseconds spin_margin =
                                std::chrono::microseconds(200));

  /**
   * @brief Returns whether high-resolution loop pacing is enabled.
   */
  bool high_res_loop() const;


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
   * @brief Set the subscribe endpoint URL
   * 
   * @param endpoint in the form tcp://hostname:port
   */
  void set_sub_endpoint(std::string endpoint) { _sub_endpoint = endpoint; }

  /**
   * @brief Get the subscribe endpoint URL
   * 
   * @return The subscribe endpoint URL
   */
  std::string sub_endpoint() const { return _sub_endpoint; }

  /**
   * @brief Set the publish endpoint URL
   * 
   * @param endpoint in the form tcp://hostname:port
   */
  void set_pub_endpoint(std::string endpoint) { _pub_endpoint = endpoint; }

  /**
   * @brief Get the publish endpoint URL
   * 
   * @return The publish endpoint URL
   */
  std::string pub_endpoint() const { return _pub_endpoint; }

  /**
   * @brief Force the transport to the broker, overriding the `transport`
   * setting of the agent's section and of [agents].
   *
   * - "auto" (the default): ipc:// when the broker runs on this host and
   *   offers it, tcp:// otherwise;
   * - "tcp": always tcp://;
   * - "ipc": ipc:// or init() fails.
   *
   * Must be called before init(). An empty string clears the override.
   *
   * @param mode "auto", "tcp", "ipc" or "".
   * @throws AgentError on any other value, or if already initialized.
   */
  void set_transport_mode(std::string const &mode);

  /**
   * @brief The resolved transport mode: "auto", "tcp" or "ipc".
   *
   * Before init() this is the override set with set_transport_mode(), or
   * "auto".
   */
  std::string transport_mode() const;

  /**
   * @brief The transport actually used for the pub/sub endpoints after
   * init(): "ipc" or "tcp".
   */
  std::string transport() const { return _transport; }

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
   * @brief Returns the last received message as a parsed JSON object.
   *
   * Fast path for consumers that want a `nlohmann::json` rather than its text:
   * for MsgPack frames this returns the already-decoded object (no re-dump and
   * no re-parse); for JSON frames it parses once and caches. Avoids the
   * round-trip that last_message() + json::parse() would otherwise incur.
   *
   * @return A tuple of {topic, parsed payload}. The payload is a copy.
   */
  std::tuple<std::string, nlohmann::json> last_json();


  /**
   * @brief Returns the topic of the last message receive() returned, whatever
   * its type: JSON (including "control" messages handled by remote control),
   * blob or shared-memory view.
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
   * @brief Zero-copy view of the last received blob.
   *
   * Returns non-owning views (topic, format, bytes) into the agent's internally
   * stored blob, avoiding the full copy performed by last_blob(). The returned
   * views remain valid only until the next call to receive() and must not be
   * used concurrently with a threaded receive/drain thread.
   *
   * @return A tuple of {topic, format, bytes} as views.
   */
  std::tuple<std::string_view, std::string_view,
             std::span<const unsigned char>>
  last_blob_view() const;


  /**
   * @brief Number of messages dropped because they were malformed or could
   * not be decoded (bad part count, failed decompression, etc.).
   *
   * @return The cumulative count since startup.
   */
  size_t dropped_messages() const;


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
   * @brief Sets the value of timeout in loading settings from URI. Set to
   * for no timeout.
   *
   * @param to the timeout in milliseconds.
   * @throws AgentError if already initialized
   */
  void set_settings_timeout(std::chrono::milliseconds to);


  /**
   * @brief Returns the value of timeout in receiving messages.
   *
   * @return the timeout in ms (default DEFAULT_RECEIVE_TIMEOUT_MS = 500).
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
   * @brief The Runtime that owns this agent's run state.
   *
   * loop(), the delivery drain thread, and the threaded remote control all
   * keep going while runtime()->running() is true and the agent has not been
   * individually stopped (shutdown()/disconnect()). Stopping the Runtime
   * stops every agent that shares it.
   *
   * @return The agent's Runtime (never null).
   */
  std::shared_ptr<Mads::Runtime> runtime() const { return _runtime; }

  /**
   * @brief True while this agent's loops should keep going.
   *
   * Combines the Runtime state (group and process level) with the per-agent
   * stop request raised by shutdown()/disconnect(). This is the condition
   * loop() checks between iterations, exposed for application main loops.
   *
   * @return Whether the agent should keep running.
   */
  bool running() const { return keep_running(); }

  /**
   * @brief Attach the agent to a different Runtime.
   *
   * Each agent owns its own Runtime by default; attach several agents to a
   * shared Runtime (e.g. another agent's runtime()) to stop them together
   * without affecting the rest of the process.
   *
   * @param runtime The Runtime to attach to.
   * @throws AgentError if runtime is null or the agent is connected.
   */
  void set_runtime(std::shared_ptr<Mads::Runtime> runtime);


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
  void setup_crypto(Mads::auth_verbose verbose = auth_verbose::off);


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

  
  [[deprecated("conflate is disabled; use set_delivery(Delivery::LastKnownValue)")]]
  void set_conflate(bool conflate);
  [[deprecated("conflate is disabled; use delivery()")]]
  bool conflate();

  /**
   * @brief Set the high watermark (ZMQ receive queue bound).
   *
   * @param i The queue size. A value of 1 selects Last-Known-Value (LKV)
   *   delivery for backward compatibility (equivalent to
   *   set_delivery(Delivery::LastKnownValue)); prefer set_delivery() for
   *   clarity. A value of 0 means "unlimited" (ZMQ semantics) and Queued
   *   delivery.
   */
  void set_high_watermark(int i = 1000);
  int high_watermark();

  /**
   * @brief Select subscriber delivery semantics.
   *
   * @param d Delivery::Queued keeps the receive queue; Delivery::LastKnownValue
   *   keeps only the latest message, draining the rest on a background thread.
   * @throws AgentError if already connected.
   */
  void set_delivery(Delivery d);

  /**
   * @brief Current delivery semantics.
   */
  Delivery delivery() const;

  /**
   * @brief Select the on-the-wire payload encoding used when publishing.
   *
   * Default is WireFormat::Json (legacy, header-less, fully backward
   * compatible). WireFormat::MsgPack emits a self-describing frame header and
   * MessagePack-encoded payloads. Receivers accept both transparently.
   *
   * @param fmt The wire format to use for outgoing messages.
   */
  void set_wire_format(WireFormat fmt);

  /**
   * @brief The wire format used for outgoing messages.
   */
  WireFormat wire_format() const;

  /**
   * @brief Select the payload compression policy for outgoing messages.
   *
   * Default is Compression::Auto (compress only frames >=
   * COMPRESSION_AUTO_THRESHOLD bytes). Compression::Snappy reproduces the
   * historical always-compress behaviour; Compression::None disables it.
   *
   * @param c The compression policy to use.
   */
  void set_compression(Compression c);

  /**
   * @brief The compression policy used for outgoing messages.
   */
  Compression compression() const;

  /**
   * @brief Install SIGINT/SIGTERM handlers that request a clean shutdown.
   *
   * Idempotent and process-global: only the first call installs handlers, so
   * calling loop() repeatedly (or running several agents in one process) does
   * not clobber handler state.
   */
  static void install_signal_handlers();


  /*
    ____ _            _      ___   __  __          _
   / ___| | ___   ___| | __ / _ \ / _|/ _|___  ___| |_
  | |   | |/ _ \ / __| |/ /| | | | |_| |_/ __|/ _ \ __|
  | |___| | (_) | (__|   < | |_| |  _|  _\__ \  __/ |_
   \____|_|\___/ \___|_|\_\ \___/|_| |_| |___/\___|\__|

  */

  /**
   * @brief Runs `samples` four-timestamp exchanges against the broker's
   * settings endpoint and adopts the result as this agent's own clock-offset
   * measurement (clock-offset source A -- clock_offset.hpp §"Source A").
   * Never throws: an unreachable broker, or one too old to know the "clock"
   * command, leaves any previous measurement in place and returns
   * {valid=false}.
   *
   * Called automatically at the end of init() when [agents] clock_source is
   * "broker" (the default) and settings come from a broker; called again
   * periodically by the background clock thread when clock_interval_ms > 0
   * and this agent is (or would become) its clock domain's adopted source.
   * Safe to call directly at any other time too.
   *
   * @param samples number of round-trips; the min-delay one is kept.
   * @param timeout_ms per-request send/receive timeout.
   */
  Mads::ClockOffsetResult measure_clock_offset(size_t samples = 5,
                                               int timeout_ms = 1000);

  /**
   * @brief Broadcasts a clock-sync ping on CLOCKSYNC_TOPIC and collects
   * every responder's pong that arrives within `window` (clock-offset
   * source B -- clock_offset.hpp §"Source B"). Used internally when
   * [agents] clock_source is "peer", and by `mads top --probe` to build its
   * fleet table.
   *
   * Blocking: drives this agent's own receive() calls for the duration of
   * `window`, so nothing else on the calling thread progresses meanwhile,
   * and it is not safe to call concurrently with another in-flight
   * broadcast_clock_probe() on the same Agent (single-flight; the
   * background clock thread already respects this by construction).
   * Returns an empty vector immediately if this agent has no publisher
   * connected (no pub_topic) -- it cannot ping without one.
   *
   * @param window how long to wait for responders.
   */
  std::vector<Mads::ClockPeerObservation> broadcast_clock_probe(
      std::chrono::milliseconds window = std::chrono::milliseconds(300));

  /**
   * @brief This agent's currently adopted clock offset: the clock domain's
   * consensus winner (Mads::ClockConsensus, possibly another agent's
   * measurement) if one is known and fresh, else this agent's own last
   * measurement, else {valid=false}. Add offset_us to a wall-clock reading
   * taken on this host to obtain broker-host time.
   */
  Mads::ClockOffsetResult clock_offset() const;

  /**
   * @brief This process's clock-domain identity
   * (Mads::detail::clock_domain_id(): boot_id on Linux, hostname
   * elsewhere). Agents that share this value share a clock and are
   * expected to converge on one identical clock_offset().
   */
  std::string clock_domain() const;

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
   * @brief True while this agent's loops should keep going.
   *
   * Combines the Runtime state (group + process level) with the per-agent
   * stop request raised by shutdown()/disconnect().
   */
  bool keep_running() const {
    return _runtime->running() && !_stopping.load();
  }

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
  bool receive_raw(zmq::multipart_t &message, bool dont_block = false);

  /**
   * @brief MQTT-style wildcard filter (P2): true if `topic` is accepted by
   * at least one entry of _sub_topic -- literal entries are matched exactly
   * as the raw ZMQ SUBSCRIBE prefix already does today, wildcard entries
   * (containing '+'/'#') via Mads::topic_match(). Only ever consulted when
   * _wildcard_sub_topic is non-empty; see connect_sub()/receive_raw().
   *
   * @param topic The concrete topic of an already-received message.
   * @return true if the message should be delivered to receive()/callbacks.
   */
  bool _topic_matches_subscription(const std::string &topic) const;

  /**
   * @brief Splits a tcp://host:port URL into {"tcp://", host, port}.
   *
   * Kept for subclasses only; MADS itself uses the scheme-aware endpoint
   * parser, since ipc:// endpoints have no host or port.
   *
   * @throws std::invalid_argument if `url` is not a tcp://host:port URL.
   */
  static std::tuple<std::string, std::string, std::string> split_URL(const std::string &url);

  /**
   * @brief Resolves the plain libzmq transport-tuning knobs (ZMQ_DEVELOPMENT.md
   * §1.4: TCP keepalive, SNDBUF/RCVBUF, ...) from settings and applies them to
   * both sockets. Called once from init(), after settings are parsed and
   * before connect(). A key left unset in both [agents] and the agent's own
   * section makes no setsockopt() call at all, so an unedited mads.ini is
   * unaffected.
   */
  void _apply_socket_options();

  /**
   * @brief Brings up _io_thread if it is not already running. Called from
   * whichever of connect_pub()/connect_sub() runs first, since a
   * publish-only agent never reaches the latter and a subscribe-only agent
   * never reaches the former.
   */
  void _start_io_thread();

  /**
   * @brief Parses the [agents]/[<name>] clock_* settings during init() and
   * appends CLOCKSYNC_TOPIC to _sub_topic when this agent needs to
   * participate (mirrors how enable_remote_control() appends "control").
   */
  void _configure_clock_sync();

  /// True when this agent needs to hear/answer CLOCKSYNC_TOPIC (source
  /// != none, or the responder is enabled -- true by default). Deliberately
  /// not reflected in _sub_topic/sub_topic() -- see _configure_clock_sync().
  bool _clock_wants_sync() const;

  /**
   * @brief Brings up _clock_thread if it is not already running: announces
   * this agent's adopted clock offset on CLOCKSYNC_TOPIC every
   * clock_announce_ms, and -- when clock_interval_ms > 0 and this agent is
   * its clock domain's current winner (or no winner is known yet) --
   * re-measures. A "peer"-source agent takes its very first measurement
   * here too, since broadcast_clock_probe() needs a connected socket that
   * does not exist yet during init(). Called from connect(); joined in
   * disconnect()/shutdown() exactly like _io_thread.
   */
  void _start_clock_thread();

  /// One clock_source == "peer" measurement attempt: probes, picks an
  /// anchor per the chain-safety rules (bounded hops, non-stale, a real
  /// source), composes clock_offset(mine) = anchor.offset_us + theta, and
  /// records/announces the result. A no-op if no eligible peer responds.
  void _run_peer_measurement();

  /// Handles one decoded CLOCKSYNC_TOPIC message (ping/pong/announce).
  /// Called from receive()'s two data-frame branches and from the
  /// _io_thread control-analogous branch; never throws, never surfaces the
  /// message to the caller.
  void _handle_clocksync_message(const nlohmann::json &msg);

  /// Publishes `r` as a CLOCKSYNC_TOPIC announcement and records it into
  /// this agent's own local consensus view.
  void _announce_clock_offset(const Mads::ClockOffsetResult &r);

  /// Fills in the Agent-level bookkeeping estimate()/ClockOffsetEstimator
  /// do not know about: source, hops, this agent's identity, and the next
  /// measurement sequence number.
  Mads::ClockOffsetResult
  _stamp_clock_result(Mads::ClockOffsetResult r, Mads::ClockSource source,
                      uint8_t hops);

  /// Stable per-agent identity for clock-sync bookkeeping (ping/pong/
  /// announce "who"), independent of the optional, user-facing
  /// --agent-id/_agent_id used for payload stamping: falls back to
  /// "<name>@<pid>" so consensus works even when --agent-id was never set.
  std::string _clock_agent_identity() const;

  /// Causes of shared-memory receive problems, each logged rate-limited.
  enum class ShmIssue : size_t {
    ForeignHost,
    Unavailable,
    Overwritten,
    Torn,
    Count
  };

  /// Decodes the meta part of a shared-memory reference frame and resolves
  /// its descriptor into a view whose meta() is the user meta (without the
  /// "shm" key). On failure counts it, logs it and returns nullopt.
  std::optional<SharedView> _resolve_shm(const std::string &meta_part,
                                         uint8_t format, uint8_t compression);

  /// True if `view` is still valid after its bytes were copied; otherwise
  /// counts and logs a torn read.
  bool _shm_copy_valid(const SharedView &view);

  /// Prints `msg` on the 1st, 10th, 100th, ... occurrence of `cause`.
  void _shm_warn(ShmIssue cause, const std::string &msg);

  /// Adds timestamp/timecode/agent_id/hostname (and the clock-correction
  /// fields) to a blob's meta, unless already present.
  void _stamp_blob_meta(nlohmann::json &meta);

  // Member variables
  std::string _hostname;
  std::string _name;
  std::string _settings_uri;
  std::string _raw_settings;
  toml::table _config;
  std::string _pub_endpoint, _sub_endpoint;
  // The tcp endpoints init() would have used, kept so set_cross() (tcp-only)
  // can fall back to them when ipc was selected.
  std::string _tcp_pub_endpoint, _tcp_sub_endpoint;
  // Transport (see set_transport_mode()): the CLI/API override, the mode
  // resolved by init(), the transport in use, and the broker's raw
  // `transport` reply (empty if none).
  std::string _transport_mode_override;
  std::string _transport_mode = "auto";
  // Kernel buffers for ipc:// connections (ipc_sndbuf/ipc_rcvbuf; <= 0 keeps
  // the OS default), resolved by _apply_socket_options().
  int _ipc_sndbuf = 0;
  int _ipc_rcvbuf = 0;
  std::string _transport = "tcp";
  std::string _transport_reply;
  std::string _pub_topic;
  std::string _agent_id;
  std::vector<std::string> _sub_topic;
  // Subset of _sub_topic containing a '+'/'#' wildcard token (P2), computed
  // once by connect_sub(). Empty for every agent using only literal
  // sub_topic entries, which keeps the receive-time filter a single cheap
  // emptiness check in that -- the common -- case, adding no overhead.
  std::vector<std::string> _wildcard_sub_topic;
  zmq::context_t _context;
  zmq::socket_t _publisher;
  zmq::socket_t _subscriber;
  // Attached before each socket's connect()/bind() (ZMQ_DEVELOPMENT.md
  // §2.1); stopped in shutdown() before the sockets are closed.
  Mads::SocketMonitor _pub_monitor;
  Mads::SocketMonitor _sub_monitor;
  // A single LazyPayload per message is shared between _last_message and
  // _status so the lazy text/object caches are shared and never duplicated.
  std::map<std::string, std::shared_ptr<LazyPayload>> _status;
  std::tuple<std::string, std::shared_ptr<LazyPayload>> _last_message;
  std::tuple<std::string, std::string, std::vector<unsigned char>> _last_blob;
  std::string _last_topic; // of any message receive() returned
  mutable std::mutex _message_state_mutex;
  bool _cross = false;
  bool _connected = false;
  int _receive_timeout = DEFAULT_RECEIVE_TIMEOUT_MS;
  int _settings_timeout = 0;
  bool _init_done = false;
  bool _settings_fetched = false;
  bool _restart = false;
  std::shared_ptr<Mads::Runtime> _runtime = std::make_shared<Mads::Runtime>();
  // Per-agent stop request: set by shutdown()/disconnect(), cleared by
  // connect(). Keeps an individual agent's teardown from stopping the other
  // agents that share its Runtime.
  std::atomic<bool> _stopping{false};
  bool _remote_controlled = false;
  std::chrono::nanoseconds _time_step = std::chrono::nanoseconds(0);
  bool _high_res_loop = false;
  std::chrono::nanoseconds _spin_margin = std::chrono::microseconds(200);
  double _timecode_offset = 0.0;
  std::filesystem::path _attachment_path;
  bool _crypto = false;
  bool _conflate = false;
  std::unique_ptr<CurveAuth> _curve_auth = nullptr;
  std::filesystem::path _key_dir;
  bool _last_value_only = false;
  bool _shutdown_done = false;
  SharedLatest<zmq::multipart_t> _latest_message;
  // The agent's one socket thread (ZMQ_DEVELOPMENT.md §4.1). It always polls
  // both socket monitors' PAIR sockets -- which used to cost a thread each --
  // and additionally owns _subscriber exclusively whenever LKV delivery
  // and/or threaded remote control need it consumed off the application
  // thread. A single thread rather than one per feature: two threads calling
  // recv() on the same (non-thread-safe) ZMQ socket is undefined behaviour,
  // and with LKV and threaded remote control both enabled it also meant a
  // message landed on whichever thread's recv() call won the race.
  std::thread _io_thread;
  // Whether _io_thread polls _subscriber at all. Set by connect_sub() only
  // once the socket is fully subscribed, and read by _io_thread, so it must
  // be atomic even though it never changes after that.
  std::atomic<bool> _io_reads_subscriber{false};
  std::thread _watchdog_thread;
  // Delayed startup-event publisher: owned (not detached) so shutdown() can
  // wake it via _event_cv and join it before the sockets close.
  std::thread _startup_event_thread;
  std::mutex _event_mtx;
  std::condition_variable _event_cv;
  // ZMQ sockets are not thread-safe; the event thread publishes concurrently
  // with the owner thread, so sends on _publisher are serialized.
  std::mutex _publish_mutex;
  std::atomic<bool> _watchdog_stop{false};
  bool _rc_owns_socket = false;
  WireFormat _wire_format = WireFormat::Json;
  Compression _compression = Compression::Auto;
  std::atomic<size_t> _dropped_messages{0};
  nlohmann::json _settings_json; // cached JSON projection of settings

  // ---- Clock offset (see clock_offset.hpp) --------------------------
  Mads::ClockSource _clock_source = Mads::ClockSource::Broker;
  bool _clock_sync_responder = true;
  int _clock_interval_ms = 0;
  int _clock_announce_ms = 5000;
  bool _clock_correction = false;
  // Per-clock-domain adoption (§2 of the design): every agent, regardless
  // of its own clock_source, records what it hears so clock_offset() can
  // return the domain's winner rather than only this agent's own
  // measurement.
  Mads::ClockConsensus _clock_consensus{std::chrono::seconds(30)};
  // This agent's own last measurement (source Broker or Peer), guarded
  // separately from _clock_consensus's own internal mutex since it is
  // read/written by measure_clock_offset()/_run_peer_measurement() (the
  // clock thread or init()) and read by clock_offset() (any thread).
  mutable std::mutex _clock_mtx;
  Mads::ClockOffsetResult _own_clock_measurement;
  uint64_t _clock_seq = 0;
  std::thread _clock_thread;
  // Pending broadcast_clock_probe() collection state: the send instant (for
  // local_elapsed_us) and the pongs gathered so far. Single-flight by
  // design (see broadcast_clock_probe()'s doc comment).
  mutable std::mutex _clocksync_mtx;
  std::chrono::steady_clock::time_point _clocksync_probe_sent_at{};
  std::vector<Mads::ClockPeerObservation> _clocksync_pongs;
  uint64_t _clocksync_probe_seq = 0;
  // Per-initiator rate limit for this agent's own ping responses (§4.1 of
  // the design): guarded by _clocksync_mtx too.
  std::map<std::string, std::chrono::steady_clock::time_point>
      _clocksync_last_reply;

  // ---- Shared-memory data plane (see mads_shm.hpp) -------------------
  // Producer ring (null = blobs inline) and consumer segment cache, held by
  // shared_ptr so this header needs only forward declarations.
  std::shared_ptr<Mads::shm::SharedRing> _shm_ring;
  std::mutex _shm_ring_mutex; // serializes acquire/commit/abandon/discard
  std::shared_ptr<Mads::shm::SegmentCache> _shm_cache;
  bool _accept_shared_views = false;
  // Guarded by _message_state_mutex, like _last_blob.
  SharedView _last_shared;
  std::string _last_shared_topic;
  std::atomic<size_t> _shm_torn{0};
  std::atomic<size_t> _shm_failed{0};
  std::atomic<bool> _shm_oversize_warned{false};
  std::array<std::atomic<size_t>, static_cast<size_t>(ShmIssue::Count)>
      _shm_issue_count{};

public:
  bool dummy = false;
};

} // namespace Mads

#endif // AGENT_HPP
