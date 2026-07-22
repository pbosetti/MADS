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
#include "../agent_app.hpp"

using namespace std;
using namespace Mads;
using json = nlohmann::json;

int main(int argc, char *argv[]) {
  bool echo = false;

  // CLI options
  AgentAppFor<Logger> logger(argv[0], SETTINGS_URI);
  logger.options()
    ("p,pause", "Start paused")
    ("e,echo", "Echo messages to stdout")
    ("n,no-mongo", "Do not log to MongoDB")
    ("m,mongo", "MongoDB connection string (override)", cxxopts::value<string>())
    ("f,file", "Log to file", cxxopts::value<string>())
    ("a,array", "File log is an array of JSON objects (if not, one JSON per line)")
    ("x,cross", "Crossconnect sockets (no broker)");
  // Also brings in --room, so the broker can be located by service discovery.
  logger.add_common_options();

  auto options_parsed = logger.parse_options(argc, argv);
  if (int rc = AgentApp::handle_standard_exit_options<AgentAppFor<Logger>>(
          options_parsed, logger.raw_options(), argv);
      rc >= 0) {
    return rc;
  }

  // Core stuff
  try {
    logger.init(options_parsed);
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
  logger.enable_remote_control();
  // Registers startup on connect() and shutdown on disconnect().
  logger.enable_events();

  // Deprecated queue size option. Agent::init() already applies `queue_size`
  // from the settings, so only the superseded key is handled here.
  const auto &params = logger.settings_json();
  const bool uses_deprecated_watermark =
      params.contains("high_watermark") && !params["high_watermark"].is_null();
  if (uses_deprecated_watermark) {
    logger.set_high_watermark(params.value("high_watermark", 1000));
  }

  logger.connect();
  logger.info();
  if (uses_deprecated_watermark) {
    cerr << fg::yellow
         << "Warning: high_watermark setting is deprecated, use queue_size"
         << fg::reset << endl;
  }

  // Create and start the thread
  std::thread logger_status_thread([&logger] {
    json j;
    while (logger.runtime()->running()) {
      j["logger_paused"] = logger.paused;;
      logger.publish(j, LOGGER_STATUS_TOPIC);
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    cout << "Logger thread stopped" << endl;
  });


  // Main loop
  cout << fg::green << "Logger process started" << fg::reset << endl;
  if (logger.paused)
    cout << fg::yellow << "Logging is paused" << fg::reset << endl;
  logger.loop([&]() -> chrono::milliseconds {
    message_type type = logger.receive();
    if (type == message_type::none) return 0ms;
    auto msg = logger.last_message();
    // check for pause/unpause message
    if (get<0>(msg) == "metadata") {
      json j;
      try {
        j = json::parse(get<1>(msg));
      } catch (json::parse_error &e) {
        cerr << fg::red << e.what() << endl
             <<"Error parsing message content:" << fg::reset
             << endl << get<1>(msg) << endl;
        type = message_type::error;
      }
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
      } else if (type == message_type::error) {
        cerr << fg::red << "Error parsing message content:" << fg::reset << endl;
        cerr << get<1>(msg) << endl;
      }
      cout << fg::reset << style::reset << endl;
    }

    // logger.log() may raise when it cannot contact MongoDB
    try {
      logger.log(type);
    } catch (const AgentError &e) {
      cout << e.what() << endl;
    }
    return 0ms;
  });
  cout << fg::green << "Logger process stopped" << fg::reset << endl;

  // Cleanup
  logger_status_thread.join();
  // Explicit: disconnect() is what registers the shutdown event, and the
  // database must stay open until it has been written.
  logger.disconnect();
  logger.close_db();
  logger.restart_if_requested(argv);
  return 0;
}
