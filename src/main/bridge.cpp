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

  // Settings
  Options options(argv[0]);
  options.add_options()
      ("t,topic", "Topic (default bridge)", value<string>())
      ("m,message", "Message (default empty)", value<string>())
      ("p,period", "Sampling period (default 100 ms)", value<size_t>());
  SETUP_OPTIONS(options, Bridge)

  if (options_parsed.count("t") != 0) {
    topic = options_parsed["t"].as<string>();
  }
  if (options_parsed.count("p") != 0) {
    sleep_time = chrono::milliseconds(options_parsed["p"].as<size_t>());
  }
  if(options_parsed.count("m") != 0) {
    message = options_parsed["m"].as<string>();
    single_shot = true;
  }

  // Core stuff
  Bridge bridge(argv[0], settings_uri);
  try {
    bridge.init();
  } catch (const std::exception &e) {
    std::cout << fg::red << "Error initializing agent: " << e.what()
              << fg::reset << endl;
    exit(EXIT_FAILURE);
  }
  bridge.set_pub_topic(topic);
  bridge.connect(CONNECT_DELAY);
  if (!single_shot) bridge.info();

  if (!message.empty()) {
    // wait_for_connection();
    j = json::parse(message);
    j["timestamp"]["$date"] = get_ISODate_time(chrono::system_clock::now());
    cout << "Publishing message: " << j << endl;
    bridge.publish(j);
  } else {
    bridge.register_event(event_type::startup);
    // Main loop
    cout << fg::green << "Bridge process started" << fg::reset << endl;
    bridge.loop([&]() {
      bridge.route();
    }, sleep_time);
    cout << fg::green << "Bridge process stopped" << fg::reset << endl;
    bridge.register_event(event_type::shutdown);
  }

  // Cleanup
  bridge.disconnect();

  // Done.
  return 0;
}
