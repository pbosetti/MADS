#include "agent_app.hpp"

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace std;

namespace {

bool has_option(const cxxopts::ParseResult &parsed, const string &name) {
  return parsed.count(name) != 0;
}

template<typename T>
optional<T> option_value(const cxxopts::ParseResult &parsed,
                         const string &name) {
  if (!has_option(parsed, name)) {
    return nullopt;
  }
  return parsed[name].as<T>();
}

void print_warning(ostream &out, const string &message) {
#ifndef MADS_AGENT_NO_INFO
  out << fg::yellow;
#endif
  out << message;
#ifndef MADS_AGENT_NO_INFO
  out << fg::reset;
#endif
  out << endl;
}

void print_status(ostream &out, const string &message) {
#ifndef MADS_AGENT_NO_INFO
  out << fg::yellow;
#endif
  out << message;
#ifndef MADS_AGENT_NO_INFO
  out << fg::reset;
#endif
  out << endl;
}

} // namespace

namespace Mads {

AgentApp::AgentApp(string name, string settings_uri)
    : Agent(name, std::move(settings_uri)), _options(std::move(name)) {
}

cxxopts::OptionAdder AgentApp::options() {
  return _options.add_options();
}

cxxopts::Options &AgentApp::raw_options() {
  return _options;
}

const cxxopts::Options &AgentApp::raw_options() const {
  return _options;
}

void AgentApp::add_common_options() {
  _options.add_options()
    ("s,settings", "Settings file path/URI",
     cxxopts::value<string>())
    ("S,save-settings", "Save settings to ini file",
     cxxopts::value<string>())
    ("crypto", "Enable CURVE encryption for broker communication")
    ("keys_dir", "Directory where CURVE keys are stored",
     cxxopts::value<string>()->implicit_value(Mads::exec_dir("../etc")))
    ("key_broker", "Name of the broker key file (without .key extension)",
     cxxopts::value<string>()->implicit_value("broker"))
    ("key_client", "Name of the client key file (without .key extension)",
     cxxopts::value<string>()->implicit_value("client"))
    ("auth_verbose", "Enable verbose authentication messages")
    ("v,version", "Print version")
    ("h,help", "Print usage");
}

void AgentApp::add_agent_identity_options() {
  _options.add_options()
    ("n,name", "Agent name", cxxopts::value<string>())
    ("i,agent-id", "Agent ID to be added to JSON frames",
     cxxopts::value<string>());
}

void AgentApp::add_dont_block_option() {
  _options.add_options()
    ("b,dont-block", "don't block on read");
}

cxxopts::ParseResult AgentApp::parse_options(int argc, char *argv[]) {
  try {
    return _options.parse(argc, argv);
  } catch (const cxxopts::exceptions::exception &e) {
#ifndef MADS_AGENT_NO_INFO
    cerr << fg::red;
#endif
    cerr << "Error parsing command line: " << e.what();
#ifndef MADS_AGENT_NO_INFO
    cerr << fg::reset;
#endif
    cerr << endl << endl;
    cerr << _options.help() << endl;
    if (argc > 0 && argv != nullptr && argv[0] != nullptr) {
      cerr << "Run '" << argv[0] << " --help' for usage." << endl;
    }
    exit(EXIT_FAILURE);
  }
}

AgentApp::CliOptions AgentApp::cli_options_from_parse_result(
    const cxxopts::ParseResult &parsed, string default_settings_uri) {
  CliOptions options;
  options.settings_uri = std::move(default_settings_uri);

  if (auto settings = option_value<string>(parsed, "settings")) {
    options.settings_uri = *settings;
  }
  if (auto name = option_value<string>(parsed, "name")) {
    options.agent_name = *name;
  }
  if (auto agent_id = option_value<string>(parsed, "agent-id")) {
    options.agent_id = *agent_id;
  }

  if (has_option(parsed, "crypto")) {
    options.crypto.enabled = true;
    if (auto key_dir = option_value<string>(parsed, "keys_dir")) {
      options.crypto.key_dir = *key_dir;
    }
    if (auto server_key_name = option_value<string>(parsed, "key_broker")) {
      options.crypto.server_key_name = *server_key_name;
    }
    if (auto client_key_name = option_value<string>(parsed, "key_client")) {
      options.crypto.client_key_name = *client_key_name;
    }
    if (has_option(parsed, "auth_verbose")) {
      options.crypto.auth_verbose = Mads::auth_verbose::on;
    }
  }

  return options;
}

void AgentApp::configure_from_cli_options(const CliOptions &options) {
  _settings_uri = options.settings_uri;

  if (options.agent_name) {
    const auto &name = *options.agent_name;
    const size_t pos = name.rfind('-');
    if (pos != string::npos) {
      _name = name.substr(pos + 1);
    } else {
      _name = name;
    }
  }

  if (options.agent_id) {
    set_agent_id(*options.agent_id);
  }

  if (options.crypto.enabled) {
    set_key_dir(options.crypto.key_dir);
    client_key_name = options.crypto.client_key_name;
    server_key_name = options.crypto.server_key_name;
    auth_verbose = options.crypto.auth_verbose;
  }
}

void AgentApp::init_from_cli_options(const CliOptions &options,
                                     bool install_watchdog) {
  configure_from_cli_options(options);
  init(options.crypto.enabled, install_watchdog);
}

void AgentApp::init(const cxxopts::ParseResult &parsed,
                    string default_settings_uri, bool install_watchdog) {
  init_from_cli_options(
      cli_options_from_parse_result(parsed, std::move(default_settings_uri)),
      install_watchdog);
}

void AgentApp::enable_events(bool enabled) {
  _events_enabled = enabled;
}

void AgentApp::connect(chrono::milliseconds delay) {
  Agent::connect(delay);
  if (_events_enabled) {
    register_event(event_type::startup);
  }
}

void AgentApp::disconnect() {
  if (_events_enabled && is_connected()) {
    register_event(event_type::shutdown);
  }
  Agent::disconnect();
}

nlohmann::json AgentApp::settings_json() {
  return get_settings();
}

void AgentApp::apply_receive_timeout_from_settings(
    const nlohmann::json &settings) {
  if (!settings.contains("receive_timeout")) {
    return;
  }
  const auto &receive_timeout = settings.at("receive_timeout");
  if (receive_timeout.is_number_integer()) {
    set_receive_timeout(receive_timeout.get<int>());
  }
}

void AgentApp::apply_high_watermark_from_settings(
    const nlohmann::json &settings, ostream &out) {
  if (!settings.contains("high_watermark") ||
      settings.at("high_watermark").is_null()) {
    return;
  }
  set_high_watermark(settings.value("high_watermark", 1000));
  print_warning(out,
                "Warning: high_watermark setting is deprecated, use queue_size");
}

bool AgentApp::resolve_dont_block(const cxxopts::ParseResult &parsed,
                                  const nlohmann::json &settings,
                                  ostream &out) {
  bool dont_block = false;
  if (settings.contains("dont-block") && !settings.at("dont-block").is_null()) {
    dont_block = settings.value("dont-block", false);
    print_warning(out,
                  "Warning: dont-block setting is deprecated, use dont_block");
  }
  dont_block = settings.value("dont_block", dont_block);
  if (has_option(parsed, "dont-block")) {
    dont_block = true;
  }
  if (dont_block) {
    print_status(out, "Running in non-blocking mode");
  }
  return dont_block;
}

bool AgentApp::restart_if_requested(char *argv[], ostream &out) {
  if (!restart()) {
    return false;
  }
  auto cmd = string(MADS_PREFIX) + argv[0];
  out << "Restarting " << cmd << "..." << endl;
#if defined(_WIN32)
  _execvp(cmd.c_str(), argv);
#else
  execvp(cmd.c_str(), argv);
#endif
  return true;
}

} // namespace Mads
