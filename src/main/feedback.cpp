/*
  _____             _ _                _    
 |  ___|__  ___  __| | |__   __ _  ___| | __
 | |_ / _ \/ _ \/ _` | '_ \ / _` |/ __| |/ /
 |  _|  __/  __/ (_| | |_) | (_| | (__|   < 
 |_|  \___|\___|\__,_|_.__/ \__,_|\___|_|\_\
                                            
A pure subscriber. Provides feedback to the user.

Author(s): Paolo Bosetti
*/
#include "../mads.hpp"
#include "../agent.hpp"
#include <cxxopts.hpp>
#include <filesystem>

using namespace std;
using namespace cxxopts;
using json = nlohmann::json;
using namespace Mads;

int main(int argc, char *argv[]) {
  string settings_uri = SETTINGS_URI;
  json payload, params;
  bool crypto = false;
  filesystem::path key_dir(Mads::exec_dir() + "/../etc");
  string client_key_name = "client";
  string server_key_name = "broker";
  auth_verbose auth_verbose = auth_verbose::off;
  int width = 65, indent = -1;

  // CLI options
  Options options(argv[0]);
  SETUP_OPTIONS(options, Agent);

  // Settings
  if (options_parsed.count("crypto") != 0) {
    crypto = true;
    if (options_parsed.count("keys_dir") != 0) {
      key_dir = options_parsed["keys_dir"].as<string>();
    }
    if (options_parsed.count("key_server") != 0) {
      server_key_name = options_parsed["key_server"].as<string>();
    }
    if (options_parsed.count("key_client") != 0) {
      client_key_name = options_parsed["key_client"].as<string>();
    }
    if (options_parsed.count("auth_verbose") != 0) {
      auth_verbose = auth_verbose::on;
    }
  }
  
  // Core stuff
  Agent agent(argv[0], settings_uri);
  if (crypto) {
    agent.set_key_dir(key_dir);
    agent.client_key_name = client_key_name;
    agent.server_key_name = server_key_name;
    agent.auth_verbose = auth_verbose;
  }
  try {
    agent.init(crypto);
  } catch(const std::exception& e) {
    std::cout << fg::red << "Error initializing agent: " << e.what() << fg::reset << endl;
    exit(EXIT_FAILURE);
  } catch (...) {
    std::cout << fg::red << "Error initializing agent: Unexpected" << fg::reset << endl;
    exit(EXIT_FAILURE);
  }
  agent.enable_remote_control();
  agent.connect();
  params = agent.get_settings();
  agent.register_event(event_type::startup);
  agent.info();
  try {
    width = params["print_width"].get<int>();
  } catch (...) {}
  try {
    indent = params["indent_width"].get<int>();
  } catch (...) {}
  // Main loop
  cout << fg::green << "Feedback process started" << fg::reset << endl;
  agent.loop([&]() {
    message_type type = agent.receive();
    auto msg = agent.last_message();
    // agent.remote_control();
    if (get<0>(msg) == LOGGER_STATUS_TOPIC) {
      return;
    }
    switch (type) {
    case message_type::json:
      if (width > 0) {
        cout << style::bold << agent.last_topic() << ": " << style::reset 
             << get<1>(msg).substr(0, width) << "..." << endl;
      } else {
        cout << style::bold << agent.last_topic() << ": " << style::reset 
             << json::parse(get<1>(msg)).dump(indent) << endl;
      }
      break;
    case message_type::blob:
      cout << fg::yellow << "Received BLOB message" << fg::reset << endl;
      break;
    case message_type::none:
      break;
    default:
      cout << fg::red << "Received unknown message type" << fg::reset << endl;
      break;
    }
  });
  cout << fg::green << "Feedback process stopped" << fg::reset << endl;

  // Cleanup
  agent.register_event(event_type::shutdown);
  agent.disconnect();
  if (agent.restart()) {
    auto cmd = string(MADS_PREFIX) + argv[0];
    cout << "Restarting " << cmd << "..." << endl;
    execvp(cmd.c_str(), argv);
  }
  return 0;
}