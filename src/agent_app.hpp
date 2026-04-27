/*******************************************************************************
     _                    _      _                
    / \   __ _  ___ _ __ | |_   / \   _ __  _ __  
   / _ \ / _` |/ _ \ '_ \| __| / _ \ | '_ \| '_ \ 
  / ___ \ (_| |  __/ | | | |_ / ___ \| |_) | |_) |
 /_/   \_\__, |\___|_| |_|\__/_/   \_\ .__/| .__/ 
         |___/                       |_|   |_|    

Executable-oriented helpers for MADS agents.
Author(s): Paolo Bosetti, 2026
This header defines the AgentApp class, a subclass of Agent that provides common setup for agent executables, including command-line option parsing and handling.
*******************************************************************************/

#ifndef AGENT_APP_HPP
#define AGENT_APP_HPP

#include "agent.hpp"
#include "exec_path.hpp"
#include "mads.hpp"
#include <cxxopts.hpp>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <type_traits>

namespace Mads {

/**
 * @brief Application-facing Agent subclass with common executable setup.
 *
 * AgentApp keeps the messaging behavior in Agent unchanged while collecting
 * repetitive command-line and startup operations used by agent executables.
 * It owns a cxxopts::Options object, offers helpers for the common MADS
 * options, and applies parsed CLI values to the underlying Agent.
 *
 * Minimal usage:
 *
 * @code
 * int main(int argc, char *argv[]) {
 *   Mads::AgentApp agent{argv[0], SETTINGS_URI};
 *   agent.add_common_options();
 *   agent.options()
 *   // clang-format off
 *     ("my-option", "An additional option flag");
 *.  // clang-format on

 *   auto parsed = agent.parse_options(argc, argv);
 *   if (int rc = Mads::AgentApp::handle_standard_exit_options<Mads::AgentApp>(
 *           parsed, agent.raw_options(), argv);
 *       rc >= 0) {
 *     return rc;
 *   }
 *
 *   agent.init(parsed);
 *   agent.enable_events();
 *   agent.connect();
 *   agent.info();
 * 
 *   chrono::milliseconds time{100};
 *   agent.loop([&]() -> chrono::milliseconds {
 *     // Loop body here
 *     return 0ms; // or return time for next loop
 *   }, time);
 * 
 *   agent.disconnect();
 *   agent.restart_if_requested(argv);
 *   return 0;
 * }
 * @endcode
 *
 * The class is intentionally not used by existing executables until they are
 * migrated explicitly.
 */
class AgentApp : public Agent {
public:
  using Agent::init;

  /**
   * @brief CURVE encryption options parsed from the command line.
   */
  struct CryptoOptions {
    /** @brief Whether CURVE encryption is enabled. */
    bool enabled = false;

    /** @brief Directory containing CURVE key files. */
    std::filesystem::path key_dir = Mads::exec_dir("../etc");

    /** @brief Client key file name without extension. */
    std::string client_key_name = "client";

    /** @brief Broker/server public key file name without extension. */
    std::string server_key_name = "broker";

    /** @brief Authentication logging verbosity. */
    Mads::auth_verbose auth_verbose = Mads::auth_verbose::off;
  };

  /**
   * @brief Common options parsed for agent executables.
   */
  struct CliOptions {
    /** @brief Settings path or broker URI. */
    std::string settings_uri = SETTINGS_URI;

    /** @brief Optional agent name override. */
    std::optional<std::string> agent_name;

    /** @brief Optional agent identifier added to outgoing JSON frames. */
    std::optional<std::string> agent_id;

    /** @brief CURVE encryption configuration. */
    CryptoOptions crypto;

    /** @brief Timeout in milliseconds for reading settings from broker. */
    int settings_timeout = 0;
  };

  /**
   * @brief Construct an AgentApp with an owned cxxopts parser.
   *
   * @param name Agent name or executable path, following Agent constructor
   *        semantics.
   * @param settings_uri Settings path or broker URI.
   */
  AgentApp(std::string name, std::string settings_uri);

  /**
   * @brief Access the common option adder for fluent cxxopts declarations.
   *
   * This returns the result of cxxopts::Options::add_options(), enabling the
   * standard cxxopts operator() syntax:
   *
   * @code
   * AgentApp agent{name, uri};
   * agent.options()("my-option", "An additional option flag");
   * @endcode
   *
   * @return A cxxopts option adder bound to the owned parser.
   */
  cxxopts::OptionAdder options();

  /**
   * @brief Access the owned cxxopts parser.
   *
   * Use this when lower-level cxxopts APIs are needed, such as
   * parse_positional(), positional_help(), or help().
   *
   * @return Mutable reference to the owned parser.
   */
  cxxopts::Options &raw_options();

  /**
   * @brief Access the owned cxxopts parser.
   *
   * @return Const reference to the owned parser.
   */
  const cxxopts::Options &raw_options() const;

  /**
   * @brief Add common MADS executable options to the owned parser.
   *
   * Adds settings, save-settings, CURVE, version, and help options using the
   * same option names currently used by agent executables.
   */
  void add_common_options();

  /**
   * @brief Add optional agent identity options.
   *
   * Adds --name and --agent-id to the owned parser. Executables should call
   * this only when those options are meaningful.
   */
  void add_agent_identity_options();

  /**
   * @brief Add the non-blocking receive option.
   *
   * Adds --dont-block / -b to the owned parser. Executables should call this
   * only when they support non-blocking receive loops.
   */
  void add_dont_block_option();

  /**
  * @brief Add the ZMQ socket queue size option.
  *
  * Adds --queue-size / -q to the owned parser. Executables should call this
  * only when they support configuring the ZMQ socket queue size.
  */
  void add_queue_size_option();

  /**
   * @brief Parse the owned cxxopts parser.
   *
   * @param argc Argument count from main().
   * @param argv Argument vector from main().
   * @return Parsed cxxopts result.
   */
  cxxopts::ParseResult &parse_options(int argc, char *argv[]);

  /**
   * @brief Handle standard options that terminate an executable early.
   *
   * Handles --help, --version, and --save-settings. This method returns -1 when
   * no early-exit option was present; otherwise it returns the process exit
   * code that should be returned by main().
   *
   * @tparam AgentT Agent type used for --save-settings. It must be
   *         constructible as AgentT(std::string, std::string) and derive from
   *         Agent.
   * @param parsed cxxopts parse result.
   * @param parser cxxopts parser used to print help.
   * @param argv Argument vector from main().
   * @param default_settings_uri Settings URI to use when --settings is absent.
   * @param out Stream for normal output.
   * @param err Stream for error output.
   * @return -1 when no early exit is needed, otherwise EXIT_SUCCESS or
   *         EXIT_FAILURE.
   */
  template<typename AgentT>
  static int handle_standard_exit_options(
      const cxxopts::ParseResult &parsed, const cxxopts::Options &parser,
      char *argv[], std::string default_settings_uri = SETTINGS_URI,
      std::ostream &out = std::cout, std::ostream &err = std::cerr) {
    static_assert(std::is_base_of_v<Agent, AgentT>,
                  "AgentT must derive from Mads::Agent");

    if (parsed.count("help")) {
      out << argv[0] << " ver. " << LIB_VERSION << std::endl;
      out << parser.help() << std::endl;
      return EXIT_SUCCESS;
    }

    if (parsed.count("version")) {
      out << LIB_VERSION << std::endl;
      return EXIT_SUCCESS;
    }

    if (parsed.count("save-settings")) {
      auto cli_options =
          cli_options_from_parse_result(parsed, std::move(default_settings_uri));
      const auto output_path = parsed["save-settings"].as<std::string>();
      AgentT obj(argv[0], cli_options.settings_uri);
      if (cli_options.crypto.enabled) {
        obj.set_key_dir(cli_options.crypto.key_dir);
        obj.client_key_name = cli_options.crypto.client_key_name;
        obj.server_key_name = cli_options.crypto.server_key_name;
        obj.auth_verbose = cli_options.crypto.auth_verbose;
      }
      try {
        obj.init(cli_options.crypto.enabled);
        obj.save_settings(output_path);
      } catch (const AgentError &e) {
#ifndef MADS_AGENT_NO_INFO
        err << fg::red;
#endif
        err << "Error saving local settings: " << e.what();
#ifndef MADS_AGENT_NO_INFO
        err << fg::reset;
#endif
        err << std::endl;
        return EXIT_FAILURE;
      } catch (const std::exception &e) {
#ifndef MADS_AGENT_NO_INFO
        err << fg::red;
#endif
        err << "Error saving settings: " << e.what();
#ifndef MADS_AGENT_NO_INFO
        err << fg::reset;
#endif
        err << std::endl;
        return EXIT_FAILURE;
      }
#ifndef MADS_AGENT_NO_INFO
      out << fg::magenta;
#endif
      out << "Settings saved to " << output_path;
#ifndef MADS_AGENT_NO_INFO
      out << fg::reset;
#endif
      out << std::endl;
      return EXIT_SUCCESS;
    }

    return -1;
  }

  /**
   * @brief Apply parsed CLI options and initialize the agent.
   *
   * Parses common AgentApp options from a cxxopts parse result, applies them,
   * and calls Agent::init().
   *
   * @param parsed cxxopts parse result.
   * @param default_settings_uri Settings URI to use when --settings is absent.
   * @param install_watchdog Whether Agent::init() should install the loop
   *        watchdog.
   * @throws AgentError or std::exception on initialization errors.
   */
  void init(const cxxopts::ParseResult &parsed,
            std::string default_settings_uri = SETTINGS_URI,
            bool install_watchdog = true);

  /**
   * @brief Enable automatic startup and shutdown event registration.
   *
   * When enabled, AgentApp::connect() calls register_event(startup) after a
   * successful connection, and AgentApp::disconnect() calls
   * register_event(shutdown) before disconnecting sockets.
   *
   * Event registration is disabled by default to preserve existing executable
   * behavior until each executable explicitly opts in.
   *
   * @param enabled Whether automatic event registration should be enabled.
   */
  void enable_events(bool enabled = true);

  /**
   * @brief Connect and optionally register a startup event.
   *
   * This method intentionally hides Agent::connect() for AgentApp call sites.
   *
   * @param delay Delay after connecting, matching Agent::connect().
   */
  void connect(std::chrono::milliseconds delay = std::chrono::milliseconds(250));

  /**
   * @brief Optionally register a shutdown event and disconnect.
   *
   * This method intentionally hides Agent::disconnect() for AgentApp call
   * sites.
   */
  void disconnect();

  /**
   * @brief Return this agent's settings section as JSON.
   *
   * @return Cached settings JSON for the current agent.
   */
  const nlohmann::json &settings_json() const;

  /**
   * @brief Apply receive_timeout from cached settings when present.
   */
  void apply_receive_timeout();

  /**
   * @brief Apply queue_size from cached settings when present.
   *
   *
   */
  void apply_queue_size();

  /**
   * @brief Restart the current executable if requested by remote control.
   *
   * @param argv Original argv from main().
   * @param out Stream for restart status output.
   * @return true when a restart was requested and exec was attempted.
   */
  bool restart_if_requested(char *argv[], std::ostream &out = std::cout);

private:
  static CliOptions cli_options_from_parse_result(
      const cxxopts::ParseResult &parsed,
      std::string default_settings_uri = SETTINGS_URI);

  void configure_from_cli_options(const CliOptions &options);

  bool _events_enabled = false;
  nlohmann::json _settings;
  cxxopts::Options _options;
  cxxopts::ParseResult _parsed_options;
};

} // namespace Mads

#endif // AGENT_APP_HPP
