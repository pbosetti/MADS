/*
  _                                       _
 | |    ___   __ _  __ _  ___ _ __    ___| | __ _ ___ ___
 | |   / _ \ / _` |/ _` |/ _ \ '__|  / __| |/ _` / __/ __|
 | |__| (_) | (_| | (_| |  __/ |    | (__| | (_| \__ \__ \
 |_____\___/ \__, |\__, |\___|_|     \___|_|\__,_|___/___/
             |___/ |___/

This class subscribes to all messages published by the broker and logs them to a
MongoDB instance.

Author(s): Paolo Bosetti
*/

#ifndef LOGGER_HPP
#define LOGGER_HPP


#include "mads.hpp"
#include "agent.hpp"
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>
#include <iostream>
#include <mongocxx/client.hpp>
#include <mongocxx/exception/bulk_write_exception.hpp>
#include <mongocxx/instance.hpp>

using bsoncxx::from_json;
using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_array;
using bsoncxx::builder::basic::make_document;
using bsoncxx::types::b_date;

namespace Mads {

/**
 * @brief The Logger class is responsible for logging messages to a MongoDB
 * instance.
 *
 * It inherits from the Agent class and provides methods for logging messages
 * and connecting to the database.
 *
 * @see Metadata for example usage. This class also has the log method.
 */
class Logger : public Agent {
public:
  /**
   * @brief Constructs a Logger object with the specified name and settings
   * path.
   *
   * @param name The name of the logger.
   * @param settings_path The path to the settings file.
   */
  Logger(std::string name, std::string settings_path)
      : Agent(name, settings_path) {
  }

  ~Logger() { close_db(); }

  /**
   * @brief Sets whether logging to MongoDB is enabled or disabled.
   *
   * @param enabled Whether logging to MongoDB is enabled or disabled. Default
   * is true.
   * @param uri The URI of the MongoDB instance. If not empty, overrides the
   * one in the settings.
   */
  void set_mongo(bool enabled = true, string uri = "") { 
    _log_to_mongo = enabled;
    if (!uri.empty()) {
      _uri = uri;
    }
  }


  /**
   * @brief Sets the file name for logging.
   *
   * @param filename The name of the file to log to.
   * @param array (optional) Indicates whether the log should be stored as an
   * array. If not is is stored as a JSON object per line. Default is false.
   */
  void set_file(string filename, bool array = false) {
    _log_filename = filename;
    _log_to_file = true;
    _log_array = array;
  }

  /**
   * @brief Sets whether logging should be done to a file.
   *
   * @param enabled Whether logging to a file should be enabled or disabled.
   * Default is false.
   */
  void set_file(bool enabled = false) { _log_to_file = enabled; }


  /**
   * @brief Starts the logger.
   *
   * This method opens the logfile (if enabled) and connects to the MongoDB
   * instance (if enabled)
   */
  void open_db() {
    if (_is_open) {
      return;
    }
    if (_log_to_mongo) {
      connect_to_db();
    }
    if (_log_to_file) {
      open_log_file(_log_filename, _log_array);
    }
    _is_open = true;
  }

  /**
   * @brief Stops the logger.
   *
   * This method closes the logfile (if enabled) and disconnects from the
   * MongoDB instance (if enabled)
   */
  void close_db() {
    if (!_is_open) {
      return;
    }
    if (_log_to_file) {
      close_log_file();
    }
    if (_log_to_mongo) {
      for (auto &[k, v] : status()) {
        _db[k].create_index(make_document(kvp("message.timestamp", 1)));
        _db[k].create_index(make_document(kvp("message.hostname", 1)));
        _db[k].create_index(make_document(kvp("message.timecode", 1)));
      }
    }
    _is_open = false;
  }

  /**
   * @brief Overrides the info method from the Agent class.
   *
   * This method provides information about the logger.
   */
  void info(ostream &out = cout) override {
    Agent::info(out);
    if (_log_to_file) {
      out << "  Log file: " << _log_filename << endl;
      out << "  Log file is an array: " << (_log_array ? "yes" : "no") << endl;
    }
    if (_log_to_mongo) {
      out << "  MongoDB URI:      " << style::bold << _uri << style::reset
          << endl;
      out << "  MongoDB database: " << style::bold << _db_name << style::reset
          << endl;
    }
  }

  /**
   * @brief Registers the startup/shutdown of the logger.
   *
   * This method registers the startup of the logger to the MongoDB instance.
   *
   * @param event The event to be registered.
   */
  void register_event(event_type event) {
    if (!_log_to_mongo)
      return;
    nlohmann::json payload;
    stringstream ss;
    payload["name"] = _name;
    payload["event"] = event_map.at(event);
    ss << toml::json_formatter{_config};
    payload["settings"] = nlohmann::json::parse(ss.str());
    tuple<string, string> msg = make_tuple("agent_event", payload.dump());
    log_to_mongo(&msg);
  }

  /**
   * @brief Logs the given message to the MongoDB instance and to the log file.
   * 
   * @param message A touple containing the topic and the message to be logged.
   */
  void log(tuple<string, string> *message = nullptr) {
    if (_log_to_mongo) {
      log_to_mongo(message);
    }
    if (_log_to_file) {
      log_to_file(message);
    }
  }

  /**
   * @brief Logs the last message to the MongoDB instance and to the log file.
   * 
   * @param type the type of message: json or blob.
   */
  void log(message_type type) {
    switch (type) {
    case message_type::none:
      break;
    case message_type::json:
      log();
      break;
    case message_type::blob:
      log_blob_to_mongo();
      break;
    default:
      cerr << "Unsupported message type" << endl;
      break;
    }
  }

  /**
   * @brief Truncates the message to the maximum length.
   *
   * @param message The message to be truncated.
   * @return string The truncated message.
   */
  string truncated_message(const string &message) {
    if (message.length() > _max_length - 3) {
      return message.substr(0, _max_length) + "...";
    } else {
      return message;
    }
  }

public:
  bool paused = false; // Whether the logger is paused or not

private:
  void load_settings() override {
    auto cfg = _config[_name];
    _db_name = cfg["mongo_db"].value_or("mads");
    _uri = cfg["mongo_uri"].value_or("mongodb://localhost:27017");
    _max_length = cfg["max_length"].value_or(75);
    paused = cfg["initially_paused"].value_or(false);
  }

  void connect_to_db() {
    _client = mongocxx::client{mongocxx::uri{_uri}};
    _db = _client[_db_name];
  }

  void open_log_file(string filename, bool array = false) {
    _log_filename = filename;
    _log_array = array;
    _log_file.open(_log_filename);
    if (_log_array) {
      _log_file << "[" << endl;
    }
  }

  void close_log_file() {
    if (_log_file.is_open()) {
      if (_log_array) {
        _log_file.seekp(-2, ios_base::end);
        _log_file << endl << "]" << endl;
      }
      _log_file.close();
    }
  }

  void log_to_mongo(tuple<string, string> *message = nullptr) {
    if (paused) {
      return;
    }
    auto now = chrono::system_clock::now();
    auto doc = make_document();
    auto msg = message ? *message : _last_message;
    auto topic = get<0>(msg);
    if (topic.empty() || topic == LOGGER_STATUS_TOPIC) {
      return;
    }
    try {
      auto j = from_json(get<1>(msg));
      doc = make_document(kvp("timestamp", b_date(now)), kvp("message", j));
    } catch (const bsoncxx::exception &e) {
      cerr << "Error while parsing JSON: " << e.what() << endl;
      doc =
          make_document(kvp("timestamp", b_date(now)), kvp("error", e.what()));
    }
    auto coll = _db[topic];
    try {
      coll.insert_one(doc.view());
    } catch (const mongocxx::bulk_write_exception &e) {
      cerr << fg::red << "Error while inserting document: " << e.what()
           << fg::reset << endl;
    }
  }

  void log_blob_to_mongo() {
    if (paused) {
      return;
    }
    uint32_t blob_size = get<2>(_last_blob).size();
    auto now = chrono::system_clock::now();
    bsoncxx::types::b_binary blob{bsoncxx::binary_sub_type::k_binary,
                                  blob_size, get<2>(_last_blob).data()};
    auto doc =
        make_document(kvp("timestamp", b_date(now)),
                      kvp("message", from_json(get<1>(_last_blob))), 
                      kvp("data", blob));
    auto coll = _db[get<0>(_last_blob)];
    try {
      coll.insert_one(doc.view());
    } catch (const mongocxx::bulk_write_exception &e) {
      cerr << "Error while inserting document: " << e.what() << endl;
    }
  }

  void log_to_file(tuple<string, string> *message = nullptr) {
    if (paused) {
      return;
    }
    auto msg = message ? *message : _last_message;
    _log_file << "{\"" << get<0>(msg) << "\":" << get<1>(msg) << "}"
              << (_log_array ? "," : "") << endl;
  }

  mongocxx::instance _instance{};    // MongoDB instance
  mongocxx::client _client;          // MongoDB client
  mongocxx::database _db;            // MongoDB database
  string _uri;                       // MongoDB URI
  string _db_name;                   // MongoDB database name
  size_t _max_length = 75;           // Maximum length of a message (printing)
  bool _log_to_mongo = true;         // Log to MongoDB
  bool _log_to_file = false;         // Log to file
  string _log_filename = "log.json"; // Log filename
  ofstream _log_file;                // Log file
  bool _log_array = false;           // Log file is an array of JSON objects
  bool _is_open = false;             // Log system is running
};

} // namespace Mads

#endif // LOGGER_HPP