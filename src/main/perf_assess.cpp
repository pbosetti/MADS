/*
                   __
  _ __   ___ _ __ / _|  __ _ ___ ___  ___  ___ ___
 | '_ \ / _ \ '__| |_  / _` / __/ __|/ _ \/ __/ __|
 | |_) |  __/ |  |  _|| (_| \__ \__ \  __/\__ \__ \
 | .__/ \___|_|  |_|___\__,_|___/___/\___||___/___/
 |_|              |_____|


Assess network performance.

Author(s): Paolo Bosetti
*/
#include "../mads.hpp"
#include "../agent.hpp"
#include <cxxopts.hpp>

using namespace std;
using namespace cxxopts;
using json = nlohmann::json;
using namespace Mads;


int main(int argc, char *argv[]) {
  string settings_uri = SETTINGS_URI;
  size_t i = 0, len = 10;
  json payload;
  char *buf;
  chrono::milliseconds time{100};
  bool crypto = false;
  filesystem::path key_dir(Mads::exec_dir() + "/../etc");
  string client_key_name = "client";
  string server_key_name = "broker";
  auth_verbose auth_verbose = auth_verbose::off;

  // CLI options
  Options options(argv[0]);
  options.add_options()
    ("p", "Sampling period (default 100 ms)", value<size_t>())
    ("l", "Byte length of Payload", value<size_t>());
  SETUP_OPTIONS(options, Agent);

  // Settings
  if (options_parsed.count("p") != 0) {
    time = chrono::milliseconds(options_parsed["p"].as<size_t>());
  }
  if (options_parsed.count("l") != 0) {
    len = options_parsed["l"].as<size_t>();
  }

  if (options_parsed.count("crypto") != 0) {
    crypto = true;
    if (options_parsed.count("keys_dir") != 0) {
      key_dir = options_parsed["keys_dir"].as<string>();
    }
    if (options_parsed.count("key_server") != 0) {
      server_key_name = options_parsed["key_server"].as<string>();
    }
    if (options_parsed.count("key_client") != 0) {
      client_key_name = options_parsed["key_client"].as<string>();
    }
    if (options_parsed.count("auth_verbose") != 0) {
      auth_verbose = auth_verbose::on;
    }
  }

  // Core stuff
  // build a random payload of readable characters
  buf = (char *)malloc(len);
  for (size_t j = 0; j < len; j++) {
    buf[j] = (char)(rand() % 57 + 65);
  }
  buf[len - 1] = 0;  // null-terminate the string

  Agent agent(argv[0], settings_uri);
  if (crypto) {
    agent.set_key_dir(key_dir);
    agent.client_key_name = client_key_name;
    agent.server_key_name = server_key_name;
    agent.auth_verbose = auth_verbose;
  }
  try {
    agent.init(crypto);
  } catch (const std::exception &e) {
    std::cout << fg::red << "Error initializing agent: " << e.what()
              << fg::reset << endl;
    exit(EXIT_FAILURE);
  }
  agent.enable_remote_control();
  agent.connect();
  agent.register_event(event_type::startup);
  agent.info();

  // Main loop
  cout << fg::green << "Performance assessment process started" << fg::reset
       << endl;
  agent.loop([&]() {
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
  }, time);
  cout << endl << fg::green << "Performance assessment process stopped" 
       << fg::reset << endl;

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