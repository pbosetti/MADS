/*
Shared helpers for the MADS unit-test suite.

Conventions for test authors:
- ZeroMQ endpoints must be in-process loopback only (tcp://127.0.0.1:<port>).
  inproc:// does NOT work between two Agent instances, because each Agent owns
  its own ZMQ context.
- Tests run serially under ctest, but each suite must still use its own port
  range to stay independent:
    test_agent_*  (pub/sub, wire, loop)   42100-42199
    test_agent_settings / broker          42200-42299
    test_agent_c / test_curve             42300-42399
    test_agent_events / test_agent_app /
      test_logger_receive                 42400-42499 (no longer spare)
    test_echo_loopback                    42500-42599
    test_broker_probe                     42600-42699
    test_bag_roundtrip                    42700-42799
    test_doctor_checks                    42700-42799 (shares with above)
    test_zap_auth                         42800-42899
    test_zmqpp_compat                     42900-42999
    test_broker_steering                  43900-43999
    test_socket_options                   44000-44099
    test_socket_monitor                   44100-44149
    test_broker_subscriptions             44150-44199
    test_broker_settings_pool             44200-44249
    test_agent_io_thread                  44250-44299
    test_agent_link_state                 44300-44349
    test_worker_pull                      44350-44399
    test_up_supervisor (curve probes)     44400-44449
    test_agent_slow_joiner                44450-44499
    (spare)                               44500+
- Any test that touches Mads::Agent::loop() must instantiate RunningGuard,
  since Mads::running is a process-global atomic.
*/
#ifndef MADS_TEST_HELPERS_HPP
#define MADS_TEST_HELPERS_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "../src/mads.hpp"

namespace mads_test {

inline std::string loopback(uint16_t port) {
  return "tcp://127.0.0.1:" + std::to_string(port);
}

// Restores the global run flag on scope exit so a test that stops the loop
// cannot poison later tests in the same binary. Since the Runtime refactor
// only process-level stops (signal handlers, remote_control shutdown/restart,
// Runtime::stop_process()) touch shared state; this guard resets that flag.
// Agent shutdown()/disconnect() are per-agent and need no guard.
struct RunningGuard {
  RunningGuard() { Mads::Runtime::process_running() = true; }
  ~RunningGuard() { Mads::Runtime::process_running() = true; }
};

// Polls a predicate until it returns true or the timeout expires. Use this
// instead of bare sleeps when waiting for messages or callbacks.
inline bool wait_for(const std::function<bool()> &predicate,
                     std::chrono::milliseconds timeout =
                         std::chrono::milliseconds(2000),
                     std::chrono::milliseconds poll =
                         std::chrono::milliseconds(10)) {
  auto const deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(poll);
  }
  return predicate();
}

} // namespace mads_test

#endif // MADS_TEST_HELPERS_HPP
