/*
  ___                            
 |_ _|_ __ ___   __ _  __ _  ___ 
  | || '_ ` _ \ / _` |/ _` |/ _ \
  | || | | | | | (_| | (_| |  __/
 |___|_| |_| |_|\__,_|\__, |\___|
                      |___/      
This agent watches a list of files for write changes and publishes the given
file whenever a change is detected. The file is supposed to be a binary file
and it is published as such.

Author(s): Paolo Bosetti
*/
#include "../image.hpp"
#include <cxxopts.hpp>


using namespace std;
using namespace cxxopts;
using json = nlohmann::json;
using namespace Mads;

int main(int argc, char *argv[]) {
  string settings_uri = SETTINGS_URI;
  chrono::milliseconds sampling_time{500};

  // CLI options
  Options options(argv[0]);
  options.add_options()
      ("p", "Sampling period (default 500 ms)", value<size_t>());
  SETUP_OPTIONS(options, Image);

  // Settings
  if (options_parsed.count("p") != 0) {
    sampling_time = chrono::milliseconds(options_parsed["p"].as<size_t>());
  }

  // Core stuff
  Image image(argv[0], settings_uri);
  try {
    image.init();
  } catch (const std::exception &e) {
    std::cout << fg::red << "Error initializing agent: " << e.what()
              << fg::reset << endl;
    exit(EXIT_FAILURE);
  }
  image.enable_remote_control();
  image.connect();
  image.register_event(event_type::startup);
  image.info();

  // Main loop
  cout << fg::green << "Image process started" << fg::reset << endl;
  image.loop([&]() {
    image.publish_change();
  }, sampling_time);
  cout << fg::green << "Image process stopped" << fg::reset << endl;

  // Cleanup
  image.register_event(event_type::shutdown);
  image.disconnect();
  if (image.restart()) {
    auto cmd = string(MADS_PREFIX) + argv[0];
    cout << "Restarting " << cmd << "..." << endl;
    execvp(cmd.c_str(), argv);
  }
  return 0;
}
