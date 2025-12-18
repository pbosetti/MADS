/*
  _____                    _       _       
 |_   _|__ _ __ ___  _ __ | | __ _| |_ ___ 
   | |/ _ \ '_ ` _ \| '_ \| |/ _` | __/ _ \
   | |  __/ | | | | | |_) | | (_| | ||  __/
   |_|\___|_| |_| |_| .__/|_|\__,_|\__\___|
                    |_|                    
A template executable

Author(s): Paolo Bosetti
*/
#include "../mads.hpp"
#include "../agent.hpp"
#include <cxxopts.hpp>
#include <cppy3/cppy3.hpp>
#include <cppy3/utilits.hpp>

using namespace std;
using namespace cxxopts;
using namespace Mads;
using json = nlohmann::json;

int main(int argc, char *argv[]) {
  string settings_uri = SETTINGS_URI;
  bool crypto = false;
  filesystem::path key_dir(Mads::exec_dir() + "/../etc");
  string client_key_name = "client";
  string server_key_name = "broker";
  auth_verbose auth_verbose = auth_verbose::off;

  // CLI options
  Options options(argv[0]);
  // if needed, add here further CLI options
  // options.add_options()
  //     ("o,option", "option description", value<size_t>());
  SETUP_OPTIONS(options, Agent);

  // cryptography
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
  } catch (const std::exception &e) {
    std::cout << fg::red << "Error initializing agent: " << e.what()
              << fg::reset << endl;
    exit(EXIT_FAILURE);
  }
  agent.enable_remote_control();
  agent.connect(); 
  agent.register_event(event_type::startup);
  agent.info();

  // If needed, parse here further CLI options intended to override INI settings

  // Main loop
  cout << fg::green << "Feedback process started" << fg::reset << endl;
  agent.loop([&]() {
    // do the core stuff here
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