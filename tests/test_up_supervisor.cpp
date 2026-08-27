// Unit tests for src/up_supervisor.hpp/.cpp: the reproc-based process
// supervisor behind `mads up`. Builds Mads::ProcessConfig vectors directly
// (bypassing director_config.cpp's TOML parsing, which has its own test
// suite in test_director_config.cpp) and spawns trivial, portable shell
// one-liners as the managed "processes" -- exit-with-code, sleep-then-exit,
// print-then-exit -- through the supervisor's own (default) shell-out path,
// exercised on POSIX where this suite actually runs in CI.
//
// Windows-specific process-group teardown (CREATE_NEW_PROCESS_GROUP /
// CTRL_BREAK_EVENT, handled inside reproc itself -- see up_supervisor.cpp)
// is not exercised here since this repo's CI/dev environment for this task
// is POSIX; the POSIX process-group path (setpgid/killpg) is exercised
// directly, including a check that a grandchild survives if and only if the
// whole group was actually signaled.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <zmq.hpp>
#include <zmq_addon.hpp>

#ifndef _WIN32
#include <cerrno>
#include <signal.h>
#include <sys/types.h>
#endif

#include "curve.hpp"
#include "mads_test_helpers.hpp"
#include "up_supervisor.hpp"

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

Mads::ProcessConfig make_proc(const std::string &name, const std::string &command,
                             std::vector<std::string> after = {},
                             bool relaunch = false, bool enabled = true) {
  Mads::ProcessConfig p;
  p.name = name;
  p.base_name = name;
  p.command = command;
  p.workdir = fs::temp_directory_path().string();
  p.enabled = enabled;
  p.relaunch = relaunch;
  p.after = std::move(after);
  return p;
}

fs::path scratch_file(const std::string &tag) {
  return fs::temp_directory_path() /
        ("mads_test_up_supervisor_" + tag + "_" +
         std::to_string(reinterpret_cast<uintptr_t>(&tag)) + ".txt");
}

// Appends a single line to `path`. Used so that trivial shell commands can
// leave an observable, ordered trace of what actually ran.
std::string append_line_cmd(const fs::path &path, const std::string &text) {
  return "echo " + text + " >> \"" + path.string() + "\"";
}

std::vector<std::string> read_lines(const fs::path &path) {
  std::vector<std::string> lines;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty()) lines.push_back(line);
  }
  return lines;
}

} // namespace

// ---------------------------------------------------------------------------
// Start order honours `after`
// ---------------------------------------------------------------------------

TEST_CASE("run() starts processes in after-order", "[up_supervisor]") {
  // Absent an explicit `ready` key, `after`-order only guarantees *start*
  // order, not *completion* order (see up_supervisor.cpp:593-594: wait_ready()
  // is only invoked when a process configures `ready`). Since these are
  // trivial, near-instant commands, asserting completion order via the shared
  // trace file needs `a`/`b` to actually gate their dependent's start.
  //
  // A prior version of this test used a fixed `delay` ready (see 6a0a835,
  // "deflake after-order test") on the assumption that 100ms is "well over"
  // the time an `echo ... >> file; exit 0` takes. That's a guess about shell
  // fork/exec latency, and it flaked again under CI load (the Coverage job
  // runs gcov-instrumented binaries and is more prone to scheduling jitter):
  // observed order was {c, a, b}, meaning both `a` and `b` were still
  // starting up when the 100ms elapsed. A `log` ready tied to a marker each
  // process prints *after* its trace-file write removes the guess entirely --
  // wait_ready() only proceeds once that process has actually finished
  // writing, regardless of how long fork/exec/shell-startup took.
  auto trace = scratch_file("order");
  fs::remove(trace);

  Mads::ReadySpec ready_a;
  ready_a.kind = Mads::ReadyKind::Log;
  ready_a.log_pattern = "READY_A";
  Mads::ReadySpec ready_b;
  ready_b.kind = Mads::ReadyKind::Log;
  ready_b.log_pattern = "READY_B";

  Mads::ProcessConfig proc_a = make_proc(
      "a", append_line_cmd(trace, "a") + " && echo READY_A; exit 0");
  proc_a.ready = ready_a;
  Mads::ProcessConfig proc_b = make_proc(
      "b", append_line_cmd(trace, "b") + " && echo READY_B; exit 0", {"a"});
  proc_b.ready = ready_b;
  Mads::ProcessConfig proc_c =
      make_proc("c", append_line_cmd(trace, "c") + "; exit 0", {"b"});

  std::vector<Mads::ProcessConfig> procs{proc_a, proc_b, proc_c};

  Mads::UpOptions options;
  options.until_exit = "c"; // run() returns once the last one exits
  options.grace = 500ms;

  Mads::UpSupervisor sup(procs, options);
  auto result = sup.run();

  REQUIRE(result.outcome == Mads::RunOutcome::Ok);
  REQUIRE(result.exit_code == 0);

  auto lines = read_lines(trace);
  REQUIRE(lines == std::vector<std::string>{"a", "b", "c"});
  REQUIRE_FALSE(sup.any_running());
  fs::remove(trace);
}

TEST_CASE("run() skips disabled processes", "[up_supervisor]") {
  std::vector<Mads::ProcessConfig> procs{
      make_proc("skip-me", "exit 0", {}, /*relaunch=*/false, /*enabled=*/false),
      make_proc("run-me", "exit 5"),
  };
  Mads::UpOptions options;
  options.until_exit = "run-me";
  options.grace = 500ms;

  Mads::UpSupervisor sup(procs, options);
  auto result = sup.run();

  REQUIRE(result.outcome == Mads::RunOutcome::Ok);
  REQUIRE(result.exit_code == 5);
  REQUIRE(sup.start_count("skip-me") == 0);
  REQUIRE(sup.start_count("run-me") == 1);
}

// ---------------------------------------------------------------------------
// --until-exit propagates the exit code
// ---------------------------------------------------------------------------

TEST_CASE("run() propagates the --until-exit target's exit code",
         "[up_supervisor]") {
  std::vector<Mads::ProcessConfig> procs{make_proc("target", "exit 42")};
  Mads::UpOptions options;
  options.until_exit = "target";
  options.grace = 500ms;

  Mads::UpSupervisor sup(procs, options);
  auto result = sup.run();

  REQUIRE(result.outcome == Mads::RunOutcome::Ok);
  REQUIRE(result.exit_code == 42);
}

// ---------------------------------------------------------------------------
// relaunch restarts with backoff
// ---------------------------------------------------------------------------

TEST_CASE("relaunch=true restarts a crashing process with backoff",
         "[up_supervisor]") {
  std::vector<Mads::ProcessConfig> procs{
      make_proc("flaky", "exit 7", {}, /*relaunch=*/true)};
  Mads::UpOptions options;
  options.timeout = 1200ms; // let it crash/restart a couple of times, then stop
  options.grace = 300ms;

  Mads::UpSupervisor sup(procs, options);
  auto result = sup.run();

  REQUIRE(result.outcome == Mads::RunOutcome::Timeout);
  REQUIRE(sup.start_count("flaky") >= 2); // initial start + at least one relaunch
  REQUIRE_FALSE(sup.any_running());
}

TEST_CASE("relaunch honours --max-restarts", "[up_supervisor]") {
  std::vector<Mads::ProcessConfig> procs{
      make_proc("flaky", "exit 7", {}, /*relaunch=*/true)};
  Mads::UpOptions options;
  options.timeout = 1500ms;
  options.grace = 300ms;
  options.max_restarts = 1; // one restart, then give up (but keep running the
                            // rest of the system until --timeout)

  Mads::UpSupervisor sup(procs, options);
  auto result = sup.run();

  REQUIRE(result.outcome == Mads::RunOutcome::Timeout);
  // Initial start + exactly one relaunch: max_restarts=1 forbids a second.
  REQUIRE(sup.start_count("flaky") == 2);
}

// ---------------------------------------------------------------------------
// A non-relaunch process dying unexpectedly is fatal to the run
// ---------------------------------------------------------------------------

TEST_CASE("a non-relaunch process exiting non-zero ends the run",
         "[up_supervisor]") {
  std::vector<Mads::ProcessConfig> procs{
      make_proc("stable", "sleep 30; exit 0"),
      make_proc("dies", "exit 9"),
  };
  Mads::UpOptions options;
  options.grace = 300ms;
  // No --until-exit, no --timeout: only "dies" exiting badly should end this.

  Mads::UpSupervisor sup(procs, options);
  auto result = sup.run();

  REQUIRE(result.outcome == Mads::RunOutcome::ProcessFailed);
  REQUIRE(result.exit_code == 9);
  REQUIRE_FALSE(sup.any_running());
}

// ---------------------------------------------------------------------------
// --timeout fires
// ---------------------------------------------------------------------------

// Note: deliberately not named starting with "--timeout" -- catch_discover_tests
// passes each TEST_CASE's name to the binary as a filter argument, and
// Catch2's own CLI parser misreads a leading "--" as an unrecognised option
// rather than a positional test-name filter.
TEST_CASE("the timeout option fires and tears everything down",
         "[up_supervisor]") {
  std::vector<Mads::ProcessConfig> procs{make_proc("long", "sleep 30; exit 0")};
  Mads::UpOptions options;
  options.timeout = 300ms;
  options.grace = 300ms;

  const auto started = std::chrono::steady_clock::now();
  Mads::UpSupervisor sup(procs, options);
  auto result = sup.run();
  const auto elapsed = std::chrono::steady_clock::now() - started;

  REQUIRE(result.outcome == Mads::RunOutcome::Timeout);
  REQUIRE(elapsed < 10s); // proves teardown, not a 30s natural exit
  REQUIRE_FALSE(sup.any_running());
}

// ---------------------------------------------------------------------------
// Teardown kills everything, no orphans left running
// ---------------------------------------------------------------------------

TEST_CASE("request_stop() tears down a long-running process promptly",
         "[up_supervisor]") {
  std::vector<Mads::ProcessConfig> procs{make_proc("long", "sleep 30; exit 0")};
  Mads::UpOptions options;
  options.grace = 300ms;

  Mads::UpSupervisor sup(procs, options);
  std::thread runner([&] { sup.run(); });

  // Give it time to actually start before asking it to stop.
  std::this_thread::sleep_for(200ms);
  const auto stop_requested_at = std::chrono::steady_clock::now();
  sup.request_stop();
  runner.join();
  const auto elapsed = std::chrono::steady_clock::now() - stop_requested_at;

  REQUIRE(elapsed < 10s); // well under the 30s the child would sleep on its own
  REQUIRE_FALSE(sup.any_running());
}

#ifndef _WIN32
// POSIX-only: proves teardown reaches a *grandchild* process, i.e. that
// processes are actually placed in their own process group (setpgid) and
// torn down via killpg, not just the immediate shell child. Without that,
// this backgrounded `sleep` would survive as an orphan after teardown.
TEST_CASE("teardown reaches grandchildren via the process group",
         "[up_supervisor]") {
  auto pidfile = scratch_file("grandchild_pid");
  fs::remove(pidfile);

  // Backgrounds a long sleep from within the shell we spawn, records its pid,
  // then waits on it -- a minimal stand-in for "a process that itself forks
  // children," which is exactly the case process-group teardown exists for.
  std::string command = "sleep 30 & echo $! > \"" + pidfile.string() +
                        "\"; wait";
  std::vector<Mads::ProcessConfig> procs{make_proc("parent", command)};
  Mads::UpOptions options;
  options.grace = 300ms;

  Mads::UpSupervisor sup(procs, options);
  std::thread runner([&] { sup.run(); });

  // Wait for the grandchild pid file to appear.
  int pid = -1;
  for (int i = 0; i < 100 && pid <= 0; ++i) {
    std::this_thread::sleep_for(20ms);
    std::ifstream in(pidfile);
    if (in.good()) in >> pid;
  }
  REQUIRE(pid > 0);

  sup.request_stop();
  runner.join();

  // Give the OS a brief moment to finish reaping after SIGKILL.
  bool gone = false;
  for (int i = 0; i < 50 && !gone; ++i) {
    if (::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) gone = true;
    else std::this_thread::sleep_for(20ms);
  }
  REQUIRE(gone);
  fs::remove(pidfile);
}
#endif

// ---------------------------------------------------------------------------
// --no-shell tokenizes instead of shelling out
// ---------------------------------------------------------------------------

TEST_CASE("the no-shell option execs the tokenized command directly",
         "[up_supervisor]") {
  // With no shell, "exit 0" as a literal command name would fail to spawn
  // (no such executable) -- use a real, argument-taking executable instead
  // to prove tokenization (not shell interpretation) is what ran.
  auto trace = scratch_file("noshell");
  fs::remove(trace);
  std::vector<Mads::ProcessConfig> procs{
      make_proc("touch-file", "/usr/bin/touch " + trace.string())};
  Mads::UpOptions options;
  options.until_exit = "touch-file";
  options.grace = 500ms;
  options.no_shell = true;

  Mads::UpSupervisor sup(procs, options);
  auto result = sup.run();

  REQUIRE(result.outcome == Mads::RunOutcome::Ok);
  REQUIRE(result.exit_code == 0);
  REQUIRE(fs::exists(trace));
  fs::remove(trace);
}

// ---------------------------------------------------------------------------
// ready = "delay:<dur>" gates startup
// ---------------------------------------------------------------------------

TEST_CASE("a delay ready probe gates start of what depends on it",
         "[up_supervisor]") {
  auto trace = scratch_file("ready_delay");
  fs::remove(trace);

  Mads::ProcessConfig a =
      make_proc("a", append_line_cmd(trace, "a") + "; exit 0");
  Mads::ReadySpec ready;
  ready.kind = Mads::ReadyKind::Delay;
  ready.delay = 150ms;
  a.ready = ready;

  Mads::ProcessConfig b =
      make_proc("b", append_line_cmd(trace, "b") + "; exit 0", {"a"});

  Mads::UpOptions options;
  options.until_exit = "b";
  options.grace = 500ms;

  const auto started = std::chrono::steady_clock::now();
  Mads::UpSupervisor sup({a, b}, options);
  auto result = sup.run();
  const auto elapsed = std::chrono::steady_clock::now() - started;

  REQUIRE(result.outcome == Mads::RunOutcome::Ok);
  REQUIRE(elapsed >= 150ms); // b could not have started before a's delay elapsed
  auto lines = read_lines(trace);
  REQUIRE(lines == std::vector<std::string>{"a", "b"});
  fs::remove(trace);
}

TEST_CASE("a ready probe that never succeeds fails startup", "[up_supervisor]") {
  Mads::ProcessConfig a = make_proc("a", "sleep 30; exit 0");
  Mads::ReadySpec ready;
  ready.kind = Mads::ReadyKind::Port;
  ready.port = 42599; // nothing listens here
  a.ready = ready;

  Mads::UpOptions options;
  options.ready_timeout = 200ms; // keep the test fast
  options.grace = 300ms;

  Mads::UpSupervisor sup({a}, options);
  auto result = sup.run();

  REQUIRE(result.outcome == Mads::RunOutcome::ReadyTimeout);
  REQUIRE_FALSE(sup.any_running());
}

// ---------------------------------------------------------------------------
// ready = "broker" against a CURVE-secured broker
// ---------------------------------------------------------------------------

// Regression: the broker readiness probe always ran in the clear, whatever
// the fleet was configured for. A CURVE-secured broker drops a plain peer
// during the ZMTP handshake, so a perfectly healthy encrypted broker never
// answered the gate -- every dependent process waited out the full
// ready_timeout and the run died with a bare "did not become ready in time",
// naming nothing that would point at encryption.
namespace {

// A CURVE server that answers any request, standing in for `mads broker
// --crypto`'s settings endpoint. Same shape as test_doctor_checks.cpp's
// FakeCurveBroker.
class ReadyCurveBroker {
public:
  ReadyCurveBroker(uint16_t port, const fs::path &key_dir)
      : _ctx(), _sock(_ctx, zmq::socket_type::rep), _auth(_ctx) {
    _auth.setup_auth(Mads::auth_verbose::off);
    _auth.fetch_public_keys(key_dir);
    _auth.setup_curve_server(_sock, "broker");
    _sock.set(zmq::sockopt::rcvtimeo, 100);
    _sock.bind(mads_test::loopback(port));
    _thread = std::thread([this] {
      while (!_stopped) {
        zmq::multipart_t msg;
        if (!msg.recv(_sock)) continue;
        zmq::multipart_t reply;
        reply.addstr(std::string("v0.0"));
        reply.addstr(std::string("{}"));
        reply.send(_sock);
      }
    });
  }
  ~ReadyCurveBroker() {
    _stopped = true;
    if (_thread.joinable()) _thread.join();
  }

private:
  zmq::context_t _ctx;
  zmq::socket_t _sock;
  Mads::CurveAuth _auth;
  std::thread _thread;
  std::atomic<bool> _stopped{false};
};

// RAII key dir holding a client and a broker keypair, the three files
// Mads::ProbeCurveKeys names.
struct ReadyKeyDir {
  fs::path path;
  ReadyKeyDir(const std::string &tag) {
    path = fs::temp_directory_path() /
           ("mads_test_up_keys_" + tag + "_" +
            std::to_string(reinterpret_cast<uintptr_t>(&tag)));
    fs::create_directories(path);
    for (const auto &name : {std::string("broker"), std::string("client")}) {
      const auto kp = Mads::generate_keypair();
      std::ofstream(path / (name + ".key")) << kp.secret_key << "\n";
      std::ofstream(path / (name + ".pub")) << kp.public_key << "\n";
    }
  }
  ~ReadyKeyDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

} // namespace

TEST_CASE("a keyless broker ready probe never passes a CURVE broker",
          "[up_supervisor][curve]") {
  const uint16_t port = 44401;
  ReadyKeyDir keys("plain_probe");
  ReadyCurveBroker broker(port, keys.path);

  Mads::ProcessConfig a = make_proc("a", "sleep 30; exit 0");
  Mads::ReadySpec ready;
  ready.kind = Mads::ReadyKind::Broker;
  ready.broker_uri = mads_test::loopback(port);
  a.ready = ready;

  Mads::UpOptions options; // options.curve deliberately unset
  options.ready_timeout = 500ms;
  options.grace = 300ms;

  Mads::UpSupervisor sup({a}, options);
  auto result = sup.run();

  REQUIRE(result.outcome == Mads::RunOutcome::ReadyTimeout);
  // The message has to name the one cause the plan file cannot show.
  REQUIRE(result.message.find("--crypto") != std::string::npos);
}

TEST_CASE("a CURVE-configured broker ready probe opens the gate",
          "[up_supervisor][curve]") {
  const uint16_t port = 44402;
  ReadyKeyDir keys("curve_probe");
  ReadyCurveBroker broker(port, keys.path);

  auto trace = scratch_file("ready_broker_curve");
  fs::remove(trace);

  Mads::ProcessConfig a = make_proc("a", "sleep 30; exit 0");
  Mads::ReadySpec ready;
  ready.kind = Mads::ReadyKind::Broker;
  ready.broker_uri = mads_test::loopback(port);
  a.ready = ready;
  Mads::ProcessConfig b =
      make_proc("b", append_line_cmd(trace, "b") + "; exit 0", {"a"});

  Mads::UpOptions options;
  options.curve = Mads::ProbeCurveKeys{keys.path, "client", "broker"};
  options.ready_timeout = 3000ms;
  options.grace = 300ms;
  options.until_exit = "b";

  Mads::UpSupervisor sup({a, b}, options);
  auto result = sup.run();

  REQUIRE(result.outcome == Mads::RunOutcome::Ok);
  // b only ever starts once a's broker probe answered.
  REQUIRE(read_lines(trace) == std::vector<std::string>{"b"});
  fs::remove(trace);
}

TEST_CASE("a CURVE-configured probe still fails against a plain broker",
          "[up_supervisor][curve]") {
  const uint16_t port = 44403;
  ReadyKeyDir keys("curve_vs_plain");

  zmq::context_t ctx;
  zmq::socket_t plain(ctx, zmq::socket_type::rep);
  plain.bind(mads_test::loopback(port));

  Mads::ProcessConfig a = make_proc("a", "sleep 30; exit 0");
  Mads::ReadySpec ready;
  ready.kind = Mads::ReadyKind::Broker;
  ready.broker_uri = mads_test::loopback(port);
  a.ready = ready;

  Mads::UpOptions options;
  options.curve = Mads::ProbeCurveKeys{keys.path, "client", "broker"};
  options.ready_timeout = 500ms;
  options.grace = 300ms;

  Mads::UpSupervisor sup({a}, options);
  auto result = sup.run();

  REQUIRE(result.outcome == Mads::RunOutcome::ReadyTimeout);
  REQUIRE(result.message.find("drop --crypto") != std::string::npos);
}
