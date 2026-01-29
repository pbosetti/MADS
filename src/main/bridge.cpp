/*
  ____       _     _
 | __ ) _ __(_) __| | __ _  ___
 |  _ \| '__| |/ _` |/ _` |/ _ \
 | |_) | |  | | (_| | (_| |  __/
 |____/|_|  |_|\__,_|\__, |\___|
                     |___/
Bridge agent: it is designed to read JSON messages coming from an external
executable via an input pipe and route them to the broker.
Run this like:
  ./my_script | build/broker

Author: Paolo Bosetti
*/
#include "../bridge.hpp"
#include <cxxopts.hpp>

using namespace std;
using namespace cxxopts;
using json = nlohmann::json;
using namespace Mads;

int main(int argc, char const *argv[]) {
  string settings_uri = SETTINGS_URI;
  string topic = "bridge";
  string message = "";
  json j{};
  chrono::milliseconds sleep_time{100};
  bool single_shot = false;
  bool crypto = false;
  filesystem::path key_dir(Mads::exec_dir() + "/../etc");
  string client_key_name = "client";
  string server_key_name = "broker";
  auth_verbose auth_verbose = auth_verbose::off;

  // Settings
  Options options(argv[0]);
  options.add_options()("t,topic", "Topic (default bridge)", value<string>())(
      "m,message", "Message (default empty)", value<string>())(
      "p,period", "Sampling period (default 100 ms)", value<size_t>());
  SETUP_OPTIONS(options, Bridge)

  if (options_parsed.count("t") != 0) {
    topic = options_parsed["t"].as<string>();
  }
  if (options_parsed.count("p") != 0) {
    sleep_time = chrono::milliseconds(options_parsed["p"].as<size_t>());
  }
  if (options_parsed.count("m") != 0) {
    message = options_parsed["m"].as<string>();
    single_shot = true;
  }

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
  Bridge bridge(argv[0], settings_uri);
  if (crypto) {
    bridge.set_key_dir(key_dir);
    bridge.client_key_name = client_key_name;
    bridge.server_key_name = server_key_name;
    bridge.auth_verbose = auth_verbose;
  }
  try {
    bridge.init(crypto);
  } catch (const std::exception &e) {
    std::cout << fg::red << "Error initializing agent: " << e.what()
              << fg::reset << endl;
    exit(EXIT_FAILURE);
  }
  bridge.set_pub_topic(topic);
  bridge.connect(CONNECT_DELAY);
  if (!single_shot)
    bridge.info();

  if (!message.empty()) {
    // wait_for_connection();
    j = json::parse(message);
    j["timestamp"]["$date"] = get_ISODate_time(chrono::system_clock::now());
    cout << "Publishing message: " << j << endl;
    bridge.publish(j);
  } else {
    bridge.register_event(event_type::startup);
    // Main loop
    cout << fg::green << "Bridge process started, send 'exit' to stop"
         << fg::reset << endl;
    bridge.loop([&]() -> chrono::milliseconds { 
      bridge.route(); 
      return 0ms; }, sleep_time);
    cout << fg::green << "Bridge process stopped" << fg::reset << endl;
    bridge.register_event(event_type::shutdown);
  }

  // Cleanup
  bridge.disconnect();

  // Done.
  return 0;
}
