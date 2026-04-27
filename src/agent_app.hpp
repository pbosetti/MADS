/*******************************************************************************
     _                    _      _
    / \   __ _  ___ _ __ | |_   / \   _ __  _ __
   / _ \ / _` |/ _ \ '_ \| __| / _ \ | '_ \| '_ \
  / ___ \ (_| |  __/ | | | |_ / ___ \| |_) | |_) |
 /_/   \_\__, |\___|_| |_|\__/_/   \_\ .__/| .__/
         |___/                       |_|   |_|

Executable-oriented helpers for MADS agents.
Author(s): Paolo Bosetti, 2026
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
#include <utility>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace Mads {

/**
 * @brief Application-facing wrapper for Agent and Agent subclasses.
 *
 * AgentAppT keeps messaging behavior in the wrapped Agent-derived class while
 * collecting repetitive command-line and startup operations used by agent
 * executables. Use AgentApp for a plain Agent and AgentAppFor<T> for an
 * existing Agent subclass such as Bridge, Dealer, Worker, Logger, or Image.
 *
 * Minimal usage:
 *
 * @code
 * int main(int argc, char *argv[]) {
 *   Mads::AgentApp agent{argv[0], SETTINGS_URI};
 *   agent.add_common_options();
 *   agent.options()
 *     ("my-option", "An additional option flag");
 *
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
 *   std::chrono::milliseconds time{100};
 *   agent.loop([&]() -> std::chrono::milliseconds {
 *     return 0ms;
 *   }, time);
 *
 *   agent.disconnect();
 *   agent.restart_if_requested(argv);
 *   return 0;
 * }
 * @endcode
 *
 * Existing Agent subclasses can be wrapped without changing the subclass:
 *
 * @code
 * Mads::AgentAppFor<Mads::Bridge> bridge{argv[0], SETTINGS_URI};
 * bridge.route();
 * @endcode
 *
 * @tparam AgentT Agent or an Agent-derived class constructible from
 *         (std::string, std::string).
 */
template<typename AgentT>
class AgentAppT : public AgentT {
  static_assert(std::is_base_of_v<Agent, AgentT>,
                "AgentT must derive from Mads::Agent");
  static_assert(std::is_constructible_v<AgentT, std::string, std::string>,
                "AgentT must be constructible from (std::string, std::string)");

public:
  using AgentT::init;

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
   * @brief Construct an AgentAppT with an owned cxxopts parser.
   *
   * @param name Agent name or executable path, following Agent constructor
   *        semantics.
   * @param settings_uri Settings path or broker URI.
   */
  AgentAppT(std::string name, std::string settings_uri)
      : AgentT(name, std::move(settings_uri)), _options(std::move(name)) {}

  /**
   * @brief Access the common option adder for fluent cxxopts declarations.
   *
   * @return A cxxopts option adder bound to the owned parser.
   */
  cxxopts::OptionAdder options() { return _options.add_options(); }

  /**
   * @brief Access the owned cxxopts parser.
   *
   * @return Mutable reference to the owned parser.
   */
  cxxopts::Options &raw_options() { return _options; }

  /**
   * @brief Access the owned cxxopts parser.
   *
   * @return Const reference to the owned parser.
   */
  const cxxopts::Options &raw_options() const { return _options; }

  /**
   * @brief Add common MADS executable options to the owned parser.
   */
  void add_common_options() {
    _options.add_options()
      // clang-format off
      ("s,settings", "Settings file path/URI",
       cxxopts::value<std::string>())
      ("S,save-settings", "Save settings to ini file",
       cxxopts::value<std::string>())
      ("settings-timeout", "Timeout in milliseconds for reading settings from broker ('0' = no timeout)",
       cxxopts::value<int>()->default_value("0"))
      ("crypto", "Enable CURVE encryption for broker communication")
      ("keys_dir", "Directory where CURVE keys are stored",
       cxxopts::value<std::string>()->implicit_value(Mads::exec_dir("../etc")))
      ("key_broker", "Name of the broker key file (without .key extension)",
       cxxopts::value<std::string>()->implicit_value("broker"))
      ("key_client", "Name of the client key file (without .key extension)",
       cxxopts::value<std::string>()->implicit_value("client"))
      ("auth_verbose", "Enable verbose authentication messages")
      ("v,version", "Print version")
      ("h,help", "Print usage");
      // clang-format on
  }

  /**
   * @brief Add optional agent identity options.
   */
  void add_agent_identity_options() {
    _options.add_options()
      // clang-format off
      ("n,name", "Agent name", cxxopts::value<std::string>())
      ("i,agent-id", "Agent ID to be added to JSON frames",
       cxxopts::value<std::string>());
      // clang-format on
  }

  /**
   * @brief Add the non-blocking receive option.
   */
  void add_dont_block_option() {
    _options.add_options()
      ("b,dont-block", "don't block on read");
  }

  /**
   * @brief Add the ZMQ socket queue size option.
   */
  void add_queue_size_option() {
    _options.add_options()
      ("q,queue-size", "ZMQ socket queue size (default 1000)",
       cxxopts::value<int>());
  }

  /**
   * @brief Parse the owned cxxopts parser.
   *
   * Invalid command lines are reported with parser help and terminate with
   * EXIT_FAILURE.
   *
   * @param argc Argument count from main().
   * @param argv Argument vector from main().
   * @return Parsed cxxopts result owned by this object.
   */
  cxxopts::ParseResult &parse_options(int argc, char *argv[]) {
    try {
      _parsed_options = _options.parse(argc, argv);
    } catch (const cxxopts::exceptions::exception &e) {
#ifndef MADS_AGENT_NO_INFO
      std::cerr << fg::red;
#endif
      std::cerr << "Error parsing command line: " << e.what();
#ifndef MADS_AGENT_NO_INFO
      std::cerr << fg::reset;
#endif
      std::cerr << std::endl << std::endl;
      std::cerr << _options.help() << std::endl;
      if (argc > 0 && argv != nullptr && argv[0] != nullptr) {
        std::cerr << "Run '" << argv[0] << " --help' for usage." << std::endl;
      }
      std::exit(EXIT_FAILURE);
    }
    return _parsed_options;
  }

  /**
   * @brief Handle standard options that terminate an executable early.
   *
   * Handles --help, --version, and --save-settings. Returns -1 when no
   * early-exit option was present; otherwise returns the process exit code that
   * should be returned by main().
   *
   * @tparam SaveAgentT Agent type used for --save-settings.
   * @param parsed cxxopts parse result.
   * @param parser cxxopts parser used to print help.
   * @param argv Argument vector from main().
   * @param default_settings_uri Settings URI to use when --settings is absent.
   * @param out Stream for normal output.
   * @param err Stream for error output.
   * @return -1 when no early exit is needed, otherwise EXIT_SUCCESS or
   *         EXIT_FAILURE.
   */
  template<typename SaveAgentT>
  static int handle_standard_exit_options(
      const cxxopts::ParseResult &parsed, const cxxopts::Options &parser,
      char *argv[], std::string default_settings_uri = SETTINGS_URI,
      std::ostream &out = std::cout, std::ostream &err = std::cerr) {
    static_assert(std::is_base_of_v<Agent, SaveAgentT>,
                  "SaveAgentT must derive from Mads::Agent");
    static_assert(
        std::is_constructible_v<SaveAgentT, std::string, std::string>,
        "SaveAgentT must be constructible from (std::string, std::string)");

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
      SaveAgentT obj(argv[0], cli_options.settings_uri);
      if (cli_options.crypto.enabled) {
        obj.set_key_dir(cli_options.crypto.key_dir);
        obj.client_key_name = cli_options.crypto.client_key_name;
        obj.server_key_name = cli_options.crypto.server_key_name;
        obj.auth_verbose = cli_options.crypto.auth_verbose;
      }
      try {
        if (cli_options.settings_timeout > 0) {
          obj.set_settings_timeout(cli_options.settings_timeout);
        }
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
   * @brief Apply parsed CLI options and initialize the wrapped agent.
   *
   * @param parsed cxxopts parse result.
   * @param default_settings_uri Settings URI to use when --settings is absent.
   * @param install_watchdog Whether Agent::init() should install the loop
   *        watchdog.
   */
  void init(const cxxopts::ParseResult &parsed,
            std::string default_settings_uri = SETTINGS_URI,
            bool install_watchdog = true) {
    auto cli_options =
        cli_options_from_parse_result(parsed, std::move(default_settings_uri));
    configure_from_cli_options(cli_options);
    if (cli_options.settings_timeout > 0) {
      print_status(std::cout, "Using settings timeout of " +
                                  std::to_string(cli_options.settings_timeout) +
                                  " ms");
      this->set_settings_timeout(cli_options.settings_timeout);
    }
    AgentT::init(cli_options.crypto.enabled, install_watchdog);
    _settings = this->get_settings();
  }

  /**
   * @brief Enable automatic startup and shutdown event registration.
   */
  void enable_events(bool enabled = true) { _events_enabled = enabled; }

  /**
   * @brief Connect the wrapped agent and optionally register startup.
   */
  void connect(std::chrono::milliseconds delay = std::chrono::milliseconds(250)) {
    AgentT::connect(delay);
    if (_events_enabled) {
      this->register_event(event_type::startup);
    }
  }

  /**
   * @brief Optionally register shutdown and disconnect the wrapped agent.
   */
  void disconnect() {
    if (_events_enabled && this->is_connected()) {
      this->register_event(event_type::shutdown);
    }
    AgentT::disconnect();
  }

  /**
   * @brief Return cached settings loaded during init().
   */
  const nlohmann::json &settings_json() const { return _settings; }

  /**
   * @brief Apply receive_timeout from cached settings when present.
   */
  void apply_receive_timeout() {
    if (!_settings.contains("receive_timeout") ||
        _settings.at("receive_timeout").is_null()) {
      return;
    }
    const auto &receive_timeout = _settings.at("receive_timeout");
    if (receive_timeout.is_number_integer()) {
      this->set_receive_timeout(receive_timeout.template get<int>());
    }
  }

  /**
   * @brief Apply queue size from CLI or cached settings when present.
   */
  void apply_queue_size() {
    if (_parsed_options.count("queue-size") != 0) {
      const int queue_size = _parsed_options["queue-size"].template as<int>();
      this->set_high_watermark(queue_size);
      return;
    }
    if (!_settings.contains("queue_size") ||
        _settings.at("queue_size").is_null()) {
      return;
    }
    this->set_high_watermark(_settings.value("queue_size", 1000));
  }

  /**
   * @brief Restart the current executable if requested by remote control.
   *
   * @param argv Original argv from main().
   * @param out Stream for restart status output.
   * @return true when a restart was requested and exec was attempted.
   */
  bool restart_if_requested(char *argv[], std::ostream &out = std::cout) {
    if (!this->restart()) {
      return false;
    }
    auto cmd = std::string(MADS_PREFIX) + argv[0];
    out << "Restarting " << cmd << "..." << std::endl;
#if defined(_WIN32)
    _execvp(cmd.c_str(), argv);
#else
    execvp(cmd.c_str(), argv);
#endif
    return true;
  }

private:
  template<typename T>
  static std::optional<T> option_value(const cxxopts::ParseResult &parsed,
                                       const std::string &name) {
    if (parsed.count(name) == 0) {
      return std::nullopt;
    }
    return parsed[name].as<T>();
  }

  static void print_status(std::ostream &out, const std::string &message) {
#ifndef MADS_AGENT_NO_INFO
    out << fg::yellow;
#endif
    out << message;
#ifndef MADS_AGENT_NO_INFO
    out << fg::reset;
#endif
    out << std::endl;
  }

  static CliOptions cli_options_from_parse_result(
      const cxxopts::ParseResult &parsed,
      std::string default_settings_uri = SETTINGS_URI) {
    CliOptions options;
    options.settings_uri = std::move(default_settings_uri);
    options.settings_timeout =
        option_value<int>(parsed, "settings-timeout").value_or(0);

    if (auto settings = option_value<std::string>(parsed, "settings")) {
      options.settings_uri = *settings;
    }
    if (auto name = option_value<std::string>(parsed, "name")) {
      options.agent_name = *name;
    }
    if (auto agent_id = option_value<std::string>(parsed, "agent-id")) {
      options.agent_id = *agent_id;
    }

    if (parsed.count("crypto") != 0) {
      options.crypto.enabled = true;
      if (auto key_dir = option_value<std::string>(parsed, "keys_dir")) {
        options.crypto.key_dir = *key_dir;
      }
      if (auto server_key_name =
              option_value<std::string>(parsed, "key_broker")) {
        options.crypto.server_key_name = *server_key_name;
      }
      if (auto client_key_name =
              option_value<std::string>(parsed, "key_client")) {
        options.crypto.client_key_name = *client_key_name;
      }
      if (parsed.count("auth_verbose") != 0) {
        options.crypto.auth_verbose = Mads::auth_verbose::on;
      }
    }

    return options;
  }

  void configure_from_cli_options(const CliOptions &options) {
    this->_settings_uri = options.settings_uri;

    if (options.agent_name) {
      const auto &name = *options.agent_name;
      const size_t pos = name.rfind('-');
      if (pos != std::string::npos) {
        this->_name = name.substr(pos + 1);
      } else {
        this->_name = name;
      }
    }

    if (options.agent_id) {
      this->set_agent_id(*options.agent_id);
    }

    if (options.crypto.enabled) {
      this->set_key_dir(options.crypto.key_dir);
      this->client_key_name = options.crypto.client_key_name;
      this->server_key_name = options.crypto.server_key_name;
      this->auth_verbose = options.crypto.auth_verbose;
    }
  }

  bool _events_enabled = false;
  nlohmann::json _settings;
  cxxopts::Options _options;
  cxxopts::ParseResult _parsed_options;
};

using AgentApp = AgentAppT<Agent>;

template<typename AgentT>
using AgentAppFor = AgentAppT<AgentT>;

} // namespace Mads

#endif // AGENT_APP_HPP
