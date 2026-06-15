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
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/types.hpp>
#include <iostream>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <mongocxx/client.hpp>
#include <mongocxx/exception/bulk_write_exception.hpp>
#include <mongocxx/instance.hpp>

using bsoncxx::from_json;
using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_array;
using bsoncxx::builder::basic::make_document;
using bsoncxx::types::b_date;

namespace Mads {

// ---------------------------------------------------------------------------
// nlohmann::json -> bsoncxx conversion (extended-JSON aware for "$date").
//
// Builds BSON directly from a parsed nlohmann::json DOM, avoiding the
// DOM -> text (dump) -> bsoncxx::from_json (re-parse) round-trip that the
// MsgPack logging path would otherwise incur. Output is byte-identical to
// bsoncxx::from_json() for the JSON shapes MADS produces (verified by a parity
// test against from_json, including $date in Z / +hh:mm / +hhmm forms).
// ---------------------------------------------------------------------------
namespace detail {
using bsoncxx::builder::basic::sub_array;
using bsoncxx::builder::basic::sub_document;

// Parse "YYYY-MM-DDThh:mm:ss[.fff][Z|±hh[:]mm]" to ms since the Unix epoch (UTC).
inline bool parse_iso8601_ms(const std::string &s, int64_t &out) {
  int Y, Mo, D, h, mi, se;
  if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &Y, &Mo, &D, &h, &mi, &se) != 6)
    return false;
  int ms = 0;
  if (auto dot = s.find('.'); dot != std::string::npos) {
    std::string f;
    for (size_t i = dot + 1;
         i < s.size() && std::isdigit((unsigned char)s[i]) && f.size() < 3; ++i)
      f += s[i];
    while (f.size() < 3) f += '0';
    ms = std::stoi(f);
  }
  int tz = 0; // minutes east of UTC
  auto tpos = s.find('T');
  std::string tail = (tpos == std::string::npos) ? s : s.substr(tpos + 1);
  if (tail.find('Z') == std::string::npos) {
    auto p = tail.find_last_of("+-");
    if (p != std::string::npos) {
      int sign = tail[p] == '-' ? -1 : 1;
      std::string off = tail.substr(p + 1);
      int oh = 0, om = 0;
      if (off.find(':') != std::string::npos)
        std::sscanf(off.c_str(), "%d:%d", &oh, &om);
      else if (off.size() >= 4) { oh = std::stoi(off.substr(0, 2)); om = std::stoi(off.substr(2, 2)); }
      else if (!off.empty()) oh = std::stoi(off);
      tz = sign * (oh * 60 + om);
    }
  }
  using namespace std::chrono;
  sys_days days = year{Y} / Mo / D;
  auto tp = days + hours{h} + minutes{mi} + seconds{se} + milliseconds{ms} -
            minutes{tz};
  out = duration_cast<milliseconds>(tp.time_since_epoch()).count();
  return true;
}

inline bool date_millis(const nlohmann::json &v, int64_t &out) {
  if (v.is_string()) return parse_iso8601_ms(v.get<std::string>(), out);
  if (v.is_number_integer() || v.is_number_unsigned()) { out = v.get<int64_t>(); return true; }
  if (v.is_object()) {
    auto it = v.find("$numberLong");
    if (it != v.end() && it->is_string()) { out = std::stoll(it->get<std::string>()); return true; }
  }
  return false;
}

inline bool is_date_obj(const nlohmann::json &v, int64_t &ms) {
  if (!v.is_object() || v.size() != 1) return false;
  auto it = v.find("$date");
  return it != v.end() && date_millis(*it, ms);
}

inline void append_array(sub_array arr, const nlohmann::json &v);

inline void append_value(sub_document doc, const std::string &key,
                         const nlohmann::json &v) {
  using namespace bsoncxx::types;
  int64_t ms;
  switch (v.type()) {
  case nlohmann::json::value_t::object:
    if (is_date_obj(v, ms)) { doc.append(kvp(key, b_date{std::chrono::milliseconds{ms}})); break; }
    doc.append(kvp(key, [&](sub_document sub) {
      for (auto &el : v.items()) append_value(sub, el.key(), el.value());
    }));
    break;
  case nlohmann::json::value_t::array:
    doc.append(kvp(key, [&](sub_array sub) {
      for (auto &el : v) append_array(sub, el);
    }));
    break;
  case nlohmann::json::value_t::string: doc.append(kvp(key, v.get<std::string>())); break;
  case nlohmann::json::value_t::boolean: doc.append(kvp(key, v.get<bool>())); break;
  case nlohmann::json::value_t::number_integer:
  case nlohmann::json::value_t::number_unsigned: {
    int64_t n = v.get<int64_t>();
    if (n >= INT32_MIN && n <= INT32_MAX) doc.append(kvp(key, b_int32{(int32_t)n}));
    else doc.append(kvp(key, b_int64{n}));
    break;
  }
  case nlohmann::json::value_t::number_float: doc.append(kvp(key, v.get<double>())); break;
  default: doc.append(kvp(key, b_null{})); break; // null / discarded / binary
  }
}

inline void append_array(sub_array arr, const nlohmann::json &v) {
  using namespace bsoncxx::types;
  int64_t ms;
  switch (v.type()) {
  case nlohmann::json::value_t::object:
    if (is_date_obj(v, ms)) { arr.append(b_date{std::chrono::milliseconds{ms}}); break; }
    arr.append([&](sub_document sub) {
      for (auto &el : v.items()) append_value(sub, el.key(), el.value());
    });
    break;
  case nlohmann::json::value_t::array:
    arr.append([&](sub_array sub) { for (auto &el : v) append_array(sub, el); });
    break;
  case nlohmann::json::value_t::string: arr.append(v.get<std::string>()); break;
  case nlohmann::json::value_t::boolean: arr.append(v.get<bool>()); break;
  case nlohmann::json::value_t::number_integer:
  case nlohmann::json::value_t::number_unsigned: {
    int64_t n = v.get<int64_t>();
    if (n >= INT32_MIN && n <= INT32_MAX) arr.append(b_int32{(int32_t)n});
    else arr.append(b_int64{n});
    break;
  }
  case nlohmann::json::value_t::number_float: arr.append(v.get<double>()); break;
  default: arr.append(b_null{}); break;
  }
}

inline bsoncxx::document::value json_to_bson(const nlohmann::json &j) {
  bsoncxx::builder::basic::document doc;
  if (j.is_object())
    for (auto &el : j.items()) append_value(doc, el.key(), el.value());
  return doc.extract();
}
} // namespace detail

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
    // DOM path: build BSON straight from the payload, no dump + re-parse.
    log_doc_to_mongo("agent_event", payload);
  }

  /**
   * @brief Logs the given message to the MongoDB instance and to the log file.
   * 
   * @param message A touple containing the topic and the message to be logged.
   */
  void log(tuple<string, string> *message = nullptr) {
    if (_log_to_mongo) {
      if (message) {
        log_to_mongo(message); // explicit text payload: faithful from_json path
      } else {
        // Hot path: take the object straight from the agent (no MsgPack
        // re-dump, no re-parse) and build BSON directly from the DOM.
        auto [topic, doc] = last_json();
        log_doc_to_mongo(topic, doc);
      }
    }
    if (_log_to_file) {
      if (message) {
        log_to_file(message);
      } else {
        auto msg = last_message();
        log_to_file(&msg);
      }
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
    auto msg = message ? *message : last_message();
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

  // DOM variant of log_to_mongo: builds the BSON message directly from a
  // nlohmann::json object via detail::json_to_bson() instead of going through
  // text + bsoncxx::from_json(). Output is byte-identical to the text path.
  void log_doc_to_mongo(const std::string &topic,
                        const nlohmann::json &message) {
    if (paused) {
      return;
    }
    if (topic.empty() || topic == LOGGER_STATUS_TOPIC) {
      return;
    }
    auto now = chrono::system_clock::now();
    auto doc = make_document();
    try {
      doc = make_document(kvp("timestamp", b_date(now)),
                          kvp("message", detail::json_to_bson(message)));
    } catch (const std::exception &e) {
      cerr << "Error while converting JSON: " << e.what() << endl;
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
    uint32_t blob_size = static_cast<uint32_t>(get<2>(_last_blob).size());
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
    auto msg = message ? *message : last_message();
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