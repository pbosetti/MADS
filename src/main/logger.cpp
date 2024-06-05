/*
  _
 | |    ___   __ _  __ _  ___ _ __
 | |   / _ \ / _` |/ _` |/ _ \ '__|
 | |__| (_) | (_| | (_| |  __/ |
 |_____\___/ \__, |\__, |\___|_|
             |___/ |___/
The logger process: it subscribes to all topics and logs them to a MongoDB
instance.

Author(s): Paolo Bosetti
*/
#include "../logger.hpp"
#include <cxxopts.hpp>
#include <nlohmann/json.hpp>

using namespace std;
using namespace Mads;
using namespace cxxopts;
using json = nlohmann::json;

int main(int argc, char *argv[]) {
  string settings_uri = SETTINGS_URI;
  bool echo = false;

  // CLI options
  Options options(argv[0]);
  options.add_options()
    ("p,pause", "Start paused")
    ("e,echo", "Echo messages to stdout")
    ("n,no-mongo", "Do not log to MongoDB")
    ("m,mongo", "MongoDB connection string (override)", value<string>())
    ("f,file", "Log to file", value<string>())
    ("a,array", "File log is an array of JSON objects (if not, one JSON per line)")
    ("x,cross", "Crossconnect sockets (no broker)");
  SETUP_OPTIONS(options, Logger);

  // Core stuff
  Logger logger(argv[0], settings_uri);
  try {
    logger.init();
  } catch (const std::exception &e) {
    std::cout << fg::red << "Error initializing agent: " << e.what()
              << fg::reset << endl;
    exit(EXIT_FAILURE);
  }

  if (options_parsed.count("echo") != 0) {
    echo = true;
  }

  if (options_parsed.count("pause") != 0) {
    logger.paused = true;
  }

  // Select logging destinations
  if (options_parsed.count("file") != 0) {
    bool array = options_parsed.count("array") != 0;
    logger.set_file(options_parsed["file"].as<string>(), array);
  }
  if (options_parsed.count("no-mongo") != 0) {
    logger.set_mongo(false);
  } else {
    if (options_parsed.count("mongo") != 0) {
      auto uri = options_parsed["mongo"].as<string>();
      logger.set_mongo(true, uri);
    }
    logger.open_db();
  }
  if (options_parsed.count("cross") != 0) {
    logger.set_cross(true);
  }
  // logger.enable_remote_control();
  logger.connect();
  logger.register_event(Mads::event_type::startup);
  logger.info();


  // Main loop
  cout << fg::green << "Logger process started" << fg::reset << endl;
  if (logger.paused) 
    cout << fg::yellow << "Logging is paused" << fg::reset << endl;
  logger.loop([&] {
    message_type type = logger.receive();
    if (type == message_type::none) return;
    auto msg = logger.last_message();
    // check for pause/unpause message
    if (get<0>(msg) == "metadata") {
      auto j = json::parse(get<1>(msg));
      if (!j["pause"].is_null()) {
        logger.paused = j["pause"].get<bool>();
        if (logger.paused) 
          cout << fg::yellow << "Logging is paused" << fg::reset << endl;
        else
          cout << fg::green << "Logging is resumed" << fg::reset << endl;
      }
    }

    // if echo is on, provide feedback
    if (echo) {
      if (type == message_type::json) {
        for (auto const &[k, v] : logger.status()) {
          cout << (logger.paused ? fg::yellow : fg::green)
                << style::bold << k << ": " << style::reset << fg::reset
                << logger.truncated_message(v) << endl;
        }
      } else if (type == message_type::blob) {
        cout << (logger.paused ? fg::yellow : fg::green)
              << style::bold << get<0>(logger.last_blob()) << ": "
              << style::reset << get<1>(logger.last_blob()) << "("
              << get<2>(logger.last_blob()).size() << " bytes)" << endl;
      } 
      cout << fg::reset << style::reset << endl;
    }

    // logger.log() may raise when it cannot contact MongoDB
    try {
      logger.log(type);
    } catch (const AgentError &e) {
      cout << e.what() << endl;
    }
  });
  cout << fg::green << "Logger process stopped" << fg::reset << endl;

  // Cleanup
  logger.register_event(Mads::event_type::shutdown);
  logger.disconnect(); // Not necessary, called by destructor
  logger.close_db();      // Not necessary, called by destructor
  return 0;
}