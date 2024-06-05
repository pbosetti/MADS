/*
  ____                                  
 |  _ \ _   _ _ __ ___  _ __ ___  _   _ 
 | | | | | | | '_ ` _ \| '_ ` _ \| | | |
 | |_| | |_| | | | | | | | | | | | |_| |
 |____/ \__,_|_| |_| |_|_| |_| |_|\__, |
                                  |___/ 
A dummy data generator process.

Author(s): Paolo Bosetti
*/
#include "../dummy.hpp"
#include <cxxopts.hpp>

using namespace std;
using namespace cxxopts;
using json = nlohmann::json;
using namespace Mads;

int main(int argc, char *argv[]) {
  string settings_uri = SETTINGS_URI;
  chrono::milliseconds sampling_time{100};

  // CLI options
  Options options(argv[0]);
  options.add_options()
      ("p", "Sampling period (default 100 ms)", value<size_t>())
      ("e", "echo publishing ops");
  SETUP_OPTIONS(options, Dummy)

  // Settings
  bool echo = options_parsed.count("e") != 0;
  if (options_parsed.count("p") != 0) {
    sampling_time = chrono::milliseconds(options_parsed["p"].as<size_t>());
  }

  // Core stuff
  Dummy dummy(argv[0], settings_uri);
  try {
    dummy.init();
  } catch (const std::exception &e) {
    std::cout << fg::red << "Error initializing agent: " << e.what()
              << fg::reset << endl;
    exit(EXIT_FAILURE);
  }
  dummy.enable_remote_control();
  dummy.connect();
  dummy.register_event(event_type::startup);
  dummy.info();

  // Main loop
  cout << fg::green << "Dummy process started" << fg::reset << endl;
  dummy.loop([&]() {
    dummy.publish(echo);
  }, sampling_time);
  cout << fg::green << "Dummy process stopped" << fg::reset << endl;

  // Cleanup
  dummy.register_event(event_type::shutdown);
  dummy.disconnect();
  if (dummy.restart()) {
    cout << "Restarting..." << endl;
    execvp(argv[0], argv);
  }
  // Done.
  return 0;
}
