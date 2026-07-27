// Unit tests for src/director_config.hpp/.cpp -- pure parsing/validation/
// expansion/ordering of director.toml, no ZMQ/processes involved. Covers:
//   - parse_duration() / parse_ready_spec() as standalone pure functions
//   - fixture conformance (tests/fixtures/director/*.toml) against the
//     schema confirmed against mads_director v2.2.0 (see the top comment in
//     src/director_config.cpp)
//   - scale expansion, ${PWD}/${ID} substitution, after-ordering, cycle
//     detection, enabled=false, unknown-key leniency, malformed-file
//     rejection.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "director_config.hpp"

namespace fs = std::filesystem;

namespace {

fs::path fixture(const std::string &name) {
  return fs::path(MADS_TEST_FIXTURES_DIR) / "director" / name;
}

// Writes inline TOML content to a scratch file for malformed-input /
// cycle-detection cases that don't belong in the "realistic" fixtures dir.
fs::path write_temp_toml(const std::string &content, const std::string &tag) {
  auto path = fs::temp_directory_path() /
             ("mads_test_director_" + tag + ".toml");
  std::ofstream out(path, std::ios::trunc);
  out << content;
  out.close();
  return path;
}

const Mads::ProcessConfig *find_proc(const Mads::DirectorConfig &config,
                                     const std::string &name) {
  for (const auto &p : config.processes) {
    if (p.name == name) return &p;
  }
  return nullptr;
}

int index_of(const Mads::DirectorConfig &config, const std::string &name) {
  for (size_t i = 0; i < config.processes.size(); ++i) {
    if (config.processes[i].name == name) return static_cast<int>(i);
  }
  return -1;
}

} // namespace

// ---------------------------------------------------------------------------
// parse_duration
// ---------------------------------------------------------------------------

TEST_CASE("parse_duration parses supported units", "[director_config]") {
  using namespace std::chrono_literals;
  REQUIRE(Mads::parse_duration("500ms") == 500ms);
  REQUIRE(Mads::parse_duration("2s") == 2000ms);
  REQUIRE(Mads::parse_duration("1.5s") == 1500ms);
  REQUIRE(Mads::parse_duration("3m") == 180000ms);
  REQUIRE(Mads::parse_duration("5") == 5000ms); // bare number == seconds
  REQUIRE(Mads::parse_duration("0") == 0ms);
  REQUIRE(Mads::parse_duration("  250ms  ") == 250ms); // surrounding whitespace
}

TEST_CASE("parse_duration rejects malformed input", "[director_config]") {
  REQUIRE_FALSE(Mads::parse_duration("").has_value());
  REQUIRE_FALSE(Mads::parse_duration("abc").has_value());
  REQUIRE_FALSE(Mads::parse_duration("5x").has_value());
  REQUIRE_FALSE(Mads::parse_duration("-5s").has_value());
  REQUIRE_FALSE(Mads::parse_duration("5.5.5s").has_value());
}

// ---------------------------------------------------------------------------
// parse_ready_spec
// ---------------------------------------------------------------------------

TEST_CASE("parse_ready_spec parses all four probe kinds", "[director_config]") {
  std::string err;

  auto broker = Mads::parse_ready_spec("broker", &err);
  REQUIRE(broker.has_value());
  REQUIRE(broker->kind == Mads::ReadyKind::Broker);
  REQUIRE(broker->broker_uri == Mads::kDefaultBrokerProbeUri);

  auto broker_uri = Mads::parse_ready_spec("broker:tcp://localhost:9999", &err);
  REQUIRE(broker_uri.has_value());
  REQUIRE(broker_uri->broker_uri == "tcp://localhost:9999");

  auto port = Mads::parse_ready_spec("port:8080", &err);
  REQUIRE(port.has_value());
  REQUIRE(port->kind == Mads::ReadyKind::Port);
  REQUIRE(port->port == 8080);

  auto log = Mads::parse_ready_spec("log:^ready$", &err);
  REQUIRE(log.has_value());
  REQUIRE(log->kind == Mads::ReadyKind::Log);
  REQUIRE(log->log_pattern == "^ready$");

  auto delay = Mads::parse_ready_spec("delay:500ms", &err);
  REQUIRE(delay.has_value());
  REQUIRE(delay->kind == Mads::ReadyKind::Delay);
  REQUIRE(delay->delay == std::chrono::milliseconds(500));
}

TEST_CASE("parse_ready_spec rejects malformed values", "[director_config]") {
  std::string err;
  REQUIRE_FALSE(Mads::parse_ready_spec("", &err).has_value());
  REQUIRE_FALSE(Mads::parse_ready_spec("bogus", &err).has_value());
  REQUIRE_FALSE(Mads::parse_ready_spec("port:not-a-number", &err).has_value());
  REQUIRE_FALSE(Mads::parse_ready_spec("port:99999", &err).has_value());
  REQUIRE_FALSE(Mads::parse_ready_spec("log:", &err).has_value());
  REQUIRE_FALSE(Mads::parse_ready_spec("log:(unclosed", &err).has_value());
  REQUIRE_FALSE(Mads::parse_ready_spec("delay:notaduration", &err).has_value());
  REQUIRE(!err.empty());
}

// ---------------------------------------------------------------------------
// Fixture conformance
// ---------------------------------------------------------------------------

TEST_CASE("simple.toml: single process, no dependencies", "[director_config]") {
  std::string error;
  auto config = Mads::load_director_config(fixture("simple.toml").string(), &error);
  REQUIRE(config.has_value());
  REQUIRE(error.empty());
  REQUIRE(config->processes.size() == 1);
  REQUIRE(config->processes[0].name == "worker");
  REQUIRE(config->processes[0].command ==
         "mads worker -s tcp://localhost:9092");
  REQUIRE(config->processes[0].enabled);
  REQUIRE(config->processes[0].after.empty());
  REQUIRE_FALSE(config->processes[0].ready.has_value());
}

TEST_CASE("multi_after.toml: after-ordering, scale, enabled=false",
         "[director_config]") {
  std::string error;
  auto config =
      Mads::load_director_config(fixture("multi_after.toml").string(), &error);
  REQUIRE(config.has_value());
  REQUIRE(error.empty());
  REQUIRE(config->sample_rate_seconds == 2.0);

  // scale=2 on [api] expands to api[1]/api[2]; db has no scale so stays "db".
  REQUIRE(find_proc(*config, "db") != nullptr);
  REQUIRE(find_proc(*config, "api[1]") != nullptr);
  REQUIRE(find_proc(*config, "api[2]") != nullptr);
  REQUIRE(find_proc(*config, "api") == nullptr); // no bare name once scaled
  REQUIRE(find_proc(*config, "worker") != nullptr);
  REQUIRE(config->processes.size() == 4);

  // after-ordering: db before both api instances, both api instances before
  // worker (worker's `after = "api"` expands to *all* api instances).
  REQUIRE(index_of(*config, "db") < index_of(*config, "api[1]"));
  REQUIRE(index_of(*config, "db") < index_of(*config, "api[2]"));
  REQUIRE(index_of(*config, "api[1]") < index_of(*config, "worker"));
  REQUIRE(index_of(*config, "api[2]") < index_of(*config, "worker"));

  const auto *worker = find_proc(*config, "worker");
  REQUIRE(worker != nullptr);
  REQUIRE_FALSE(worker->enabled); // enabled=false is parsed, not filtered out
  REQUIRE(worker->tty);
  std::vector<std::string> deps = worker->after;
  std::sort(deps.begin(), deps.end());
  REQUIRE(deps == std::vector<std::string>{"api[1]", "api[2]"});

  const auto *db = find_proc(*config, "db");
  REQUIRE(db->relaunch);
  REQUIRE(db->after.empty());
}

TEST_CASE("scale_templating.toml: ${PWD}/${ID} substitution per instance",
         "[director_config]") {
  std::string error;
  auto config = Mads::load_director_config(
      fixture("scale_templating.toml").string(), &error);
  REQUIRE(config.has_value());

  const auto *db = find_proc(*config, "db");
  REQUIRE(db != nullptr);
  REQUIRE(db->instance_id == 0);
  // ${PWD} resolves to the instance's own workdir; ${ID} is the 0-based index.
  REQUIRE(db->command ==
         "./bin/db --port 5432 --wd=" + db->workdir + " --id=0");
  REQUIRE(fs::path(db->workdir).filename() == "db");

  const auto *api1 = find_proc(*config, "api[1]");
  const auto *api2 = find_proc(*config, "api[2]");
  const auto *api3 = find_proc(*config, "api[3]");
  REQUIRE(api1 != nullptr);
  REQUIRE(api2 != nullptr);
  REQUIRE(api3 != nullptr);
  REQUIRE(api1->instance_id == 0);
  REQUIRE(api2->instance_id == 1);
  REQUIRE(api3->instance_id == 2);
  REQUIRE(api1->command.find("--id=0") != std::string::npos);
  REQUIRE(api2->command.find("--id=1") != std::string::npos);
  REQUIRE(api3->command.find("--id=2") != std::string::npos);
  // All three instances share one workdir (scale doesn't fan out `workdir`).
  REQUIRE(api1->workdir == api2->workdir);
  REQUIRE(api1->workdir == api3->workdir);
  REQUIRE(fs::path(api1->workdir).filename() == "api");
  REQUIRE(api1->command.find("--wd=" + api1->workdir) != std::string::npos);

  std::vector<std::string> after = api1->after;
  REQUIRE(after == std::vector<std::string>{"db"});
}

TEST_CASE("with_ready.toml: ready key parsed for all four kinds",
         "[director_config]") {
  std::string error;
  auto config =
      Mads::load_director_config(fixture("with_ready.toml").string(), &error);
  REQUIRE(config.has_value());

  const auto *broker = find_proc(*config, "broker");
  REQUIRE(broker->ready.has_value());
  REQUIRE(broker->ready->kind == Mads::ReadyKind::Broker);

  const auto *logger = find_proc(*config, "logger");
  REQUIRE(logger->ready.has_value());
  REQUIRE(logger->ready->kind == Mads::ReadyKind::Delay);
  REQUIRE(logger->ready->delay == std::chrono::milliseconds(500));

  const auto *api = find_proc(*config, "api");
  REQUIRE(api->ready.has_value());
  REQUIRE(api->ready->kind == Mads::ReadyKind::Port);
  REQUIRE(api->ready->port == 8080);

  const auto *watcher = find_proc(*config, "watcher");
  REQUIRE(watcher->ready.has_value());
  REQUIRE(watcher->ready->kind == Mads::ReadyKind::Log);
  REQUIRE(watcher->ready->log_pattern == "^ready$");
}

TEST_CASE("unknown_keys.toml: unrecognized keys are ignored, not fatal, and "
         "logged as warnings",
         "[director_config]") {
  std::string error;
  std::vector<std::string> warnings;
  auto config = Mads::load_director_config(fixture("unknown_keys.toml").string(),
                                          &error, &warnings);
  REQUIRE(config.has_value());
  REQUIRE(error.empty());
  REQUIRE(config->processes.size() == 1);
  REQUIRE(config->sample_rate_seconds == 1.0);

  REQUIRE(warnings.size() == 2);
  bool saw_director_key = false, saw_process_key = false;
  for (const auto &w : warnings) {
    if (w.find("some_future_gui_only_key") != std::string::npos) saw_director_key = true;
    if (w.find("some_future_key") != std::string::npos) saw_process_key = true;
  }
  REQUIRE(saw_director_key);
  REQUIRE(saw_process_key);
}

// ---------------------------------------------------------------------------
// Cycle detection
// ---------------------------------------------------------------------------

TEST_CASE("a two-node after cycle is rejected", "[director_config]") {
  auto path = write_temp_toml(R"([a]
command = "echo a"
after = "b"

[b]
command = "echo b"
after = "a"
)",
                              "cycle2");
  std::string error;
  auto config = Mads::load_director_config(path.string(), &error);
  REQUIRE_FALSE(config.has_value());
  REQUIRE(error.find("cycle") != std::string::npos);
  fs::remove(path);
}

TEST_CASE("a process that depends on itself is rejected", "[director_config]") {
  auto path = write_temp_toml(R"([a]
command = "echo a"
after = "a"
)",
                              "self_cycle");
  std::string error;
  auto config = Mads::load_director_config(path.string(), &error);
  REQUIRE_FALSE(config.has_value());
  REQUIRE(error.find("cycle") != std::string::npos);
  fs::remove(path);
}

// ---------------------------------------------------------------------------
// Malformed files rejected with a useful error message
// ---------------------------------------------------------------------------

TEST_CASE("missing 'command' is rejected with a clear message", "[director_config]") {
  auto path = write_temp_toml("[api]\nafter = \"\"\n", "missing_command");
  std::string error;
  auto config = Mads::load_director_config(path.string(), &error);
  REQUIRE_FALSE(config.has_value());
  REQUIRE(error.find("command") != std::string::npos);
  REQUIRE(error.find("api") != std::string::npos);
  fs::remove(path);
}

TEST_CASE("invalid scale is rejected", "[director_config]") {
  auto path = write_temp_toml("[api]\ncommand = \"echo hi\"\nscale = 0\n",
                              "invalid_scale");
  std::string error;
  auto config = Mads::load_director_config(path.string(), &error);
  REQUIRE_FALSE(config.has_value());
  REQUIRE(error.find("scale") != std::string::npos);
  fs::remove(path);
}

TEST_CASE("unknown 'after' target is rejected", "[director_config]") {
  auto path = write_temp_toml(
      "[api]\ncommand = \"echo hi\"\nafter = \"nonexistent\"\n", "bad_after");
  std::string error;
  auto config = Mads::load_director_config(path.string(), &error);
  REQUIRE_FALSE(config.has_value());
  REQUIRE(error.find("nonexistent") != std::string::npos);
  fs::remove(path);
}

TEST_CASE("duplicate process names are rejected", "[director_config]") {
  // Two [api] sections. TOML itself forbids redefining a table (toml++
  // raises this as a parse error before our own explicit duplicate-name
  // guard in load_director_config() ever runs), so this asserts on the
  // observable, user-facing contract -- a director.toml naming the same
  // process twice is rejected either way -- rather than on which layer
  // catches it.
  auto path = write_temp_toml(
      "[api]\ncommand = \"echo 1\"\n\n[api]\ncommand = \"echo 2\"\n", "dup_a");
  std::string error;
  auto config = Mads::load_director_config(path.string(), &error);
  REQUIRE_FALSE(config.has_value());
  REQUIRE_FALSE(error.empty());
  fs::remove(path);
}

TEST_CASE("invalid 'ready' value is rejected with the process name in the "
         "message",
         "[director_config]") {
  auto path = write_temp_toml(
      "[api]\ncommand = \"echo hi\"\nready = \"bogus\"\n", "bad_ready");
  std::string error;
  auto config = Mads::load_director_config(path.string(), &error);
  REQUIRE_FALSE(config.has_value());
  REQUIRE(error.find("api") != std::string::npos);
  REQUIRE(error.find("ready") != std::string::npos);
  fs::remove(path);
}

TEST_CASE("malformed TOML syntax is rejected with a parse error message",
         "[director_config]") {
  auto path = write_temp_toml("[api\ncommand = \"echo hi\"\n", "bad_syntax");
  std::string error;
  auto config = Mads::load_director_config(path.string(), &error);
  REQUIRE_FALSE(config.has_value());
  REQUIRE_FALSE(error.empty());
  fs::remove(path);
}

TEST_CASE("a file with no process sections is rejected", "[director_config]") {
  auto path = write_temp_toml("[director]\nsample_rate = 1.0\n", "no_processes");
  std::string error;
  auto config = Mads::load_director_config(path.string(), &error);
  REQUIRE_FALSE(config.has_value());
  REQUIRE_FALSE(error.empty());
  fs::remove(path);
}

TEST_CASE("a nonexistent file is rejected", "[director_config]") {
  std::string error;
  auto config = Mads::load_director_config("/nonexistent/path/director.toml",
                                          &error);
  REQUIRE_FALSE(config.has_value());
  REQUIRE_FALSE(error.empty());
}

// ---------------------------------------------------------------------------
// Absolute workdir is respected as-is (not resolved against base_dir)
// ---------------------------------------------------------------------------

TEST_CASE("an absolute workdir is used verbatim", "[director_config]") {
#ifdef _WIN32
  const std::string abs_dir = "C:\\mads\\workdir";
#else
  const std::string abs_dir = "/tmp/mads_test_abs_workdir";
#endif
  auto path = write_temp_toml(
      "[api]\ncommand = \"echo hi\"\nworkdir = \"" + abs_dir + "\"\n",
      "abs_workdir");
  std::string error;
  auto config = Mads::load_director_config(path.string(), &error);
  REQUIRE(config.has_value());
  REQUIRE(config->processes[0].workdir == fs::path(abs_dir).string());
  fs::remove(path);
}
