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
  bool crypto = false;
  filesystem::path key_dir(Mads::exec_dir() + "/../etc");
  string client_key_name = "client";
  string server_key_name = "broker";
  auth_verbose auth_verbose = auth_verbose::off;

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

  if (options_parsed.count("crypto") != 0) {
    crypto = true;
    if (options_parsed.count("keys_dir") != 0) {
      key_dir = options_parsed["keys_dir"].as<string>();
    }
    if (options_parsed.count("key_broker") != 0) {
      server_key_name = options_parsed["key_broker"].as<string>();
    }
    if (options_parsed.count("key_client") != 0) {
      client_key_name = options_parsed["key_client"].as<string>();
    }
    if (options_parsed.count("auth_verbose") != 0) {
      auth_verbose = auth_verbose::on;
    }
  }

  Dealer dealer(agent_name, settings_uri);
  if (crypto) {
    dealer.set_key_dir(key_dir);
    dealer.client_key_name = client_key_name;
    dealer.server_key_name = server_key_name;
    dealer.auth_verbose = auth_verbose;
  }
  dealer.init(crypto);
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
    auto cmd = string(MADS_PREFIX) + argv[0];
    cout << "Restarting " << cmd << "..." << endl;
    execvp(cmd.c_str(), argv);
  }
  cout << "Dealer terminated" << endl;
  return 0;
}