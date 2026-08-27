/*
  _   _         ____                              _
 | | | |_ __   / ___| _   _ _ __   ___ _ ____   __(_)___  ___  _ __
 | | | | '_ \  \___ \| | | | '_ \ / _ \ '__\ \ / /| / __|/ _ \| '__|
 | |_| | |_) |  ___) | |_| | |_) |  __/|   \ V / | \__ \ (_) | |
  \___/| .__/  |____/ \__,_| .__/ \___|_|    \_/  |_|___/\___/|_|
       |_|                 |_|

Process supervisor backing `mads up` (src/main/up.cpp): starts a
Mads::DirectorConfig's processes in dependency order, gates each start on its
`ready` probe, restarts `relaunch` processes with exponential backoff, and
tears everything down (SIGTERM -> grace -> SIGKILL) on request.

Spawns via vendored reproc (see src/up_supervisor.cpp for why the plain C API
is used instead of reproc++, and how process groups are handled on POSIX,
where reproc itself does not set one up).

Author(s): Paolo Bosetti
*/
#ifndef MADS_UP_SUPERVISOR_HPP
#define MADS_UP_SUPERVISOR_HPP

#include "broker_probe.hpp"
#include "director_config.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Mads {

// Defined in up_supervisor.cpp. Deliberately a free (non-nested) type, not a
// private nested class of UpSupervisor: the reader threads that drain a
// child's stdout/stderr are free functions (std::thread needs a plain
// callable), and a private nested type would not be nameable from them.
struct UpManagedProcess;

struct UpOptions {
  std::optional<std::string> until_exit; // process name to wait for
  std::optional<std::chrono::milliseconds> timeout; // hard cap for the whole run
  std::chrono::milliseconds grace{5000};  // SIGTERM -> SIGKILL grace period
  int max_restarts = -1;                  // -1 = unlimited
  bool no_shell = false;                  // tokenize `command` instead of shelling out
  // Per-process readiness wait; not exposed on the schema/CLI (director.toml
  // has no per-key timeout), applies uniformly to every `ready = "..."`.
  std::chrono::milliseconds ready_timeout{10000};
  bool quiet = false; // suppress multiplexed [name] stdout/stderr passthrough
  // CURVE credentials for `ready = "broker"` probes, from mads-up's own
  // --crypto/--keys_dir/--key_client/--key_broker flags. Unset means "probe
  // in the clear", which reaches only an unencrypted broker: a CURVE-secured
  // one drops a plain peer during the ZMTP handshake, so the probe would
  // never come back and the gate would wait out its full ready_timeout with
  // a perfectly healthy broker running. Not a director.toml key: the plan
  // format is shared with Director's GUI, and every process already names
  // its own --crypto flags inside `command`.
  std::optional<ProbeCurveKeys> curve;
};

enum class RunOutcome {
  Ok,           // clean teardown (stop requested, or --until-exit target exited)
  Timeout,      // --timeout elapsed
  ReadyTimeout, // a process's `ready` probe never succeeded
  StartFailed,  // a process failed to spawn
  ProcessFailed // a non-relaunch process exited non-zero unexpectedly
};

struct RunResult {
  RunOutcome outcome = RunOutcome::Ok;
  int exit_code = 0; // meaningful for RunOutcome::Ok when --until-exit is set,
                      // and for the failing process otherwise
  std::string message;
};

/**
 * @brief Starts, supervises and tears down the processes described by an
 * expanded Mads::DirectorConfig. Foreground-only by design (see
 * NEW_FEATURES.md P5): one call to run() drives the whole lifecycle and
 * blocks until teardown completes.
 */
class UpSupervisor {
public:
  UpSupervisor(std::vector<ProcessConfig> processes, UpOptions options);
  ~UpSupervisor();

  UpSupervisor(const UpSupervisor &) = delete;
  UpSupervisor &operator=(const UpSupervisor &) = delete;

  /**
   * @brief Starts every enabled process in order (waiting on each one's
   * `ready` probe before moving on to whatever depends on it), then monitors
   * until a stop is requested (request_stop(), --until-exit's target
   * exiting, --timeout elapsing, or a non-relaunch process dying with a
   * non-zero exit code), then tears everything down in reverse start order.
   * Blocking; safe to call exactly once per instance.
   */
  RunResult run();

  /// Thread-safe. Asks a running (or not-yet-started) supervisor to begin
  /// teardown at the next opportunity. Intended to be called from a signal
  /// handler installed by main().
  void request_stop();

  /// True once request_stop() was called or run() decided to stop on its own.
  bool stop_requested() const;

  // --- test hooks (tests/test_up_supervisor.cpp) ---------------------------

  /// Number of times the named process instance has been started (1 after
  /// the first start, 2 after one relaunch, ...). 0 if never started/unknown.
  int start_count(const std::string &name) const;

  /// True if any managed process is currently running. Used after run()
  /// returns to assert teardown left no orphans.
  bool any_running() const;

private:
  std::vector<ProcessConfig> _processes;
  UpOptions _options;
  std::vector<std::unique_ptr<UpManagedProcess>> _managed;
  std::atomic<bool> _stop_requested{false};

  bool start_one(UpManagedProcess &mp);
  bool wait_ready(UpManagedProcess &mp, std::chrono::milliseconds timeout);
  void stop_one(UpManagedProcess &mp, std::chrono::milliseconds grace);
  void teardown();
  UpManagedProcess *find(const std::string &name);
};

} // namespace Mads

#endif // MADS_UP_SUPERVISOR_HPP
