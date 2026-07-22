/*
  __  __                         _____    _       _     
 |  \/  | ___  _ __   __ _  ___ |  ___|__| |_ ___| |__  
 | |\/| |/ _ \| '_ \ / _` |/ _ \| |_ / _ \ __/ __| '_ \ 
 | |  | | (_) | | | | (_| | (_) |  _|  __/ || (__| | | |
 |_|  |_|\___/|_| |_|\__, |\___/|_|  \___|\__\___|_| |_|
                     |___/                              
*/
#include "mongo_fetch.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/cursor.hpp>
#include <mongocxx/database.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/uri.hpp>

#include "detail/mongo_instance.hpp"

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_array;
using bsoncxx::builder::basic::make_document;
using Mads::detail::mongo_instance;

namespace {

bool is_integer_string(std::string_view value) {
  if (value.empty()) {
    return false;
  }

  std::size_t index = 0;
  if (value.front() == '+' || value.front() == '-') {
    index = 1;
  }
  if (index == value.size()) {
    return false;
  }

  for (; index < value.size(); ++index) {
    if (!std::isdigit(static_cast<unsigned char>(value[index]))) {
      return false;
    }
  }

  return true;
}

std::time_t timegm_portable(std::tm *tm_value) {
#if defined(_WIN32)
  return _mkgmtime(tm_value);
#else
  return timegm(tm_value);
#endif
}

std::optional<std::chrono::milliseconds> parse_time_string(std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }

  if (is_integer_string(value)) {
    std::int64_t epoch_ms = 0;
    const char *begin = value.data();
    const char *end = value.data() + value.size();
    auto result = std::from_chars(begin, end, epoch_ms);
    if (result.ec == std::errc{} && result.ptr == end) {
      return std::chrono::milliseconds{epoch_ms};
    }
  }

  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  char suffix[16] = {};

  const int fields = std::sscanf(
    std::string{value}.c_str(),
    "%d-%d-%dT%d:%d:%d%15s",
    &year,
    &month,
    &day,
    &hour,
    &minute,
    &second,
    suffix
  );

  if (fields != 7) {
    return std::nullopt;
  }

  std::string_view suffix_view{suffix};
  if (suffix_view.empty() || suffix_view.back() != 'Z') {
    return std::nullopt;
  }

  suffix_view.remove_suffix(1);

  int fractional_ms = 0;
  if (!suffix_view.empty()) {
    if (suffix_view.front() != '.') {
      return std::nullopt;
    }

    suffix_view.remove_prefix(1);
    if (suffix_view.empty()) {
      return std::nullopt;
    }

    std::int64_t fractional_value = 0;
    const char *fraction_begin = suffix_view.data();
    const char *fraction_end = suffix_view.data() + suffix_view.size();
    auto fraction_result = std::from_chars(fraction_begin, fraction_end, fractional_value);
    if (fraction_result.ec != std::errc{} || fraction_result.ptr != fraction_end) {
      return std::nullopt;
    }

    if (suffix_view.size() > 3) {
      for (std::size_t i = 3; i < suffix_view.size(); ++i) {
        fractional_value /= 10;
      }
    } else {
      for (std::size_t i = suffix_view.size(); i < 3; ++i) {
        fractional_value *= 10;
      }
    }

    fractional_ms = static_cast<int>(fractional_value);
  }

  std::tm tm_value{};
  tm_value.tm_year = year - 1900;
  tm_value.tm_mon = month - 1;
  tm_value.tm_mday = day;
  tm_value.tm_hour = hour;
  tm_value.tm_min = minute;
  tm_value.tm_sec = second;

  const std::time_t epoch_seconds = timegm_portable(&tm_value);
  if (epoch_seconds == static_cast<std::time_t>(-1)) {
    return std::nullopt;
  }

  return std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::seconds{epoch_seconds}
         ) +
         std::chrono::milliseconds{fractional_ms};
}

std::chrono::milliseconds extract_timestamp(const bsoncxx::document::view &document) {
  const auto timestamp = document["timestamp"];
  if (!timestamp) {
    throw std::runtime_error("Document is missing the required `timestamp` field.");
  }

  switch (timestamp.type()) {
    case bsoncxx::type::k_date:
      return std::chrono::duration_cast<std::chrono::milliseconds>(
        timestamp.get_date().value
      );
    case bsoncxx::type::k_int64:
      return std::chrono::milliseconds{timestamp.get_int64().value};
    case bsoncxx::type::k_int32:
      return std::chrono::milliseconds{timestamp.get_int32().value};
    case bsoncxx::type::k_string: {
      const auto parsed = parse_time_string(std::string{timestamp.get_string().value});
      if (!parsed) {
        throw std::runtime_error("Unsupported string format for `timestamp` field.");
      }
      return *parsed;
    }
    default:
      throw std::runtime_error("Unsupported BSON type for `timestamp` field.");
  }
}

bool is_in_range(
  std::chrono::milliseconds timestamp,
  const std::optional<std::chrono::milliseconds> &start_time,
  const std::optional<std::chrono::milliseconds> &end_time
) {
  if (start_time && timestamp < *start_time) {
    return false;
  }
  if (end_time && timestamp > *end_time) {
    return false;
  }
  return true;
}

std::string make_view_name() {
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()
  );
  const auto thread_id = std::hash<std::thread::id>{}(std::this_thread::get_id());
  return "mongo_fetch_view_" + std::to_string(now.count()) + "_" + std::to_string(thread_id);
}

struct ReplayRowData {
  std::chrono::milliseconds timestamp{0};
  std::string collection_name;
  nlohmann::json data;
};

ReplayRowData parse_replay_row(const bsoncxx::document::view &row) {
  ReplayRowData parsed_row;
  const auto data = row["data"];
  const auto source_collection = row["collection_name"];
  if (!data || data.type() != bsoncxx::type::k_document) {
    throw std::runtime_error("Replay view row is missing the `data` document.");
  }
  if (!source_collection || source_collection.type() != bsoncxx::type::k_string) {
    throw std::runtime_error("Replay view row is missing the `collection_name` field.");
  }

  parsed_row.timestamp = extract_timestamp(row);
  parsed_row.collection_name = std::string{source_collection.get_string().value};
  parsed_row.data = nlohmann::json::parse(bsoncxx::to_json(data.get_document().value));
  return parsed_row;
}

bsoncxx::document::value make_project_stage(const std::string &collection_name) {
  return make_document(
    kvp(
      "$project",
      make_document(
        kvp("_id", 0),
        kvp(
          "timestamp",
          make_document(kvp("$toDate", "$timestamp"))
        ),
        kvp("collection_name", collection_name),
        kvp("data", "$$ROOT")
      )
    )
  );
}

bsoncxx::document::value make_match_stage(
  const std::optional<std::chrono::milliseconds> &start_time,
  const std::optional<std::chrono::milliseconds> &end_time
) {
  return make_document(
    kvp(
      "$match",
      make_document(
        kvp(
          "timestamp",
          [start_time, end_time](bsoncxx::builder::basic::sub_document sub_document) {
            if (start_time) {
              sub_document.append(kvp("$gte", bsoncxx::types::b_date{*start_time}));
            }
            if (end_time) {
              sub_document.append(kvp("$lte", bsoncxx::types::b_date{*end_time}));
            }
          }
        )
      )
    )
  );
}

bsoncxx::array::value make_view_pipeline(
  const std::vector<std::string> &collections,
  const std::optional<std::chrono::milliseconds> &start_time,
  const std::optional<std::chrono::milliseconds> &end_time
) {
  bsoncxx::builder::basic::array pipeline_builder;
  pipeline_builder.append(make_project_stage(collections.front()));
  if (start_time || end_time) {
    pipeline_builder.append(make_match_stage(start_time, end_time));
  }

  for (std::size_t index = 1; index < collections.size(); ++index) {
    bsoncxx::builder::basic::array union_pipeline;
    union_pipeline.append(make_project_stage(collections[index]));
    if (start_time || end_time) {
      union_pipeline.append(make_match_stage(start_time, end_time));
    }

    pipeline_builder.append(
      make_document(
        kvp(
          "$unionWith",
          make_document(
            kvp("coll", collections[index]),
            kvp("pipeline", union_pipeline.extract())
          )
        )
      )
    );
  }

  pipeline_builder.append(
    make_document(kvp("$sort", make_document(kvp("timestamp", 1), kvp("collection_name", 1))))
  );

  return pipeline_builder.extract();
}

void validate_replay_view(
  mongocxx::database &database,
  const std::string &view_name
) {
  auto collections = database.list_collections(make_document(kvp("name", view_name)));
  auto collection_it = collections.begin();
  if (collection_it == collections.end()) {
    throw std::runtime_error("Replay view `" + view_name + "` does not exist.");
  }

  const auto collection_info = *collection_it;
  const auto type = collection_info["type"];
  if (!type || type.type() != bsoncxx::type::k_string ||
      std::string_view{type.get_string().value} != "view") {
    throw std::runtime_error("Replay source `" + view_name + "` is not a MongoDB view.");
  }

  auto sample = database[view_name].find_one({});
  if (!sample) {
    return;
  }

  const auto row = sample->view();
  extract_timestamp(row);

  const auto data = row["data"];
  if (!data || data.type() != bsoncxx::type::k_document) {
    throw std::runtime_error("Replay view row is missing the `data` document.");
  }

  const auto source_collection = row["collection_name"];
  if (!source_collection || source_collection.type() != bsoncxx::type::k_string) {
    throw std::runtime_error("Replay view row is missing the `collection_name` field.");
  }
}

}  // namespace

namespace Mads {

// All MongoDB driver state lives here so that mongo_fetch.hpp stays free of
// bsoncxx/mongocxx includes and MongoFetch's layout is independent of the
// driver ABI.
struct MongoFetch::Impl {
  explicit Impl(const std::string &uri) : _uri_string(uri), _uri(uri) {}

  struct ReplayRow {
    std::chrono::milliseconds timestamp{0};
    std::string collection_name;
    nlohmann::json data;
  };

  void disconnect();
  std::size_t fetch_data(std::string &view_name);
  std::size_t fetch_data_from(const std::string &view_name);
  std::chrono::milliseconds load_next(nlohmann::json &out, std::string &collection_name);
  std::size_t activate_view(const std::string &view_name, bool owns_view);
  void reset_replay_stream();
  void drop_owned_view() noexcept;

  std::string _uri_string;
  mongocxx::uri _uri;
  std::optional<mongocxx::client> _client;
  std::string _database_name;
  std::vector<std::string> _collections;
  std::optional<std::chrono::milliseconds> _start_time;
  std::optional<std::chrono::milliseconds> _end_time;
  std::string _view_name;
  bool _owns_view{false};
  std::size_t _view_size{0};
  std::size_t _next_index{0};
  bool _repeat{false};
  std::optional<mongocxx::cursor> _cursor;
  std::optional<mongocxx::cursor::iterator> _cursor_it;
  std::optional<ReplayRow> _next_row;
  bool _unwrap_original{false};
};

MongoFetch::MongoFetch(const std::string &uri)
    : _impl(std::make_unique<Impl>(uri)) {}

MongoFetch::~MongoFetch() {
  if (_impl && _impl->_client) {
    _impl->disconnect();
  }
}

MongoFetch::MongoFetch(MongoFetch &&) noexcept = default;
MongoFetch &MongoFetch::operator=(MongoFetch &&) noexcept = default;

void MongoFetch::connect() {
  mongo_instance();
  _impl->_client.emplace(_impl->_uri);
}

void MongoFetch::disconnect() { _impl->disconnect(); }

void MongoFetch::Impl::disconnect() {
  _cursor.reset();
  _cursor_it.reset();
  _next_row.reset();
  drop_owned_view();
  _client.reset();
  _view_name.clear();
  _owns_view = false;
  _view_size = 0;
  _next_index = 0;
}

void MongoFetch::select_database(const std::string &db_name) {
  _impl->_database_name = db_name;
}

void MongoFetch::select_collections(const std::vector<std::string> &collections) {
  _impl->_collections = collections;
}

void MongoFetch::set_repeat(bool repeat) { _impl->_repeat = repeat; }

void MongoFetch::set_unwrap_original(bool unwrap) { _impl->_unwrap_original = unwrap; }

void MongoFetch::select_time_range(const std::string &start, const std::string &end) {
  _impl->_start_time = parse_time_string(start);
  _impl->_end_time = parse_time_string(end);

  if (!start.empty() && !_impl->_start_time) {
    throw std::runtime_error("Unable to parse start time: " + start);
  }
  if (!end.empty() && !_impl->_end_time) {
    throw std::runtime_error("Unable to parse end time: " + end);
  }
  if (_impl->_start_time && _impl->_end_time && *_impl->_start_time > *_impl->_end_time) {
    throw std::runtime_error("Start time must be less than or equal to end time.");
  }
}

void MongoFetch::select_time_range(const std::string &start) {
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()
  );
  select_time_range(start, std::to_string(now.count()));
}

std::size_t MongoFetch::fetch_data() {
  std::string generated_view_name;
  return fetch_data(generated_view_name);
}

std::size_t MongoFetch::fetch_data(const std::string &view_name) {
  std::string mutable_view_name = view_name;
  return fetch_data(mutable_view_name);
}

std::size_t MongoFetch::fetch_data(std::string &view_name) {
  return _impl->fetch_data(view_name);
}

std::size_t MongoFetch::fetch_data_from(const std::string &view_name) {
  return _impl->fetch_data_from(view_name);
}

std::chrono::milliseconds MongoFetch::load_next(
  nlohmann::json &out,
  std::string &collection_name
) {
  return _impl->load_next(out, collection_name);
}

std::size_t MongoFetch::Impl::fetch_data(std::string &view_name) {
  if (!_client) {
    throw std::runtime_error("MongoDB client is not connected.");
  }
  if (_database_name.empty()) {
    throw std::runtime_error("No database selected.");
  }
  if (_collections.empty()) {
    throw std::runtime_error("No collections selected.");
  }

  auto database = (*_client)[_database_name];
  const bool owns_view = view_name.empty();
  if (owns_view) {
    view_name = make_view_name();
  }

  drop_owned_view();

  if (database.has_collection(view_name)) {
    database[view_name].drop();
  }

  const auto pipeline = make_view_pipeline(_collections, _start_time, _end_time);
  database.create_collection(
    view_name,
    make_document(
      kvp("viewOn", _collections.front()),
      kvp("pipeline", pipeline.view())
    )
  );

  return activate_view(view_name, owns_view);
}

std::size_t MongoFetch::Impl::fetch_data_from(const std::string &view_name) {
  if (!_client) {
    throw std::runtime_error("MongoDB client is not connected.");
  }
  if (_database_name.empty()) {
    throw std::runtime_error("No database selected.");
  }
  if (view_name.empty()) {
    throw std::runtime_error("View name must not be empty.");
  }

  auto database = (*_client)[_database_name];
  validate_replay_view(database, view_name);

  drop_owned_view();
  return activate_view(view_name, false);
}

std::chrono::milliseconds MongoFetch::Impl::load_next(
  nlohmann::json &out,
  std::string &collection_name
) {
  out.clear();
  collection_name.clear();

  if (!_client) {
    throw std::runtime_error("MongoDB client is not connected.");
  }
  if (_database_name.empty() || _view_name.empty()) {
    return std::chrono::milliseconds{-1};
  }
  if (_view_size == 0) {
    return std::chrono::milliseconds{-1};
  }
  if (!_next_row) {
    if (!_repeat || _next_index < _view_size) {
      return std::chrono::milliseconds{-1};
    }
    reset_replay_stream();
    if (!_next_row) {
      return std::chrono::milliseconds{-1};
    }
  }

  ReplayRow current = std::move(*_next_row);
  _next_row.reset();
  ++_next_index;

  if (_cursor && _cursor_it) {
    auto cursor_end = _cursor->end();
    if (*_cursor_it != cursor_end) {
      auto next_row = parse_replay_row(**_cursor_it);
      ReplayRow replay_row;
      replay_row.timestamp = next_row.timestamp;
      replay_row.collection_name = std::move(next_row.collection_name);
      replay_row.data = std::move(next_row.data);
      _next_row = std::move(replay_row);
      ++(*_cursor_it);
    }
  }

  collection_name = current.collection_name;
  if (_unwrap_original && current.data.contains("message")) {
    out = current.data["message"];
  } else {
    out = current.data;
  }

  if (_next_row) {
    return _next_row->timestamp - current.timestamp;
  }

  if (_repeat) {
    _next_index = _view_size;
    return std::chrono::milliseconds{0};
  }

  return std::chrono::milliseconds{-1};
}

std::size_t MongoFetch::Impl::activate_view(const std::string &view_name, bool owns_view) {
  _view_name = view_name;
  _owns_view = owns_view;
  _next_index = 0;
  _view_size = (*_client)[_database_name][_view_name].count_documents({});
  reset_replay_stream();
  return _view_size;
}

void MongoFetch::Impl::reset_replay_stream() {
  _cursor.reset();
  _cursor_it.reset();
  _next_row.reset();

  if (!_client || _database_name.empty() || _view_name.empty()) {
    return;
  }

  mongocxx::options::find find_options;
  find_options.batch_size(128);

  auto view_collection = (*_client)[_database_name][_view_name];
  _cursor.emplace(view_collection.find({}, find_options));

  auto cursor_it = _cursor->begin();
  const auto cursor_end = _cursor->end();
  if (cursor_it == cursor_end) {
    return;
  }

  auto next_row = parse_replay_row(*cursor_it);
  ReplayRow replay_row;
  replay_row.timestamp = next_row.timestamp;
  replay_row.collection_name = std::move(next_row.collection_name);
  replay_row.data = std::move(next_row.data);
  _next_row = std::move(replay_row);
  ++cursor_it;
  _cursor_it = std::move(cursor_it);
}

void MongoFetch::Impl::drop_owned_view() noexcept {
  _cursor.reset();
  _cursor_it.reset();
  _next_row.reset();

  if (!_client || _database_name.empty() || _view_name.empty() || !_owns_view) {
    return;
  }

  try {
    auto database = (*_client)[_database_name];
    if (database.has_collection(_view_name)) {
      database[_view_name].drop();
    }
  } catch (...) {
  }

  _view_name.clear();
  _owns_view = false;
  _view_size = 0;
  _next_index = 0;
}

}  // namespace Mads
