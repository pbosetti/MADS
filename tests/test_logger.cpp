// Unit tests for Mads::Logger (src/logger.hpp, src/logger.cpp) that never
// contact a MongoDB server.
//
// Two tiers live here:
//
//  - Tier 1 exercises everything that does not involve the driver at all:
//    settings loading, message truncation, the file-logging paths, the pause
//    gate, and the open/close state machine. These run identically whether or
//    not MADS was built with MADS_ENABLE_MONGOCXX.
//
//  - Tier 2 exercises the driver without a server. `mongocxx::client`
//    construction is lazy -- nothing connects until an operation runs -- so
//    pointing the URI at a closed port exercises connect/insert error handling
//    with no infrastructure. The property under test is that the logger
//    *survives* MongoDB being unreachable rather than propagating.
//
// The receive path (Agent::status() populated by real messages) needs a
// cross-connected agent pair and lives in test_logger_receive.cpp.
//
// Neither tier needs a broker: Logger is constructed against a local TOML file
// and init(false, false) skips the network, exactly as test_agent_settings.cpp
// does.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <tuple>

#include "logger.hpp"
#include "mads_test_helpers.hpp"

#ifdef MADS_HAS_MONGOCXX
#include "mongo_fetch.hpp"
#endif

namespace fs = std::filesystem;

namespace {

// Syntactically valid but unconnectable: port 1 on loopback is closed, and the
// short server-selection timeout keeps each failure to well under a second.
constexpr const char *kDeadUri =
    "mongodb://127.0.0.1:1/?serverSelectionTimeoutMS=50";

// Per-test scratch directory, removed on scope exit.
struct TempDir {
  fs::path path;
  explicit TempDir(const std::string &tag)
      : path(fs::temp_directory_path() / ("mads_logger_test_" + tag)) {
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

// Writes a settings file with a [logger] section, mirroring the shape of
// tests/fixtures/settings/valid.toml. Generated rather than checked in so that
// max_length / initially_paused can vary per test.
fs::path write_settings(const fs::path &dir, int max_length = 75,
                        bool initially_paused = false,
                        const std::string &uri = kDeadUri) {
  auto file = dir / "mads.ini";
  std::ofstream out(file);
  out << "[agents]\n"
      << "frontend_address = \"tcp://localhost:42410\"\n"
      << "backend_address = \"tcp://localhost:42411\"\n"
      << "\n[logger]\n"
      << "mongo_db = \"mads_unit_test\"\n"
      << "mongo_uri = \"" << uri << "\"\n"
      << "max_length = " << max_length << "\n"
      << "initially_paused = " << (initially_paused ? "true" : "false") << "\n";
  return file;
}

// Redirects std::cerr for the lifetime of the object so warnings are assertable.
struct CerrCapture {
  std::ostringstream captured;
  std::streambuf *previous;
  CerrCapture() : previous(std::cerr.rdbuf(captured.rdbuf())) {}
  ~CerrCapture() { std::cerr.rdbuf(previous); }
  std::string str() const { return captured.str(); }
};

std::string read_file(const fs::path &path) {
  std::ifstream in(path);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::tuple<std::string, std::string> msg(std::string topic,
                                         std::string payload) {
  return std::make_tuple(std::move(topic), std::move(payload));
}

} // namespace

// ---------------------------------------------------------------------------
// Tier 1: build-independent behaviour (no driver involved)
// ---------------------------------------------------------------------------

TEST_CASE("Logger logs one JSON object per line by default", "[logger]") {
  mads_test::RunningGuard guard;
  TempDir tmp("per_line");
  auto log_path = tmp.path / "out.jsonl";

  {
    Mads::Logger logger("logger", write_settings(tmp.path).string());
    logger.init(false, false);
    logger.set_mongo(false);
    logger.set_file(log_path.string(), false);
    logger.open_db();

    auto first = msg("topicA", R"({"a":1})");
    auto second = msg("topicB", R"({"b":2})");
    logger.log(&first);
    logger.log(&second);
    logger.close_db();
  }

  REQUIRE(read_file(log_path) ==
          "{\"topicA\":{\"a\":1}}\n{\"topicB\":{\"b\":2}}\n");
}

TEST_CASE("Logger array mode emits a well-formed JSON array", "[logger]") {
  mads_test::RunningGuard guard;
  TempDir tmp("array");
  auto log_path = tmp.path / "out.json";

  {
    Mads::Logger logger("logger", write_settings(tmp.path).string());
    logger.init(false, false);
    logger.set_mongo(false);
    logger.set_file(log_path.string(), true);
    logger.open_db();

    auto first = msg("topicA", R"({"a":1})");
    auto second = msg("topicB", R"({"b":2})");
    logger.log(&first);
    logger.log(&second);
    logger.close_db();
  }

  // close_log_file() rewinds two bytes to overwrite the final ",\n" separator.
  REQUIRE(read_file(log_path) ==
          "[\n{\"topicA\":{\"a\":1}},\n{\"topicB\":{\"b\":2}}\n]\n");
}

// Characterisation test, not an endorsement: with no records the two-byte
// rewind in close_log_file() lands on offset 0 and overwrites the opening "[",
// so the result is not valid JSON. Pinned so a future fix is a deliberate,
// visible change rather than a silent one.
TEST_CASE("Logger array mode with no records overwrites the opening bracket",
          "[logger][characterisation]") {
  mads_test::RunningGuard guard;
  TempDir tmp("array_empty");
  auto log_path = tmp.path / "out.json";

  {
    Mads::Logger logger("logger", write_settings(tmp.path).string());
    logger.init(false, false);
    logger.set_mongo(false);
    logger.set_file(log_path.string(), true);
    logger.open_db();
    logger.close_db();
  }

  REQUIRE(read_file(log_path) == "\n]\n");
}

TEST_CASE("Logger writes nothing while paused", "[logger]") {
  mads_test::RunningGuard guard;
  TempDir tmp("paused");
  auto log_path = tmp.path / "out.jsonl";

  {
    Mads::Logger logger("logger", write_settings(tmp.path).string());
    logger.init(false, false);
    logger.set_mongo(false);
    logger.set_file(log_path.string(), false);
    logger.open_db();

    logger.paused = true;
    auto dropped = msg("topicA", R"({"a":1})");
    logger.log(&dropped);

    logger.paused = false;
    auto kept = msg("topicB", R"({"b":2})");
    logger.log(&kept);
    logger.close_db();
  }

  REQUIRE(read_file(log_path) == "{\"topicB\":{\"b\":2}}\n");
}

TEST_CASE("initially_paused from settings is applied by load_settings",
          "[logger]") {
  mads_test::RunningGuard guard;
  TempDir tmp("initially_paused");

  Mads::Logger logger("logger",
                      write_settings(tmp.path, 75, true).string());
  REQUIRE_FALSE(logger.paused); // default before init()
  logger.init(false, false);
  REQUIRE(logger.paused); // load_settings() ran during init()
}

// Characterisation tests for truncated_message(). The implementation reads
//   if (message.length() > _max_length - 3) return message.substr(0, _max_length) + "...";
// which has two quirks worth pinning:
//   1. the returned string can be max_length + 3 characters long, so
//      max_length is not actually an upper bound;
//   2. a message shorter than max_length can still take the truncating branch
//      and gain an ellipsis it did not need.
TEST_CASE("truncated_message honours max_length from settings", "[logger]") {
  mads_test::RunningGuard guard;
  TempDir tmp("truncate");

  Mads::Logger logger("logger", write_settings(tmp.path, 10).string());
  logger.init(false, false);

  // Short enough to be left alone: 6 <= 10 - 3.
  REQUIRE(logger.truncated_message("012345") == "012345");

  // Longer than max_length: truncated to 10 characters plus the ellipsis, so
  // the result is 13 characters -- longer than the configured maximum.
  REQUIRE(logger.truncated_message("0123456789ABCDEF") == "0123456789...");

  // Shorter than max_length but past the (max_length - 3) threshold: substr()
  // returns the whole string and the ellipsis is appended anyway.
  REQUIRE(logger.truncated_message("01234567") == "01234567...");
}

// size_t underflow: for max_length < 3, `_max_length - 3` wraps to SIZE_MAX and
// the truncating branch becomes unreachable.
TEST_CASE("truncated_message never truncates when max_length is below three",
          "[logger][characterisation]") {
  mads_test::RunningGuard guard;
  TempDir tmp("truncate_underflow");

  Mads::Logger logger("logger", write_settings(tmp.path, 2).string());
  logger.init(false, false);

  const std::string long_message(500, 'x');
  REQUIRE(logger.truncated_message(long_message) == long_message);
}

TEST_CASE("open_db() and close_db() are idempotent", "[logger]") {
  mads_test::RunningGuard guard;
  TempDir tmp("idempotent");
  auto log_path = tmp.path / "out.jsonl";

  {
    Mads::Logger logger("logger", write_settings(tmp.path).string());
    logger.init(false, false);
    logger.set_mongo(false);
    logger.set_file(log_path.string(), false);

    logger.open_db();
    auto first = msg("topicA", R"({"a":1})");
    logger.log(&first);
    // Second open must not reopen (and so truncate) the log file.
    REQUIRE_NOTHROW(logger.open_db());
    auto second = msg("topicB", R"({"b":2})");
    logger.log(&second);

    REQUIRE_NOTHROW(logger.close_db());
    REQUIRE_NOTHROW(logger.close_db());
  }

  REQUIRE(read_file(log_path) ==
          "{\"topicA\":{\"a\":1}}\n{\"topicB\":{\"b\":2}}\n");
}

TEST_CASE("open_db() warns when no logging destination is enabled",
          "[logger]") {
  mads_test::RunningGuard guard;
  TempDir tmp("no_destination");

  Mads::Logger logger("logger", write_settings(tmp.path).string());
  logger.init(false, false);
  logger.set_mongo(false);
  logger.set_file(false);

  CerrCapture cerr_capture;
  logger.open_db();
  logger.close_db();

  REQUIRE(cerr_capture.str().find("no logging destination") !=
          std::string::npos);
}

TEST_CASE("set_file(name) enables file logging, set_file(false) disables it",
          "[logger]") {
  mads_test::RunningGuard guard;
  TempDir tmp("set_file");
  auto log_path = tmp.path / "out.jsonl";

  Mads::Logger logger("logger", write_settings(tmp.path).string());
  logger.init(false, false);
  logger.set_mongo(false);
  logger.set_file(log_path.string(), false);
  logger.set_file(false); // disable again before opening

  CerrCapture cerr_capture;
  logger.open_db();
  logger.close_db();

  // Neither destination is active, so the warning fires and no file is created.
  REQUIRE(cerr_capture.str().find("no logging destination") !=
          std::string::npos);
  REQUIRE_FALSE(fs::exists(log_path));
}

TEST_CASE("info() reports the configured destinations", "[logger]") {
  mads_test::RunningGuard guard;
  TempDir tmp("info");
  auto log_path = tmp.path / "out.jsonl";

  Mads::Logger logger("logger", write_settings(tmp.path).string());
  logger.init(false, false);
  logger.set_mongo(false);
  logger.set_file(log_path.string(), true);

  std::ostringstream out;
  logger.info(out);
  const std::string text = out.str();

  REQUIRE(text.find(log_path.string()) != std::string::npos);
  REQUIRE(text.find("Log file is an array: yes") != std::string::npos);
  if (!Mads::Logger::has_mongo_support()) {
    REQUIRE(text.find("not compiled in") != std::string::npos);
  }
}

TEST_CASE("has_mongo_support() agrees with the build configuration",
          "[logger]") {
#ifdef MADS_HAS_MONGOCXX
  REQUIRE(Mads::Logger::has_mongo_support());
#else
  REQUIRE_FALSE(Mads::Logger::has_mongo_support());
#endif
}

TEST_CASE("set_mongo(true) is refused on a build without driver support",
          "[logger]") {
  if (Mads::Logger::has_mongo_support()) {
    SKIP("build has MongoDB support; covered by the tier-2 cases");
  }

  mads_test::RunningGuard guard;
  TempDir tmp("no_driver");

  Mads::Logger logger("logger", write_settings(tmp.path).string());
  logger.init(false, false);

  CerrCapture cerr_capture;
  logger.set_mongo(true);
  REQUIRE(cerr_capture.str().find("no MongoDB support") != std::string::npos);

  // The request was ignored, so info() still reports the unavailable driver
  // rather than a URI.
  std::ostringstream out;
  logger.info(out);
  REQUIRE(out.str().find("not compiled in") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Tier 2: driver exercised against an unreachable server
// ---------------------------------------------------------------------------

TEST_CASE("open_db() does not block or throw when the server is unreachable",
          "[logger][mongo]") {
  if (!Mads::Logger::has_mongo_support()) {
    SKIP("build has no MongoDB support");
  }

  mads_test::RunningGuard guard;
  TempDir tmp("unreachable_open");

  Mads::Logger logger("logger", write_settings(tmp.path).string());
  logger.init(false, false);
  logger.set_file(false);

  // mongocxx::client construction is lazy: no I/O happens until an operation.
  REQUIRE_NOTHROW(logger.open_db());
  REQUIRE_NOTHROW(logger.close_db());
}

TEST_CASE("log() reports but does not propagate an unreachable server",
          "[logger][mongo]") {
  if (!Mads::Logger::has_mongo_support()) {
    SKIP("build has no MongoDB support");
  }

  mads_test::RunningGuard guard;
  TempDir tmp("unreachable_log");

  Mads::Logger logger("logger", write_settings(tmp.path).string());
  logger.init(false, false);
  logger.set_file(false);
  logger.open_db();

  auto payload = msg("topicA", R"({"a":1})");
  CerrCapture cerr_capture;
  REQUIRE_NOTHROW(logger.log(&payload));
  REQUIRE(cerr_capture.str().find("Error while inserting document") !=
          std::string::npos);

  REQUIRE_NOTHROW(logger.close_db());
}

TEST_CASE("register_event() does not propagate an unreachable server",
          "[logger][mongo]") {
  if (!Mads::Logger::has_mongo_support()) {
    SKIP("build has no MongoDB support");
  }

  mads_test::RunningGuard guard;
  TempDir tmp("unreachable_event");

  Mads::Logger logger("logger", write_settings(tmp.path).string());
  logger.init(false, false);
  logger.set_file(false);
  logger.open_db();

  CerrCapture cerr_capture;
  REQUIRE_NOTHROW(logger.register_event(Mads::event_type::startup));
  REQUIRE_NOTHROW(logger.close_db());
}

// The mongocxx::instance is a per-process singleton that throws if constructed
// twice. It used to be a Logger member, which made these two cases impossible;
// it is now shared through Mads::detail::mongo_instance().
TEST_CASE("two Logger instances can connect in the same process",
          "[logger][mongo]") {
  if (!Mads::Logger::has_mongo_support()) {
    SKIP("build has no MongoDB support");
  }

  mads_test::RunningGuard guard;
  TempDir tmp("two_loggers");
  const auto settings = write_settings(tmp.path).string();

  Mads::Logger first("logger", settings);
  first.init(false, false);
  first.set_file(false);
  REQUIRE_NOTHROW(first.open_db());

  Mads::Logger second("logger", settings);
  second.init(false, false);
  second.set_file(false);
  REQUIRE_NOTHROW(second.open_db());

  REQUIRE_NOTHROW(first.close_db());
  REQUIRE_NOTHROW(second.close_db());
}

#ifdef MADS_HAS_MONGOCXX
TEST_CASE("a Logger and a MongoFetch can coexist in the same process",
          "[logger][mongo]") {
  mads_test::RunningGuard guard;
  TempDir tmp("logger_and_fetch");

  Mads::MongoFetch fetcher{kDeadUri};
  REQUIRE_NOTHROW(fetcher.connect());

  Mads::Logger logger("logger", write_settings(tmp.path).string());
  logger.init(false, false);
  logger.set_file(false);
  REQUIRE_NOTHROW(logger.open_db());
  REQUIRE_NOTHROW(logger.close_db());
}
#endif
