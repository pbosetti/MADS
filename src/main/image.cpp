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
#include "../agent_app.hpp"


using namespace std;
using json = nlohmann::json;
using namespace Mads;

int main(int argc, char *argv[]) {
  chrono::milliseconds sampling_time{500};

  // CLI options
  AgentAppFor<Image> image(argv[0], SETTINGS_URI);
  image.options()
    ("p", "Sampling period (default 500 ms)", cxxopts::value<size_t>());
  image.add_common_options();

  auto options_parsed = image.parse_options(argc, argv);
  if (int rc = AgentApp::handle_standard_exit_options<AgentAppFor<Image>>(
          options_parsed, image.raw_options(), argv);
      rc >= 0) {
    return rc;
  }

  // Settings
  if (options_parsed.count("p") != 0) {
    sampling_time = chrono::milliseconds(options_parsed["p"].as<size_t>());
  }

  // Core stuff
  try {
    image.init(options_parsed);
  } catch (const std::exception &e) {
    std::cout << fg::red << "Error initializing agent: " << e.what()
              << fg::reset << endl;
    exit(EXIT_FAILURE);
  }
  image.enable_remote_control();
  image.enable_events();
  image.connect();
  image.info();

  // Main loop
  cout << fg::green << "Image process started" << fg::reset << endl;
  image.loop([&]() -> chrono::milliseconds {
    image.publish_change();
    return 0ms;
  }, sampling_time);
  cout << fg::green << "Image process stopped" << fg::reset << endl;

  // Cleanup
  image.disconnect();
  image.restart_if_requested(argv);
  return 0;
}
