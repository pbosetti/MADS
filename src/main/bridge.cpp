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
#include "../agent_app.hpp"
#include "../bridge.hpp"

using namespace std;
using json = nlohmann::json;
using namespace Mads;

int main(int argc, char *argv[]) {
  string topic = "bridge";
  string message = "";
  json j{};
  chrono::milliseconds sleep_time{100};
  bool single_shot = false;

  // Settings
  AgentAppFor<Bridge> bridge(argv[0], SETTINGS_URI);
  bridge.options()
    ("t,topic", "Topic (default bridge)", cxxopts::value<string>())
    ("m,message", "Message (default empty)", cxxopts::value<string>())
    ("p,period", "Sampling period (default 100 ms)", cxxopts::value<size_t>());
  bridge.add_queue_size_option();
  bridge.add_common_options();

  auto options_parsed = bridge.parse_options(argc, argv);
  if (int rc = AgentApp::handle_standard_exit_options<AgentAppFor<Bridge>>(
          options_parsed, bridge.raw_options(), argv);
      rc >= 0) {
    return rc;
  }

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

  // Core stuff
  try {
    bridge.init(options_parsed);
  } catch (const std::exception &e) {
    std::cout << fg::red << "Error initializing agent: " << e.what()
              << fg::reset << endl;
    exit(EXIT_FAILURE);
  }
  bridge.set_pub_topic(topic);
  // HWM or CONFLATE options:
  json settings = bridge.settings_json();
  bridge.apply_queue_size();
  if (!single_shot) {
    bridge.enable_events();
  }
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
    // Main loop
    cout << fg::green << "Bridge process started, send 'exit' to stop"
         << fg::reset << endl;
    bridge.loop([&]() -> chrono::milliseconds {
      bridge.route();
      return 0ms;
    }, sleep_time);
    cout << fg::green << "Bridge process stopped" << fg::reset << endl;
  }

  // Cleanup
  bridge.disconnect();

  // Done.
  return 0;
}
