// Tier-3 tests for Mads::Logger: the paths that only become reachable once the
// agent has actually received a message, so that Agent::status() is populated
// (src/agent.cpp:950, :976).
//
// Still no MongoDB server: the URI points at a closed port. What a real receive
// unlocks is close_db(), which iterates status() to create indexes. Tiers 1 and
// 2 in test_logger.cpp can never reach that loop because status() is empty
// there.
//
// Topology is the cross-connected pair from test_agent_pubsub.cpp: a publisher
// Agent binds with set_cross(true), and the Logger subscribes to it. Ports come
// from the spare 42400-42499 range documented in mads_test_helpers.hpp.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "logger.hpp"
#include "mads_test_helpers.hpp"

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

constexpr const char *kDeadUri =
    "mongodb://127.0.0.1:1/?serverSelectionTimeoutMS=50";

struct TempDir {
  fs::path path;
  explicit TempDir(const std::string &tag)
      : path(fs::temp_directory_path() / ("mads_logger_recv_" + tag)) {
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

fs::path write_settings(const fs::path &dir) {
  auto file = dir / "mads.ini";
  std::ofstream out(file);
  out << "[agents]\n"
      << "frontend_address = \"tcp://localhost:42410\"\n"
      << "backend_address = \"tcp://localhost:42411\"\n"
      << "\n[logger]\n"
      << "mongo_db = \"mads_unit_test\"\n"
      << "mongo_uri = \"" << kDeadUri << "\"\n"
      << "max_length = 75\n"
      << "initially_paused = false\n";
  return file;
}

// Publisher: binds (cross=true) at loopback(port), publishing only.
std::unique_ptr<Mads::Agent> make_pub(std::string name, uint16_t port) {
  auto a = std::make_unique<Mads::Agent>(name, "none");
  a->init(false, false);
  a->set_cross(true);
  a->set_sub_endpoint(mads_test::loopback(port));
  a->set_sub_topic({});
  a->connect(std::chrono::milliseconds(0));
  return a;
}

// Logger wired as a pure subscriber against the cross-connected publisher.
std::unique_ptr<Mads::Logger> make_logger_sub(const fs::path &settings,
                                              uint16_t port) {
  auto lg = std::make_unique<Mads::Logger>("logger", settings.string());
  lg->init(false, false);
  lg->set_sub_endpoint(mads_test::loopback(port));
  lg->set_pub_topic("");
  lg->set_sub_topic({""});
  lg->connect(std::chrono::milliseconds(0));
  return lg;
}

template <typename PublishFn, typename CheckFn>
bool retry_until(PublishFn publish_once, CheckFn check_received,
                 std::chrono::milliseconds total = 3000ms,
                 std::chrono::milliseconds settle = 150ms) {
  auto deadline = std::chrono::steady_clock::now() + total;
  while (std::chrono::steady_clock::now() < deadline) {
    publish_once();
    if (mads_test::wait_for(check_received, settle, 10ms))
      return true;
  }
  return false;
}

} // namespace

TEST_CASE("log() of a received message survives an unreachable server",
          "[logger][receive][mongo]") {
  if (!Mads::Logger::has_mongo_support()) {
    SKIP("build has no MongoDB support");
  }

  mads_test::RunningGuard guard;
  TempDir tmp("log_received");
  constexpr uint16_t port = 42420;

  auto pub = make_pub("pub", port);
  auto logger = make_logger_sub(write_settings(tmp.path), port);
  logger->set_file(false);
  logger->open_db();

  nlohmann::json payload;
  payload["value"] = 42;

  Mads::message_type type = Mads::message_type::none;
  REQUIRE(retry_until(
      [&] { pub->publish(payload, "topicA"); },
      [&] {
        type = logger->receive(true);
        return type == Mads::message_type::json;
      }));

  // Goes through last_json() -> log_doc_to_mongo(), not the explicit-tuple
  // path covered in test_logger.cpp.
  REQUIRE_NOTHROW(logger->log(type));
  REQUIRE_FALSE(logger->status().empty());

  REQUIRE_NOTHROW(logger->close_db());
}

TEST_CASE("close_db() after receiving survives an unreachable server",
          "[logger][receive][mongo]") {
  if (!Mads::Logger::has_mongo_support()) {
    SKIP("build has no MongoDB support");
  }

  mads_test::RunningGuard guard;
  TempDir tmp("close_after_receive");
  constexpr uint16_t port = 42421;

  auto pub = make_pub("pub", port);
  auto logger = make_logger_sub(write_settings(tmp.path), port);
  logger->set_file(false);
  logger->open_db();

  nlohmann::json payload;
  payload["value"] = 7;

  REQUIRE(retry_until(
      [&] { pub->publish(payload, "topicA"); },
      [&] { return logger->receive(true) == Mads::message_type::json; }));

  // status() is now non-empty, so close_db() reaches its create_index() loop.
  // Those calls hit the unreachable server; the failure must be reported, not
  // propagated -- ~Logger() also calls close_db(), and an exception escaping a
  // destructor terminates the process.
  REQUIRE_FALSE(logger->status().empty());
  REQUIRE_NOTHROW(logger->close_db());
}

TEST_CASE("destroying a Logger that received messages does not terminate",
          "[logger][receive][mongo]") {
  if (!Mads::Logger::has_mongo_support()) {
    SKIP("build has no MongoDB support");
  }

  mads_test::RunningGuard guard;
  TempDir tmp("destructor");
  constexpr uint16_t port = 42422;

  auto pub = make_pub("pub", port);

  nlohmann::json payload;
  payload["value"] = 1;

  {
    auto logger = make_logger_sub(write_settings(tmp.path), port);
    logger->set_file(false);
    logger->open_db();

    REQUIRE(retry_until(
        [&] { pub->publish(payload, "topicA"); },
        [&] { return logger->receive(true) == Mads::message_type::json; }));
    REQUIRE_FALSE(logger->status().empty());

    // No explicit close_db(): the destructor must handle it on its own.
  }

  SUCCEED("Logger destructor completed without terminating");
}
