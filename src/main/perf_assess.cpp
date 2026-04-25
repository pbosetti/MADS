/*******************************************************************************
                   __
  _ __   ___ _ __ / _|  __ _ ___ ___  ___  ___ ___
 | '_ \ / _ \ '__| |_  / _` / __/ __|/ _ \/ __/ __|
 | |_) |  __/ |  |  _|| (_| \__ \__ \  __/\__ \__ \
 | .__/ \___|_|  |_|___\__,_|___/___/\___||___/___/
 |_|              |_____|


Assess network performance.

Author(s): Paolo Bosetti
*******************************************************************************/

#include "../mads.hpp"
#include "../agent_app.hpp"
#include <cxxopts.hpp>

using namespace std;
using namespace cxxopts;
using json = nlohmann::json;
using namespace Mads;


int main(int argc, char *argv[]) {
  size_t i = 0, len = 10;
  json payload;
  char *buf;
  chrono::milliseconds time{100};

  // CLI options
  AgentApp agent(argv[0], SETTINGS_URI);
  agent.add_agent_identity_options();
  // clang-format off
  agent.options()
    ("p", "Sampling period (default 100 ms)", value<size_t>())
    ("l", "Byte length of Payload", value<size_t>());
  // clang-format on
  agent.add_common_options();
  
  auto options_parsed = agent.parse_options(argc, argv);
  if (int rc = AgentApp::handle_standard_exit_options<AgentApp>(
          options_parsed, agent.raw_options(), argv);
      rc >= 0) {
    return rc;
  }

  // Settings
  if (options_parsed.count("p") != 0) {
    time = chrono::milliseconds(options_parsed["p"].as<size_t>());
  }
  if (options_parsed.count("l") != 0) {
    len = options_parsed["l"].as<size_t>();
  }

  // Core stuff
  // build a random payload of readable characters
  buf = (char *)malloc(len);
  for (size_t j = 0; j < len; j++) {
    buf[j] = (char)(rand() % 57 + 65);
  }
  buf[len - 1] = 0;  // null-terminate the string

  try {
    agent.init(options_parsed);
  } catch (const std::exception &e) {
    std::cout << fg::red << "Error initializing agent: " << e.what()
              << fg::reset << endl;
    exit(EXIT_FAILURE);
  }
  agent.enable_threaded_remote_control();
  agent.enable_events();
  agent.connect();
  agent.info();

  // Main loop
  cout << fg::green << "Performance assessment process started" << fg::reset
       << endl;
  agent.loop([&]() -> chrono::milliseconds {
    json payload;
    payload["id"] = i;
    payload["origin_timestamp"]["$date"] = 
      get_ISODate_time(chrono::system_clock::now());
    payload["payload"] = buf;
    payload["payload_length"] = len;
    payload["period"] = time.count();
    agent.publish(payload);
    cout << "\r\x1b[0KSent message " << i++;
    cout.flush();
    return 0ms;
  }, time);
  cout << endl << fg::green << "Performance assessment process stopped" 
       << fg::reset << endl;

  // Cleanup
  agent.disconnect();
  agent.restart_if_requested(argv);
  return 0;
}
