/*
  _____             _ _                _    
 |  ___|__  ___  __| | |__   __ _  ___| | __
 | |_ / _ \/ _ \/ _` | '_ \ / _` |/ __| |/ /
 |  _|  __/  __/ (_| | |_) | (_| | (__|   < 
 |_|  \___|\___|\__,_|_.__/ \__,_|\___|_|\_\
                                            
A pure subscriber. Provides feedback to the user.

Author(s): Paolo Bosetti
*/
#include "../mads.hpp"
#include "../agent.hpp"
#include <cxxopts.hpp>
#include <filesystem>

using namespace std;
using namespace cxxopts;
using json = nlohmann::json;
using namespace Mads;

int main(int argc, char *argv[]) {
  string settings_uri = SETTINGS_URI;
  json payload, params;
  bool crypto = false;
  filesystem::path key_dir(Mads::exec_dir() + "/../etc");
  string client_key_name = "client";
  string server_key_name = "broker";
  auth_verbose auth_verbose = auth_verbose::off;
  int width = 65, indent = -1;
  bool dont_block = false;

  // CLI options
  Options options(argv[0]);
  options.add_options()
    ("b,dont-block", "don't block on read");
  SETUP_OPTIONS(options, Agent);

  // Settings
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
  
  // Core stuff
  Agent agent(argv[0], settings_uri);
  if (crypto) {
    agent.set_key_dir(key_dir);
    agent.client_key_name = client_key_name;
    agent.server_key_name = server_key_name;
    agent.auth_verbose = auth_verbose;
  }
  try {
    agent.init(crypto);
  } catch(const std::exception& e) {
    std::cout << fg::red << "Error initializing agent: " << e.what() << fg::reset << endl;
    exit(EXIT_FAILURE);
  } catch (...) {
    std::cout << fg::red << "Error initializing agent: Unexpected" << fg::reset << endl;
    exit(EXIT_FAILURE);
  }
  agent.enable_remote_control();
  params = agent.get_settings();

  if (params["receive_timeout"].is_number_integer()) {
    agent.set_receive_timeout(params["receive_timeout"]);
  }
  
  // deprecated queue size option:
  if (!params["high_watermark"].is_null()) {
    agent.set_high_watermark(params.value("high_watermark", 1000));
  }

  agent.connect();
  agent.register_event(event_type::startup);
  agent.info();
  if (!params["high_watermark"].is_null()) {
    cerr << fg::yellow 
         << "Warning: high_watermark setting is deprecated, use queue_size" 
         << fg::reset << endl;
  }

  /*
  if (!params["dont_block"].is_null()) {
    dont_block = params.value("dont_block", false);
    cerr << fg::yellow << "Running in non-blocking mode" << fg::reset << endl;
  }
  if (!params["dont-block"].is_null()) {
    dont_block = params.value("dont-block", false);
    cerr << fg::yellow 
         << "Warning: dont-block setting is deprecated, use dont_block" 
         << fg::reset << endl;
    cerr << fg::yellow << "Running in non-blocking mode" << fg::reset << endl;
  }
  */
  dont_block = params.value("dont_block", dont_block);
  if (!params["dont-block"].is_null()) {
    cerr << fg::yellow
        << "Warning: dont-block setting is deprecated, use dont_block"
        << fg::reset << endl;
  }
  if (options_parsed.count("dont-block") != 0) {
    dont_block = true;
  }
  if (dont_block) {
    cerr << fg::yellow << "Running in non-blocking mode" << fg::reset << endl;
  }

  try {
    width = params["print_width"].get<int>();
  } catch (...) {}
  try {
    indent = params["indent_width"].get<int>();
  } catch (...) {}
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
  agent.register_event(event_type::shutdown);
  agent.disconnect();
  if (agent.restart()) {
    auto cmd = string(MADS_PREFIX) + argv[0];
    cout << "Restarting " << cmd << "..." << endl;
    execvp(cmd.c_str(), argv);
  }
  return 0;
}