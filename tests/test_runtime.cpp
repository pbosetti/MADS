// Unit tests for Mads::Runtime and the per-agent run-state semantics
// introduced by it. The key regressions guarded here are the historical
// process-global Mads::running hazards: destroying one agent used to stop
// every other agent's loop (and LKV drain thread) in the same process, and a
// disconnected agent could never loop again.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "agent.hpp"
#include "mads_test_helpers.hpp"

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Runtime semantics
// ---------------------------------------------------------------------------

TEST_CASE("Runtime starts running and stops/resets independently",
          "[runtime]") {
  Mads::Runtime rt;
  REQUIRE(rt.running());
  rt.stop();
  REQUIRE_FALSE(rt.running());
  rt.reset();
  REQUIRE(rt.running());
}

TEST_CASE("stop_process() stops every Runtime and reset() cannot override it",
          "[runtime]") {
  mads_test::RunningGuard guard; // restores the process flag afterwards
  Mads::Runtime rt1, rt2;
  REQUIRE(rt1.running());
  REQUIRE(rt2.running());

  Mads::Runtime::stop_process();
  REQUIRE_FALSE(rt1.running());
  REQUIRE_FALSE(rt2.running());

  rt1.reset(); // a group-level reset must not resurrect a stopping process
  REQUIRE_FALSE(rt1.running());
}

// ---------------------------------------------------------------------------
// Agent/Runtime wiring
// ---------------------------------------------------------------------------

TEST_CASE("each agent owns its own Runtime by default", "[runtime]") {
  Mads::Agent a("rtA", "none"), b("rtB", "none");
  REQUIRE(a.runtime() != nullptr);
  REQUIRE(b.runtime() != nullptr);
  REQUIRE(a.runtime() != b.runtime());
  a.runtime()->stop();
  REQUIRE_FALSE(a.runtime()->running());
  REQUIRE(b.runtime()->running());
}

TEST_CASE("set_runtime() rejects null and connected agents", "[runtime]") {
  Mads::Agent a("rtC", "none");
  a.init(false, false);
  REQUIRE_THROWS_AS(a.set_runtime(nullptr), Mads::AgentError);

  a.set_pub_topic("t");
  a.connect(0ms);
  REQUIRE_THROWS_AS(a.set_runtime(std::make_shared<Mads::Runtime>()),
                    Mads::AgentError);
  a.disconnect();
}

TEST_CASE("agents sharing a Runtime are stopped together", "[runtime]") {
  Mads::Agent a("rtD", "none"), b("rtE", "none");
  b.set_runtime(a.runtime());
  REQUIRE(a.runtime() == b.runtime());
  a.runtime()->stop();
  REQUIRE_FALSE(b.runtime()->running());
}

// ---------------------------------------------------------------------------
// Regressions for the historical global-flag hazards
// ---------------------------------------------------------------------------

TEST_CASE("destroying one agent does not stop another agent's loop",
          "[runtime]") {
  auto victim = std::make_unique<Mads::Agent>("rtF", "none");
  victim->init(false, false);

  Mads::Agent survivor("rtG", "none");
  survivor.init(false, false);

  std::atomic<int> count{0};
  std::thread loop_thread([&] {
    survivor.loop(
        [&]() -> std::chrono::nanoseconds {
          ++count;
          return std::chrono::nanoseconds(0);
        },
        1ms);
  });

  REQUIRE(mads_test::wait_for([&] { return count.load() >= 3; }));

  // Historically this shutdown flipped the process-global run flag and the
  // survivor's loop exited on its next iteration.
  victim->shutdown();
  victim.reset();

  int at_destruction = count.load();
  bool still_looping = mads_test::wait_for(
      [&] { return count.load() >= at_destruction + 5; });

  survivor.runtime()->stop();
  loop_thread.join();
  REQUIRE(still_looping);
}

TEST_CASE("an agent can loop again after disconnect() and connect()",
          "[runtime]") {
  Mads::Agent a("rtH", "none");
  a.init(false, false);
  a.set_pub_topic("t");
  a.connect(0ms);
  a.disconnect(); // raises the per-agent stop request...
  a.connect(0ms); // ...and reconnecting clears it

  int count = 0;
  a.loop(
      [&]() -> std::chrono::nanoseconds {
        if (++count >= 3)
          a.runtime()->stop();
        return std::chrono::nanoseconds(0);
      },
      std::chrono::nanoseconds(0));
  REQUIRE(count == 3);
  a.disconnect();
}

// ---------------------------------------------------------------------------
// Deprecated Mads::running shim
// ---------------------------------------------------------------------------

// The shim must keep legacy code working: it aliases the process-wide flag,
// so writing false stops every agent and reading it reports process state.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
TEST_CASE("deprecated Mads::running still aliases the process-wide flag",
          "[runtime]") {
  mads_test::RunningGuard guard;
  Mads::Runtime rt;

  REQUIRE(Mads::running.load());
  Mads::running = false; // legacy process-wide stop
  REQUIRE_FALSE(rt.running());
  REQUIRE_FALSE(Mads::Runtime::process_running().load());

  Mads::running = true;
  REQUIRE(rt.running());
}
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif
