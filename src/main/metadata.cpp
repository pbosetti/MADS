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

  // CLI options
  Options options(argv[0]);
  options.add_options()
    ("p", "Sampling period (default 100 ms)", value<size_t>())
    ("c,continuous", "Continuous sampling", value<bool>());
  SETUP_OPTIONS(options, Mads::Metadata);

  // Settings
  if (options_parsed.count("p") != 0) {
    time = chrono::milliseconds(options_parsed["p"].as<size_t>());
  }
  continuous = (options_parsed.count("c") != 0);

  // Core stuff
  Metadata metadata(argv[0], settings_uri);
  try {
    metadata.init();
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
    cout << "Restarting..." << endl;
    execvp(argv[0], argv);
  }
  return 0;
}