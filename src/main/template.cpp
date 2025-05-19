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

  // CLI options
  Options options(argv[0]);
  // if needed, add here further CLI options
  // options.add_options()
  //     ("o,option", "option description", value<size_t>());
  SETUP_OPTIONS(options, Agent);

  // Core stuff
  Agent agent(argv[0], settings_uri);
  try {
    agent.init();
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
    cout << "Restarting..." << endl;
    execvp(argv[0], argv);
  }
  return 0;
}