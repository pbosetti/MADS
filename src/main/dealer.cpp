/*
  ____             _                                    _
 |  _ \  ___  __ _| | ___ _ __    __ _  __ _  ___ _ __ | |_
 | | | |/ _ \/ _` | |/ _ \ '__|  / _` |/ _` |/ _ \ '_ \| __|
 | |_| |  __/ (_| | |  __/ |    | (_| | (_| |  __/ | | | |_
 |____/ \___|\__,_|_|\___|_|     \__,_|\__, |\___|_| |_|\__|
                                       |___/
*/

#include "../dealer.hpp"
#include <cxxopts.hpp>

using namespace std;
using namespace Mads;
using namespace cxxopts;

int main(int argc, char *argv[]) {
  string settings_uri = SETTINGS_URI;
  string agent_name = argv[0];
  size_t count = 0, count_err = 0;

  Options options(argv[0]);
  // clang-format off
  options.add_options()
    ("n,name", "Agent name (default to" + agent_name + ")", value<string>())
    ("i,agent-id", "Agent ID to be added to JSON frames", value<string>());
  // clang-format on
  SETUP_OPTIONS(options, Dealer);

  if (options_parsed.count("name") != 0) {
    agent_name = options_parsed["name"].as<string>();
  } 

  Dealer dealer(agent_name, settings_uri);
  dealer.init();
  dealer.enable_remote_control();
  dealer.connect();

  if (options_parsed.count("agent-id")) {
    dealer.set_agent_id(options_parsed["agent-id"].as<string>());
  }

  dealer.info(cerr);
  dealer.register_event(event_type::startup);
  dealer.loop([&]() {
    json j;
    message_type type = dealer.receive();
    auto msg = dealer.last_message();
    dealer.remote_control();
    switch (type) {
    case message_type::json:
      try {
        j = json::parse(get<1>(msg));
      } catch (const std::exception &e) {
        count_err++;
        break;
      }
      dealer.push(get<1>(msg));
      dealer.publish(j);
      break;
    case message_type::none:
      return;
    default:
      cerr << fg::yellow << "Received unsupported message type" << fg::reset
           << endl;
      count_err++;
      break;
    }
    cerr << "\r\x1b[0KMessages processed: " << fg::green << ++count << fg::reset
         << " total, " << fg::red << count_err << fg::reset << " with errors ";
    cerr.flush();
  });

  dealer.register_event(event_type::shutdown);
  dealer.disconnect();
  if (dealer.restart()) {
    cout << "Restarting..." << endl;
    execvp(argv[0], argv);
  }
  cout << "Dealer terminated" << endl;
  return 0;
}