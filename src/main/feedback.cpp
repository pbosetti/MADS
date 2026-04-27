/*
  _____             _ _                _
 |  ___|__  ___  __| | |__   __ _  ___| | __
 | |_ / _ \/ _ \/ _` | '_ \ / _` |/ __| |/ /
 |  _|  __/  __/ (_| | |_) | (_| | (__|   <
 |_|  \___|\___|\__,_|_.__/ \__,_|\___|_|\_\

A pure subscriber. Provides feedback to the user.

Author(s): Paolo Bosetti
*/
#include "../agent_app.hpp"
#include "../mads.hpp"

using namespace std;
using json = nlohmann::json;
using namespace Mads;
using namespace cxxopts;

int main(int argc, char *argv[]) {
  json params;
  int width = 65, indent = -1;
  bool dont_block = false;

  // CLI options
  AgentApp agent(argv[0], SETTINGS_URI);
  agent.add_dont_block_option();
  agent.add_queue_size_option();
  agent.add_common_options();

  auto options_parsed = agent.parse_options(argc, argv);
  if (int rc = AgentApp::handle_standard_exit_options<AgentApp>(
          options_parsed, agent.raw_options(), argv);
      rc >= 0) {
    return rc;
  }

  // Core stuff
  try {
    agent.init(options_parsed);
  } catch (const std::exception &e) {
    std::cout << fg::red << "Error initializing agent: " << e.what()
              << fg::reset << endl;
    exit(EXIT_FAILURE);
  } catch (...) {
    std::cout << fg::red << "Error initializing agent: Unexpected" << fg::reset
              << endl;
    exit(EXIT_FAILURE);
  }

  params = agent.settings_json();
  agent.apply_receive_timeout();
  agent.apply_queue_size();

  dont_block = params.value("dont_block", dont_block);
  if (options_parsed.count("dont-block") != 0) {
    dont_block = true;
  }

  agent.enable_remote_control();
  agent.enable_events();
  agent.connect();
  agent.info();

  if (dont_block) {
    cerr << fg::yellow << "Running in non-blocking mode" << fg::reset << endl;
  }

  try {
    width = params["print_width"].get<int>();
  } catch (...) {
  }
  try {
    indent = params["indent_width"].get<int>();
  } catch (...) {
  }
  // Main loop
  cout << fg::green << "Feedback process started" << fg::reset << endl;
  agent.loop([&]() -> chrono::milliseconds {
    message_type type = agent.receive(dont_block);
    auto msg = agent.last_message();
    // agent.remote_control();
    if (get<0>(msg) == LOGGER_STATUS_TOPIC) {
      return 0ms;
    }
    switch (type) {
    case message_type::json:
      if (width > 0) {
        cout << style::bold << agent.last_topic() << ": " << style::reset
             << get<1>(msg).substr(0, width) << "..." << endl;
      } else {
        cout << style::bold << agent.last_topic() << ": " << style::reset
             << json::parse(get<1>(msg)).dump(indent) << endl;
      }
      break;
    case message_type::blob:
      cout << fg::yellow << "Received BLOB message" << fg::reset << endl;
      break;
    case message_type::none:
      // cout << ".";
      // flush(cout);
      break;
    default:
      cout << fg::red << "Received unknown message type" << fg::reset << endl;
      break;
    }
    return 0ms;
  });
  cout << fg::green << "Feedback process stopped" << fg::reset << endl;

  // Cleanup
  agent.disconnect();
  agent.restart_if_requested(argv);
  return 0;
}
