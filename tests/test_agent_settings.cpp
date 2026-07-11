// Unit tests for Mads::Agent settings loading from local TOML files
// (src/agent.cpp: Agent::init(), Agent::load_settings(), Agent::save_settings(),
// Agent::settings_are_local(), Agent::settings_uri(), Agent::get_settings()).
//
// These tests never touch the network: `settings_uri` is always either "none"
// (documented test mode, src/agent.hpp:232) or a local filesystem path, so
// `settings_are_local()` (src/agent.cpp:1122-1124, "no tcp://" in the URI) is
// always true here. The broker (tcp://) path is covered by
// test_agent_broker.cpp.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>

#include "agent.hpp"
#include "mads_test_helpers.hpp"

using namespace std::chrono_literals;

namespace {

std::string fixture(const std::string &name) {
  return std::string(MADS_TEST_FIXTURES_DIR) + "/settings/" + name;
}

} // namespace

// ---------------------------------------------------------------------------
// init() from a local TOML file
// ---------------------------------------------------------------------------

TEST_CASE("init() from a local TOML file loads settings, endpoints, and "
          "fleet-wide defaults",
          "[agent_settings]") {
  mads_test::RunningGuard guard;
  std::string path = fixture("valid.toml");
  Mads::Agent a("settingstest", path);
  a.init(false, false);

  REQUIRE(a.settings_are_local());
  REQUIRE(a.settings_uri() == path);

  // Fleet-wide [agents] defaults, inherited since not overridden per-agent.
  REQUIRE(a.pub_endpoint() == "tcp://localhost:42210");
  REQUIRE(a.sub_endpoint() == "tcp://localhost:42211");
  REQUIRE(a.timecode_fps == Catch::Approx(25.0));
  REQUIRE(a.wire_format() == Mads::WireFormat::Json);
  REQUIRE(a.compression() == Mads::Compression::Auto);

  // Per-agent section.
  REQUIRE(a.pub_topic() == "settingstest_pub");
  REQUIRE(a.sub_topic() == std::vector<std::string>{"topicA", "topicB"});

  nlohmann::json j = a.get_settings();
  REQUIRE(j.at("pub_topic") == "settingstest_pub");
  REQUIRE(j.at("sub_topic") == nlohmann::json::array({"topicA", "topicB"}));
  REQUIRE(j.at("time_step") == 100);
  REQUIRE(j.at("queue_size") == 5);
  REQUIRE(j.at("custom_string") == "custom_value");
  REQUIRE(j.at("custom_int") == 42);
  REQUIRE(j.at("custom_bool") == true);
  REQUIRE(j.at("custom_double").get<double>() == Catch::Approx(3.14));

  // No settings were read from a broker.
  REQUIRE(a.attachment_path().empty());
}

TEST_CASE("init() accepts sub_topic as a single string", "[agent_settings]") {
  mads_test::RunningGuard guard;
  Mads::Agent a("strsub", fixture("string_subtopic.toml"));
  a.init(false, false);

  REQUIRE(a.sub_topic() == std::vector<std::string>{"onlytopic"});
  REQUIRE(a.get_settings().at("sub_topic") == "onlytopic");
}

TEST_CASE("init() leaves sub_topic empty when the key is absent",
          "[agent_settings]") {
  mads_test::RunningGuard guard;
  Mads::Agent a("nosub", fixture("no_subtopic.toml"));
  a.init(false, false);

  REQUIRE(a.sub_topic().empty());
}

// ---------------------------------------------------------------------------
// Error paths
// ---------------------------------------------------------------------------

TEST_CASE("init() from a nonexistent settings file throws toml::parse_error",
          "[agent_settings]") {
  mads_test::RunningGuard guard;
  Mads::Agent a("nofile", fixture("does_not_exist.toml"));
  REQUIRE_THROWS_AS(a.init(false, false), toml::parse_error);
}

TEST_CASE("init() from a malformed TOML file throws toml::parse_error",
          "[agent_settings]") {
  mads_test::RunningGuard guard;
  Mads::Agent a("badcfg", fixture("malformed.toml"));
  REQUIRE_THROWS_AS(a.init(false, false), toml::parse_error);
}

TEST_CASE("init() throws AgentError when no section matches the agent name",
          "[agent_settings]") {
  mads_test::RunningGuard guard;
  Mads::Agent a("missingsection", fixture("missing_section.toml"));
  bool threw = false;
  try {
    a.init(false, false);
  } catch (const Mads::AgentError &e) {
    threw = true;
    std::string msg = e.what();
    REQUIRE(msg.find("missing") != std::string::npos);
    REQUIRE(msg.find("missingsection") != std::string::npos);
  }
  REQUIRE(threw);
}

TEST_CASE("save_settings() throws AgentError when settings are local",
          "[agent_settings]") {
  mads_test::RunningGuard guard;
  Mads::Agent a("settingstest", fixture("valid.toml"));
  a.init(false, false);

  auto tmp = std::filesystem::temp_directory_path() /
             "mads_test_settings_save_local.ini";
  REQUIRE_THROWS_AS(a.save_settings(tmp.string()), Mads::AgentError);
}

// ---------------------------------------------------------------------------
// Timeout accessors: guarded by _init_done (set_settings_timeout) or not
// (set_receive_timeout), verbatim from src/agent.cpp.
// ---------------------------------------------------------------------------

TEST_CASE("settings/receive timeout accessors: defaults and guards",
          "[agent_settings]") {
  mads_test::RunningGuard guard;
  Mads::Agent a("settingstest", fixture("valid.toml"));

  // Defaults, before init().
  REQUIRE(a.settings_timeout() == 0);
  REQUIRE(a.receive_timeout() == Mads::DEFAULT_RECEIVE_TIMEOUT_MS);

  // set_settings_timeout() is allowed before init()...
  a.set_settings_timeout(1234);
  REQUIRE(a.settings_timeout() == 1234);
  a.set_settings_timeout(std::chrono::milliseconds(50));
  REQUIRE(a.settings_timeout() == 50);

  // set_receive_timeout() has no _init_done guard in the source: it is legal
  // to call at any time, and immediately reflects in the getter.
  a.set_receive_timeout(777);
  REQUIRE(a.receive_timeout() == 777);
  a.set_receive_timeout(std::chrono::milliseconds(321));
  REQUIRE(a.receive_timeout() == 321);

  a.init(false, false);

  // ...but throws once initialized.
  REQUIRE_THROWS_AS(a.set_settings_timeout(999), Mads::AgentError);
  REQUIRE_THROWS_AS(a.set_settings_timeout(std::chrono::milliseconds(999)),
                    Mads::AgentError);

  // set_receive_timeout() still has no guard post-init().
  a.set_receive_timeout(444);
  REQUIRE(a.receive_timeout() == 444);
}

// ---------------------------------------------------------------------------
// "none" settings URI (documented test mode, src/agent.hpp:232): compare
// against a local-file equivalent to pin down the synthesized defaults
// (src/agent.cpp:365-371).
// ---------------------------------------------------------------------------

TEST_CASE("settings_uri \"none\" synthesizes minimal defaults",
          "[agent_settings]") {
  mads_test::RunningGuard guard;
  Mads::Agent a("noneagent", "none");
  a.init(false, false);

  REQUIRE(a.settings_are_local());
  REQUIRE(a.settings_uri() == "none");
  REQUIRE(a.pub_topic() == "noneagent");
  REQUIRE(a.sub_topic() == std::vector<std::string>{""});
  REQUIRE(a.get_settings().at("pub_topic") == "noneagent");
}
