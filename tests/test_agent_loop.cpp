// Unit tests for Mads::Agent's main loop machinery and remote_control()
// (src/agent.cpp ~995-1120). Each agent owns its own Mads::Runtime, so loop
// tests stop agents through agent.runtime()->stop() without touching shared
// state. remote_control("restart"/"shutdown") is process-level by design
// (it stops Mads::Runtime::process_running()), so those TEST_CASEs
// instantiate mads_test::RunningGuard to restore the process flag.
//
// The watchdog *firing* path (install_loop_watchdog()'s force-exit branch)
// is intentionally not exercised: it calls std::_Exit() and would kill the
// test binary. install_loop_watchdog() itself is exercised once, in a way
// that never lets the countdown start (the agent is never asked to stop
// while looping).
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "agent.hpp"
#include "mads_test_helpers.hpp"

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// loop()
// ---------------------------------------------------------------------------

TEST_CASE("loop() invokes the lambda repeatedly until the agent's Runtime "
          "is stopped",
          "[agent_loop]") {
  Mads::Agent a("loopA", "none");
  a.init(false, false);

  int count = 0;
  const int target = 5;
  a.loop(
      [&]() -> std::chrono::nanoseconds {
        ++count;
        if (count >= target)
          a.runtime()->stop();
        return std::chrono::nanoseconds(0);
      },
      std::chrono::nanoseconds(0));

  REQUIRE(count == target);
}

TEST_CASE("loop() paces iterations by the requested duration",
          "[agent_loop]") {
  Mads::Agent a("loopB", "none");
  a.init(false, false);

  const auto period = std::chrono::milliseconds(20);
  const int target = 5;
  int count = 0;
  auto t0 = std::chrono::steady_clock::now();
  a.loop(
      [&]() -> std::chrono::nanoseconds {
        ++count;
        if (count >= target)
          a.runtime()->stop();
        return std::chrono::nanoseconds(0); // keep using the fixed `period`
      },
      period);
  auto elapsed = std::chrono::steady_clock::now() - t0;

  REQUIRE(count == target);
  // 5 iterations at ~20ms apiece must take noticeably longer than max-speed
  // (which would complete in well under 1ms); generous bounds avoid flakiness
  // while still proving pacing is actually happening.
  REQUIRE(elapsed >= 60ms);
  REQUIRE(elapsed < 5000ms);
}

TEST_CASE("loop()'s lambda-returned duration overrides the fixed duration "
          "on subsequent iterations",
          "[agent_loop]") {
  Mads::Agent a("loopE", "none");
  a.init(false, false);

  int count = 0;
  auto t0 = std::chrono::steady_clock::now();
  // The wait before each iteration is picked from the *previous* iteration's
  // returned duration (falling back to the fixed `duration` argument only
  // while that is still zero, i.e. on the very first iteration). So with a
  // fixed 1s duration and a lambda that requests 500us from then on, total
  // runtime should be roughly one second (one long wait), not five.
  a.loop(
      [&]() -> std::chrono::nanoseconds {
        ++count;
        if (count >= 5)
          a.runtime()->stop();
        return std::chrono::microseconds(500);
      },
      std::chrono::seconds(1));
  auto elapsed = std::chrono::steady_clock::now() - t0;

  REQUIRE(count == 5);
  REQUIRE(elapsed >= 900ms);  // the first iteration's wait used the 1s duration
  REQUIRE(elapsed < 2500ms); // but not all five (that would take >=4s)
}

TEST_CASE("loop() catches an exception thrown by the lambda and stops "
          "only that agent's loop",
          "[agent_loop]") {
  Mads::Agent a("loopF", "none");
  a.init(false, false);

  int count = 0;
  a.loop(
      [&]() -> std::chrono::nanoseconds {
        ++count;
        throw std::runtime_error("boom");
      },
      std::chrono::nanoseconds(0));

  REQUIRE(count == 1); // the loop exits after the first, throwing iteration
  // The stop is agent-local: neither the agent's Runtime nor the process-wide
  // flag is touched, so other agents in the process keep running.
  REQUIRE(a.runtime()->running());
  REQUIRE(Mads::Runtime::process_running().load());
}

// ---------------------------------------------------------------------------
// high_res_loop
// ---------------------------------------------------------------------------

TEST_CASE("enable_high_res_loop toggles high_res_loop()", "[agent_loop]") {
  Mads::Agent a("loopC", "none");
  a.init(false, false);

  REQUIRE_FALSE(a.high_res_loop()); // default off
  a.enable_high_res_loop(true, std::chrono::microseconds(500));
  REQUIRE(a.high_res_loop());
  a.enable_high_res_loop(false);
  REQUIRE_FALSE(a.high_res_loop());
}

// ---------------------------------------------------------------------------
// install_loop_watchdog(): install/orderly-stop path only (never trips)
// ---------------------------------------------------------------------------

TEST_CASE("install_loop_watchdog() starts and joins cleanly on an orderly "
          "shutdown",
          "[agent_loop]") {
  mads_test::RunningGuard guard;
  Mads::Agent a("loopD", "none");
  // Default install_watchdog=true exercises install_loop_watchdog(). The
  // agent is never asked to stop while looping, so the watchdog's force-exit
  // countdown never starts before shutdown() joins it.
  a.init();
  a.shutdown(); // stops and joins the watchdog thread promptly
  SUCCEED("watchdog thread installed and joined without tripping");
}

// ---------------------------------------------------------------------------
// remote_control()
// ---------------------------------------------------------------------------

TEST_CASE("remote_control(\"restart\") sets restart() and requests a "
          "process-wide stop",
          "[agent_loop]") {
  mads_test::RunningGuard guard;
  Mads::Agent a("rcA", "none");
  a.init(false, false);

  REQUIRE_FALSE(a.restart());
  a.remote_control(R"({"cmd":"restart"})");
  REQUIRE(a.restart());
  REQUIRE_FALSE(Mads::Runtime::process_running().load());
}

TEST_CASE("remote_control(\"shutdown\") requests a process-wide stop "
          "without setting restart()",
          "[agent_loop]") {
  mads_test::RunningGuard guard;
  Mads::Agent a("rcB", "none");
  a.init(false, false);

  a.remote_control(R"({"cmd":"shutdown"})");
  REQUIRE_FALSE(a.restart());
  REQUIRE_FALSE(Mads::Runtime::process_running().load());
}

TEST_CASE("remote_control() with malformed JSON is a no-op that leaves "
          "the process-wide run flag set",
          "[agent_loop]") {
  mads_test::RunningGuard guard;
  Mads::Agent a("rcC", "none");
  a.init(false, false);

  a.remote_control("not json at all");
  REQUIRE(Mads::Runtime::process_running().load());
  REQUIRE_FALSE(a.restart());
}

TEST_CASE("remote_control() with an unrecognized cmd is a no-op",
          "[agent_loop]") {
  mads_test::RunningGuard guard;
  Mads::Agent a("rcE", "none");
  a.init(false, false);

  a.remote_control(R"({"cmd":"frobnicate"})");
  REQUIRE(Mads::Runtime::process_running().load());
  REQUIRE_FALSE(a.restart());
}

TEST_CASE("remote_control(\"info\") publishes the agent's settings on the "
          "\"info\" topic",
          "[agent_loop]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42150;

  auto pub = std::make_unique<Mads::Agent>("rcD", "none");
  pub->init(false, false);
  pub->set_cross(true);
  pub->set_sub_endpoint(mads_test::loopback(port));
  pub->set_sub_topic({});
  pub->connect(std::chrono::milliseconds(0));

  auto sub = std::make_unique<Mads::Agent>("rcDsub", "none");
  sub->init(false, false);
  sub->set_sub_endpoint(mads_test::loopback(port));
  sub->set_pub_topic("");
  sub->set_sub_topic({""});
  sub->connect(std::chrono::milliseconds(0));

  // Warm up the loopback pair (absorbs the ZMQ "slow joiner" delay) before
  // relying on remote_control()'s internal publish() call.
  bool warm = mads_test::wait_for(
      [&] {
        pub->publish(nlohmann::json{{"warm", 1}}, "warm");
        return mads_test::wait_for(
            [&] { return sub->receive(true) == Mads::message_type::json; },
            150ms, 10ms);
      },
      3000ms, 200ms);
  REQUIRE(warm);

  pub->remote_control(R"({"cmd":"info"})");

  bool got = mads_test::wait_for(
      [&] { return sub->receive(true) == Mads::message_type::json; }, 2000ms,
      20ms);
  REQUIRE(got);
  auto [topic, doc] = sub->last_json();
  REQUIRE(topic == "info");
  REQUIRE(doc.at("agent") == "rcD");
  // "info" must not touch the run state
  REQUIRE(Mads::Runtime::process_running().load());
}
