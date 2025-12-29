/*
  __  __      _            _       _        
 |  \/  | ___| |_ __ _  __| | __ _| |_ __ _ 
 | |\/| |/ _ \ __/ _` |/ _` |/ _` | __/ _` |
 | |  | |  __/ || (_| | (_| | (_| | || (_| |
 |_|  |_|\___|\__\__,_|\__,_|\__,_|\__\__,_|
                                            
The metadata process: if publishes metadata collected from the field.

Author(s): Paolo Bosetti
*/
#include "../metadata.hpp"
#include <cxxopts.hpp>
#include <chrono>

using namespace std;
using namespace cxxopts;
using json = nlohmann::json;
using namespace Mads;

int main(int argc, char *argv[]) {
  string settings_uri = SETTINGS_URI;
  size_t i = 0;
  chrono::milliseconds time{100};
  bool continuous = false;
  bool crypto = false;
  filesystem::path key_dir(Mads::exec_dir() + "/../etc");
  string client_key_name = "client";
  string server_key_name = "broker";
  auth_verbose auth_verbose = auth_verbose::off;

  // CLI options
  Options options(argv[0]);
  // clang-format off
  options.add_options()
    ("p", "Sampling period (default 100 ms)", value<size_t>())
    ("c,continuous", "Continuous sampling", value<bool>());
  // clang-format on
  SETUP_OPTIONS(options, Mads::Metadata);

  // Settings
  if (options_parsed.count("p") != 0) {
    time = chrono::milliseconds(options_parsed["p"].as<size_t>());
  }
  continuous = (options_parsed.count("c") != 0);

  if (options_parsed.count("crypto") != 0) {
    crypto = true;
    if (options_parsed.count("keys_dir") != 0) {
      key_dir = options_parsed["keys_dir"].as<string>();
    }
    if (options_parsed.count("key_broker") != 0) {
      server_key_name = options_parsed["key_broker"].as<string>();
    }
    if (options_parsed.count("key_client") != 0) {
      client_key_name = options_parsed["key_client"].as<string>();
    }
    if (options_parsed.count("auth_verbose") != 0) {
      auth_verbose = auth_verbose::on;
    }
  }

  // Core stuff
  Metadata metadata(argv[0], settings_uri);
  if (crypto) {
    metadata.set_key_dir(key_dir);
    metadata.client_key_name = client_key_name;
    metadata.server_key_name = server_key_name;
    metadata.auth_verbose = auth_verbose;
  }
  try {
    metadata.init(crypto);
  } catch (const std::exception &e) {
    std::cout << fg::red << "Error initializing agent: " << e.what()
              << fg::reset << endl;
    exit(EXIT_FAILURE);
  }
  metadata.enable_remote_control();
  metadata.connect(CONNECT_DELAY);
  metadata.register_event(event_type::startup);
  metadata.info();

  // Main loop
  cout << fg::green << "Metadata process started" << fg::reset << endl;
  metadata.loop([&]() {
    if (metadata.read_lines() || continuous) {
      json j = {{"GPIO", metadata.values()}};
      metadata.publish(j);
      cout << "Sent message " << j.dump() << endl;
    }
  }, time);
  cout << fg::green << "Metadata process stopped" << fg::reset << endl;

  // Cleanup
  metadata.register_event(event_type::shutdown);
  metadata.disconnect();
  if (metadata.restart()) {
    auto cmd = string(MADS_PREFIX) + argv[0];
    cout << "Restarting " << cmd << "..." << endl;
    execvp(cmd.c_str(), argv);
  }
  return 0;
}