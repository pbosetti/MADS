// Unit tests for Mads::AgentApp / AgentAppT (src/agent_app.hpp): CLI option
// plumbing, standard early-exit options (--help/--version/--save-settings),
// settings application helpers, and the events-enabled connect/disconnect
// flow. Port range: 42440-42460.
//
// Deliberately out of scope (documented, untestable in-process):
// - print_parse_error_and_exit() calls std::exit(EXIT_FAILURE);
// - restart_if_requested()'s true path calls execvp (replaces the process);
// - the --room branch performs real UDP service discovery.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <zmqpp/zmqpp.hpp>

#include "agent_app.hpp"
#include "mads_test_helpers.hpp"

using namespace std::chrono_literals;

namespace {

const std::string FIXTURE =
    std::string(MADS_TEST_FIXTURES_DIR) + "/agent_app/app.toml";

// Owns writable argv storage for cxxopts::parse(argc, argv).
struct Argv {
  explicit Argv(std::vector<std::string> args) : strings(std::move(args)) {
    for (auto &s : strings) ptrs.push_back(s.data());
    ptrs.push_back(nullptr);
  }
  int argc() { return static_cast<int>(strings.size()); }
  char **argv() { return ptrs.data(); }
  std::vector<std::string> strings;
  std::vector<char *> ptrs;
};

// Same minimal REP settings broker as tests/test_agent_broker.cpp, bound in
// this suite's port range (that file must not be modified).
class FakeBroker {
public:
  explicit FakeBroker(uint16_t port, std::string body)
      : _body(std::move(body)), _ctx(), _sock(_ctx, zmqpp::socket_type::rep) {
    _sock.set(zmqpp::socket_option::receive_timeout, 100);
    _sock.bind(mads_test::loopback(port));
    _thread = std::thread([this] { run(); });
  }
  ~FakeBroker() {
    _stopped = true;
    if (_thread.joinable()) _thread.join();
  }

private:
  void run() {
    while (!_stopped) {
      zmqpp::message msg;
      if (!_sock.receive(msg)) continue;
      if (msg.parts() < 2) continue;
      std::string kind = msg.get(1);
      zmqpp::message reply;
      if (kind == "settings") {
        reply << std::string(LIB_VERSION) << _body;
      } else if (kind == "timecode") {
        reply << std::string("0.0");
      } else {
        reply << std::string(LIB_VERSION) << std::string("{}");
      }
      _sock.send(reply);
    }
  }

  std::string _body;
  zmqpp::context _ctx;
  zmqpp::socket _sock;
  std::thread _thread;
  std::atomic<bool> _stopped{false};
};

std::string broker_toml(const std::string &name) {
  return "[agents]\ntimecode_fps = 25\n\n[" + name + "]\npub_topic = \"" +
         name + "_pub\"\n";
}

} // namespace

TEST_CASE("init(parsed) loads settings from -s and applies -n and -i",
          "[agent_app]") {
  mads_test::RunningGuard guard;
  Mads::AgentApp app{"mads-appx", "none"};
  app.add_common_options();
  app.add_agent_identity_options();
  app.add_dont_block_option();
  app.add_queue_size_option();

  Argv cli({"mads-appx", "-s", FIXTURE, "-n", "prefix-appx", "-i", "unit42"});
  auto &parsed = app.parse_options(cli.argc(), cli.argv());
  REQUIRE(parsed.count("settings") == 1);

  app.init(parsed, "none", /*install_watchdog=*/false);
  REQUIRE(app.name() == "appx"); // -n drops the part before the last '-'
  REQUIRE(app.get_agent_id() == "unit42");
  REQUIRE(app.settings_are_local());
  REQUIRE(app.settings_json().at("pub_topic") == "appx_pub");
}

TEST_CASE("apply_receive_timeout and apply_queue_size read the cached "
          "settings",
          "[agent_app]") {
  mads_test::RunningGuard guard;
  Mads::AgentApp app{"mads-appx", "none"};
  app.add_common_options();
  app.add_queue_size_option();
  Argv cli({"mads-appx", "-s", FIXTURE});
  app.init(app.parse_options(cli.argc(), cli.argv()), "none", false);

  app.apply_receive_timeout();
  REQUIRE(app.receive_timeout() == 350);

  app.apply_queue_size(); // no -q: falls back to settings queue_size
  REQUIRE(app.high_watermark() == 42);
}

TEST_CASE("apply_queue_size prefers the -q CLI option over settings",
          "[agent_app]") {
  mads_test::RunningGuard guard;
  Mads::AgentApp app{"mads-appx", "none"};
  app.add_common_options();
  app.add_queue_size_option();
  Argv cli({"mads-appx", "-s", FIXTURE, "-q", "7"});
  app.init(app.parse_options(cli.argc(), cli.argv()), "none", false);

  app.apply_queue_size();
  REQUIRE(app.high_watermark() == 7);
}

TEST_CASE("set_agent_name drops path and command prefix", "[agent_app]") {
  mads_test::RunningGuard guard;
  Mads::AgentApp app{"mads-appx", "none"};
  app.set_agent_name("/usr/local/bin/mads-renamed");
  REQUIRE(app.name() == "renamed");
  app.set_agent_name("plainname");
  REQUIRE(app.name() == "plainname");
}

TEST_CASE("handle_standard_exit_options handles --help and --version",
          "[agent_app]") {
  mads_test::RunningGuard guard;
  Mads::AgentApp app{"mads-appx", "none"};
  app.add_common_options();

  SECTION("--help prints usage and exits successfully") {
    Argv cli({"mads-appx", "--help"});
    auto &parsed = app.parse_options(cli.argc(), cli.argv());
    std::ostringstream out, err;
    int rc = Mads::AgentApp::handle_standard_exit_options<Mads::AgentApp>(
        parsed, app.raw_options(), cli.argv(), "none", out, err);
    REQUIRE(rc == EXIT_SUCCESS);
    REQUIRE(out.str().find("Print usage") != std::string::npos);
  }

  SECTION("--version prints the library version") {
    Argv cli({"mads-appx", "--version"});
    auto &parsed = app.parse_options(cli.argc(), cli.argv());
    std::ostringstream out, err;
    int rc = Mads::AgentApp::handle_standard_exit_options<Mads::AgentApp>(
        parsed, app.raw_options(), cli.argv(), "none", out, err);
    REQUIRE(rc == EXIT_SUCCESS);
    REQUIRE(out.str().find(LIB_VERSION) != std::string::npos);
  }

  SECTION("no early-exit option returns -1") {
    Argv cli({"mads-appx"});
    auto &parsed = app.parse_options(cli.argc(), cli.argv());
    std::ostringstream out, err;
    int rc = Mads::AgentApp::handle_standard_exit_options<Mads::AgentApp>(
        parsed, app.raw_options(), cli.argv(), "none", out, err);
    REQUIRE(rc == -1);
  }
}

TEST_CASE("the save-settings option writes broker-served settings to a file",
          "[agent_app]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42441;
  const std::string body = broker_toml("appsave");
  FakeBroker broker(port, body);

  auto out_path = std::filesystem::temp_directory_path() /
                  "mads_agent_app_saved.toml";
  std::filesystem::remove(out_path);

  Mads::AgentApp app{"mads-appsave", "none"};
  app.add_common_options();
  Argv cli({"mads-appsave", "-s", mads_test::loopback(port),
            "--settings-timeout", "2000", "-S", out_path.string()});
  auto &parsed = app.parse_options(cli.argc(), cli.argv());
  std::ostringstream out, err;
  int rc = Mads::AgentApp::handle_standard_exit_options<Mads::AgentApp>(
      parsed, app.raw_options(), cli.argv(), "none", out, err);
  REQUIRE(rc == EXIT_SUCCESS);
  REQUIRE(out.str().find("Settings saved") != std::string::npos);

  std::ifstream ifs(out_path);
  std::stringstream saved;
  saved << ifs.rdbuf();
  REQUIRE(saved.str() == body);
  std::filesystem::remove(out_path);
}

TEST_CASE("the save-settings option with local settings reports an error",
          "[agent_app]") {
  mads_test::RunningGuard guard;
  Mads::AgentApp app{"mads-appx", "none"};
  app.add_common_options();
  Argv cli({"mads-appx", "-s", FIXTURE, "-S", "/tmp/should_not_exist.toml"});
  auto &parsed = app.parse_options(cli.argc(), cli.argv());
  std::ostringstream out, err;
  int rc = Mads::AgentApp::handle_standard_exit_options<Mads::AgentApp>(
      parsed, app.raw_options(), cli.argv(), "none", out, err);
  REQUIRE(rc == EXIT_FAILURE);
  REQUIRE(err.str().find("Error saving") != std::string::npos);
}

TEST_CASE("init(parsed) against a broker prints and applies the settings "
          "timeout",
          "[agent_app]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42442;
  FakeBroker broker(port, broker_toml("appbrk"));

  Mads::AgentApp app{"mads-appbrk", "none"};
  app.add_common_options();
  Argv cli({"mads-appbrk", "-s", mads_test::loopback(port),
            "--settings-timeout", "2000"});
  app.init(app.parse_options(cli.argc(), cli.argv()), "none", false);
  REQUIRE(app.settings_timeout() == 2000);
  REQUIRE_FALSE(app.settings_are_local());
  REQUIRE(app.settings_json().at("pub_topic") == "appbrk_pub");
}

TEST_CASE("crypto CLI options are propagated to the agent before init",
          "[agent_app]") {
  mads_test::RunningGuard guard;
  auto key_dir = std::filesystem::temp_directory_path() / "mads_app_nokeys";
  std::filesystem::create_directories(key_dir);

  Mads::AgentApp app{"mads-appx", "none"};
  app.add_common_options();
  // keys_dir/key_broker/key_client declare implicit values, so cxxopts only
  // accepts the --option=value spelling for them.
  Argv cli({"mads-appx", "-s", FIXTURE, "--crypto",
            "--keys_dir=" + key_dir.string(), "--key_broker=srv",
            "--key_client=cli", "--auth_verbose"});
  auto &parsed = app.parse_options(cli.argc(), cli.argv());
  // configure_from_cli_options() runs before Agent::init(), which then fails
  // because the key files do not exist — the CLI->agent plumbing is what
  // this test covers.
  REQUIRE_THROWS(app.init(parsed, "none", false));
  REQUIRE(app.client_key_name == "cli");
  REQUIRE(app.server_key_name == "srv");

  std::filesystem::remove_all(key_dir);
}

TEST_CASE("restart_if_requested returns false when no restart is pending",
          "[agent_app]") {
  mads_test::RunningGuard guard;
  Mads::AgentApp app{"mads-appx", "none"};
  Argv cli({"mads-appx"});
  std::ostringstream out;
  REQUIRE_FALSE(app.restart_if_requested(cli.argv(), out));
  REQUIRE(out.str().empty());
}

TEST_CASE("enable_events registers startup and shutdown events over "
          "connect/disconnect",
          "[agent_app]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42443;

  Mads::AgentApp app{"mads-appevents", "none"};
  app.add_common_options();
  Argv cli({"mads-appevents", "-s", FIXTURE});
  app.init(app.parse_options(cli.argc(), cli.argv()), "none", false);
  app.set_cross(true);
  app.set_sub_endpoint(mads_test::loopback(port));

  Mads::Agent sub("appevents_watch", "none");
  sub.init(false, false);
  sub.set_sub_endpoint(mads_test::loopback(port));
  sub.set_pub_topic("");
  sub.set_sub_topic({METADATA_TOPIC});

  app.enable_events();
  app.connect(0ms); // registers the (delayed) startup event
  sub.connect(0ms);

  bool got_startup = mads_test::wait_for(
      [&] {
        if (sub.receive(true) != Mads::message_type::json) return false;
        auto [topic, doc] = sub.last_json();
        return doc.value("event", "") == "startup";
      },
      3000ms, 10ms);
  REQUIRE(got_startup);

  app.disconnect(); // registers the shutdown event synchronously
  bool got_shutdown = mads_test::wait_for(
      [&] {
        if (sub.receive(true) != Mads::message_type::json) return false;
        auto [topic, doc] = sub.last_json();
        return doc.value("event", "") == "shutdown";
      },
      3000ms, 10ms);
  REQUIRE(got_shutdown);
}
