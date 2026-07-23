/*
  ____             _                                    _
 |  _ \  ___  __ _| | ___ _ __    __ _  __ _  ___ _ __ | |_
 | | | |/ _ \/ _` | |/ _ \ '__|  / _` |/ _` |/ _ \ '_ \| __|
 | |_| |  __/ (_| | |  __/ |    | (_| | (_| |  __/ | | | |_
 |____/ \___|\__,_|_|\___|_|     \__,_|\__, |\___|_| |_|\__|
                                       |___/
*/

#include "../dealer.hpp"
#include "../agent_app.hpp"

using namespace std;
using namespace Mads;

int main(int argc, char *argv[]) {
  size_t count = 0, count_err = 0;

  // CLI options
  AgentAppFor<Dealer> dealer(argv[0], SETTINGS_URI);
  // clang-format off
  dealer.options()
    ("n,name", "Agent name (default to the executable name)",
     cxxopts::value<string>())
    ("i,agent-id", "Agent ID to be added to JSON frames",
     cxxopts::value<string>());
  // clang-format on
  // Also brings in --room, so the broker can be located by service discovery.
  dealer.add_common_options();

  auto options_parsed = dealer.parse_options(argc, argv);
  if (int rc = AgentApp::handle_standard_exit_options<AgentAppFor<Dealer>>(
          options_parsed, dealer.raw_options(), argv);
      rc >= 0) {
    return rc;
  }

  // Core stuff
  try {
    dealer.init(options_parsed);
  } catch (const std::exception &e) {
    std::cout << fg::red << "Error initializing agent: " << e.what()
              << fg::reset << endl;
    exit(EXIT_FAILURE);
  }
  dealer.enable_remote_control();
  // Registers startup on connect() and shutdown on disconnect().
  dealer.enable_events();
  // Dealer::connect() binds the PUSH socket and defaults to no settle delay;
  // pass it explicitly, since AgentApp's own default is 250 ms.
  dealer.connect(0ms);

  dealer.info(cerr);
  dealer.loop([&]() -> chrono::milliseconds {
    json j;
    message_type type = dealer.receive();
    auto msg = dealer.last_message();
    // dealer.remote_control();
    switch (type) {
    case message_type::json:
      try {
        j = json::parse(get<1>(msg));
      } catch (...) {
        count_err++;
        break;
      }
      dealer.push(get<1>(msg));
      dealer.publish(j);
      break;
    case message_type::none:
      return 0ms;
    default:
      cerr << fg::yellow << "Received unsupported message type" << fg::reset
           << endl;
      count_err++;
      break;
    }
    cerr << "\r\x1b[0KMessages processed: " << fg::green << ++count << fg::reset
         << " total, " << fg::red << count_err << fg::reset << " with errors ";
    cerr.flush();
    return 0ms;
  });

  dealer.disconnect();
  dealer.restart_if_requested(argv);
  cout << "Dealer terminated" << endl;
  return 0;
}
