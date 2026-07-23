/*
  _                                       _
 | |    ___   __ _  __ _  ___ _ __    ___| | __ _ ___ ___
 | |   / _ \ / _` |/ _` |/ _ \ '__|  / __| |/ _` / __/ __|
 | |__| (_) | (_| | (_| |  __/ |    | (__| | (_| \__ \__ \
 |_____\___/ \__, |\__, |\___|_|     \___|_|\__,_|___/___/
             |___/ |___/

Implementation of the Logger agent. Everything that touches the MongoDB driver
lives here so that logger.hpp stays free of bsoncxx/mongocxx includes.

MADS_HAS_MONGOCXX is defined by the build when MADS_ENABLE_MONGOCXX=ON. Without
it this file still compiles and Logger degrades to a file-only logger.

Author(s): Paolo Bosetti
*/

#include "logger.hpp"

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <sstream>

#ifdef MADS_HAS_MONGOCXX
#include "detail/mongo_instance.hpp"
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/database.hpp>
#include <mongocxx/exception/bulk_write_exception.hpp>
#include <mongocxx/uri.hpp>
#endif

namespace Mads {

#ifdef MADS_HAS_MONGOCXX

using bsoncxx::from_json;
using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::types::b_date;

// ---------------------------------------------------------------------------
// nlohmann::json -> bsoncxx conversion (extended-JSON aware for "$date").
//
// Builds BSON directly from a parsed nlohmann::json DOM, avoiding the
// DOM -> text (dump) -> bsoncxx::from_json (re-parse) round-trip that the
// MsgPack logging path would otherwise incur. Output is byte-identical to
// bsoncxx::from_json() for the JSON shapes MADS produces (verified by a parity
// test against from_json, including $date in Z / +hh:mm / +hhmm forms).
// ---------------------------------------------------------------------------
namespace {
using bsoncxx::builder::basic::sub_array;
using bsoncxx::builder::basic::sub_document;

// Parse "YYYY-MM-DDThh:mm:ss[.fff][Z|±hh[:]mm]" to ms since the Unix epoch (UTC).
bool parse_iso8601_ms(const std::string &s, int64_t &out) {
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

bool date_millis(const nlohmann::json &v, int64_t &out) {
  if (v.is_string()) return parse_iso8601_ms(v.get<std::string>(), out);
  if (v.is_number_integer() || v.is_number_unsigned()) { out = v.get<int64_t>(); return true; }
  if (v.is_object()) {
    auto it = v.find("$numberLong");
    if (it != v.end() && it->is_string()) { out = std::stoll(it->get<std::string>()); return true; }
  }
  return false;
}

bool is_date_obj(const nlohmann::json &v, int64_t &ms) {
  if (!v.is_object() || v.size() != 1) return false;
  auto it = v.find("$date");
  return it != v.end() && date_millis(*it, ms);
}

void append_array(sub_array arr, const nlohmann::json &v);

void append_value(sub_document doc, const std::string &key,
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

void append_array(sub_array arr, const nlohmann::json &v) {
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

bsoncxx::document::value json_to_bson(const nlohmann::json &j) {
  bsoncxx::builder::basic::document doc;
  if (j.is_object())
    for (auto &el : j.items()) append_value(doc, el.key(), el.value());
  return doc.extract();
}
} // namespace

#endif // MADS_HAS_MONGOCXX

// All driver state lives here, so logger.hpp needs no bsoncxx/mongocxx headers.
// The mongocxx::instance is deliberately *not* a member: it is a per-process
// singleton that must outlive every client, so it is shared through
// detail::mongo_instance() instead.
struct Logger::Impl {
  string _uri;                       // MongoDB URI
  string _db_name;                   // MongoDB database name
  size_t _max_length = 75;           // Maximum length of a message (printing)
#ifdef MADS_HAS_MONGOCXX
  bool _log_to_mongo = true;         // Log to MongoDB
#else
  bool _log_to_mongo = false;        // No driver in this build: file only
#endif
  bool _log_to_file = false;         // Log to file
  string _log_filename = "log.json"; // Log filename
  ofstream _log_file;                // Log file
  bool _log_array = false;           // Log file is an array of JSON objects
  bool _is_open = false;             // Log system is running

#ifdef MADS_HAS_MONGOCXX
  mongocxx::client _client;          // MongoDB client
  mongocxx::database _db;            // MongoDB database
#endif
};

Logger::Logger(std::string name, std::string settings_path)
    : Agent(name, settings_path), _impl(std::make_unique<Impl>()) {}

Logger::~Logger() { close_db(); }

bool Logger::has_mongo_support() noexcept {
#ifdef MADS_HAS_MONGOCXX
  return true;
#else
  return false;
#endif
}

void Logger::set_mongo(bool enabled, string uri) {
#ifdef MADS_HAS_MONGOCXX
  _impl->_log_to_mongo = enabled;
  if (!uri.empty()) {
    _impl->_uri = uri;
  }
#else
  (void)uri;
  if (enabled) {
    cerr << fg::yellow
         << "Warning: this build has no MongoDB support; ignoring the request "
            "to log to MongoDB."
         << fg::reset << endl;
  }
  _impl->_log_to_mongo = false;
#endif
}

void Logger::set_file(string filename, bool array) {
  _impl->_log_filename = filename;
  _impl->_log_to_file = true;
  _impl->_log_array = array;
}

void Logger::set_file(bool enabled) { _impl->_log_to_file = enabled; }

void Logger::open_db() {
  if (_impl->_is_open) {
    return;
  }
  if (_impl->_log_to_mongo) {
    connect_to_db();
  }
  if (_impl->_log_to_file) {
    open_log_file(_impl->_log_filename, _impl->_log_array);
  }
  if (!_impl->_log_to_mongo && !_impl->_log_to_file) {
    cerr << fg::yellow
         << "Warning: no logging destination is enabled; messages are received "
            "but not stored."
         << fg::reset << endl;
  }
  _impl->_is_open = true;
}

// Must not throw: ~Logger() calls this, and an exception escaping a destructor
// terminates the process.
void Logger::close_db() {
  if (!_impl || !_impl->_is_open) {
    return;
  }
  // Cleared up front so a failure below cannot leave the logger looking open
  // and have the destructor retry the same failing work.
  _impl->_is_open = false;
  if (_impl->_log_to_file) {
    close_log_file();
  }
#ifdef MADS_HAS_MONGOCXX
  if (_impl->_log_to_mongo) {
    // Index creation talks to the server, so it fails whenever MongoDB is
    // unreachable -- including on the shutdown path, where the agent has
    // usually received messages and status() is non-empty. Report and carry on,
    // as the insert paths do.
    try {
      for (auto &[k, v] : status()) {
        _impl->_db[k].create_index(make_document(kvp("message.timestamp", 1)));
        _impl->_db[k].create_index(make_document(kvp("message.hostname", 1)));
        _impl->_db[k].create_index(make_document(kvp("message.timecode", 1)));
      }
    } catch (const std::exception &e) {
      cerr << fg::red << "Error while creating indexes: " << e.what()
           << fg::reset << endl;
    }
  }
#endif
}

void Logger::info(ostream &out) {
  Agent::info(out);
  if (_impl->_log_to_file) {
    out << "  Log file: " << _impl->_log_filename << endl;
    out << "  Log file is an array: " << (_impl->_log_array ? "yes" : "no")
        << endl;
  }
  if (_impl->_log_to_mongo) {
    out << "  MongoDB URI:      " << style::bold << _impl->_uri << style::reset
        << endl;
    out << "  MongoDB database: " << style::bold << _impl->_db_name
        << style::reset << endl;
  } else if (!has_mongo_support()) {
    out << "  MongoDB:          " << style::bold << "not compiled in"
        << style::reset << endl;
  }
}

void Logger::register_event(event_type event) {
  if (!_impl->_log_to_mongo)
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

void Logger::log(tuple<string, string> *message) {
  if (_impl->_log_to_mongo) {
    if (message) {
      log_to_mongo(message); // explicit text payload: faithful from_json path
    } else {
      // Hot path: take the object straight from the agent (no MsgPack
      // re-dump, no re-parse) and build BSON directly from the DOM.
      auto [topic, doc] = last_json();
      log_doc_to_mongo(topic, doc);
    }
  }
  if (_impl->_log_to_file) {
    if (message) {
      log_to_file(message);
    } else {
      auto msg = last_message();
      log_to_file(&msg);
    }
  }
}

void Logger::log(message_type type) {
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

string Logger::truncated_message(const string &message) {
  if (message.length() > _impl->_max_length - 3) {
    return message.substr(0, _impl->_max_length) + "...";
  } else {
    return message;
  }
}

void Logger::load_settings() {
  auto cfg = _config[_name];
  _impl->_db_name = cfg["mongo_db"].value_or("mads");
  _impl->_uri = cfg["mongo_uri"].value_or("mongodb://localhost:27017");
  _impl->_max_length = cfg["max_length"].value_or(75);
  paused = cfg["initially_paused"].value_or(false);
}

void Logger::connect_to_db() {
#ifdef MADS_HAS_MONGOCXX
  // Materialise the process-wide driver instance before the first client.
  detail::mongo_instance();
  _impl->_client = mongocxx::client{mongocxx::uri{_impl->_uri}};
  _impl->_db = _impl->_client[_impl->_db_name];
#endif
}

void Logger::open_log_file(string filename, bool array) {
  _impl->_log_filename = filename;
  _impl->_log_array = array;
  // Binary mode: close_log_file()'s seekp(-2, end) assumes each written "\n"
  // is exactly one byte on disk. In text mode, Windows silently translates
  // "\n" to "\r\n", which throws that fixed-size rewind off by one byte per
  // line and corrupts the array-mode trailing-comma/opening-bracket fixup.
  _impl->_log_file.open(_impl->_log_filename, ios_base::out | ios_base::binary);
  if (_impl->_log_array) {
    _impl->_log_file << "[" << endl;
  }
}

void Logger::close_log_file() {
  if (_impl->_log_file.is_open()) {
    if (_impl->_log_array) {
      _impl->_log_file.seekp(-2, ios_base::end);
      _impl->_log_file << endl << "]" << endl;
    }
    _impl->_log_file.close();
  }
}

void Logger::log_to_mongo(tuple<string, string> *message) {
#ifdef MADS_HAS_MONGOCXX
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
    doc = make_document(kvp("timestamp", b_date(now)), kvp("error", e.what()));
  }
  auto coll = _impl->_db[topic];
  try {
    coll.insert_one(doc.view());
  } catch (const mongocxx::bulk_write_exception &e) {
    cerr << fg::red << "Error while inserting document: " << e.what()
         << fg::reset << endl;
  }
#else
  (void)message;
#endif
}

// DOM variant of log_to_mongo: builds the BSON message directly from a
// nlohmann::json object via json_to_bson() instead of going through text +
// bsoncxx::from_json(). Output is byte-identical to the text path.
void Logger::log_doc_to_mongo(const std::string &topic,
                              const nlohmann::json &message) {
#ifdef MADS_HAS_MONGOCXX
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
                        kvp("message", json_to_bson(message)));
  } catch (const std::exception &e) {
    cerr << "Error while converting JSON: " << e.what() << endl;
    doc = make_document(kvp("timestamp", b_date(now)), kvp("error", e.what()));
  }
  auto coll = _impl->_db[topic];
  try {
    coll.insert_one(doc.view());
  } catch (const mongocxx::bulk_write_exception &e) {
    cerr << fg::red << "Error while inserting document: " << e.what()
         << fg::reset << endl;
  }
#else
  (void)topic;
  (void)message;
#endif
}

void Logger::log_blob_to_mongo() {
#ifdef MADS_HAS_MONGOCXX
  if (paused) {
    return;
  }
  uint32_t blob_size = static_cast<uint32_t>(get<2>(_last_blob).size());
  auto now = chrono::system_clock::now();
  bsoncxx::types::b_binary blob{bsoncxx::binary_sub_type::k_binary, blob_size,
                                get<2>(_last_blob).data()};
  auto doc = make_document(kvp("timestamp", b_date(now)),
                           kvp("message", from_json(get<1>(_last_blob))),
                           kvp("data", blob));
  auto coll = _impl->_db[get<0>(_last_blob)];
  try {
    coll.insert_one(doc.view());
  } catch (const mongocxx::bulk_write_exception &e) {
    cerr << "Error while inserting document: " << e.what() << endl;
  }
#endif
}

void Logger::log_to_file(tuple<string, string> *message) {
  if (paused) {
    return;
  }
  auto msg = message ? *message : last_message();
  _impl->_log_file << "{\"" << get<0>(msg) << "\":" << get<1>(msg) << "}"
                   << (_impl->_log_array ? "," : "") << endl;
}

} // namespace Mads
