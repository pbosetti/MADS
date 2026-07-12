// Unit tests for the run-state functions of the C ABI (src/agent_c.h):
// agent_stop(), agent_running(), mads_stop_process(), mads_process_running().
// These are the C-side face of the Mads::Runtime model: agent_stop() affects
// one agent only, while mads_stop_process() stops every agent in the process.
#include <catch2/catch_test_macros.hpp>

#include "agent_c.h"
#include "mads_test_helpers.hpp"

TEST_CASE("a fresh agent is running and agent_stop() stops only it",
          "[agent_c_runtime]") {
  mads_test::RunningGuard guard;
  agent_t a = agent_create("crtA", "none");
  agent_t b = agent_create("crtB", "none");
  REQUIRE(agent_init(a, false) == 0);
  REQUIRE(agent_init(b, false) == 0);

  REQUIRE(agent_running(a));
  REQUIRE(agent_running(b));

  REQUIRE(agent_stop(a) == 0);
  REQUIRE_FALSE(agent_running(a));
  REQUIRE(agent_running(b)); // per-agent stop: b is unaffected

  agent_destroy(a);
  agent_destroy(b);
}

TEST_CASE("mads_stop_process() stops every agent and is reported by "
          "mads_process_running()",
          "[agent_c_runtime]") {
  mads_test::RunningGuard guard; // restores the process flag afterwards
  agent_t a = agent_create("crtC", "none");
  agent_t b = agent_create("crtD", "none");
  REQUIRE(agent_init(a, false) == 0);
  REQUIRE(agent_init(b, false) == 0);

  REQUIRE(mads_process_running());
  mads_stop_process();
  REQUIRE_FALSE(mads_process_running());
  REQUIRE_FALSE(agent_running(a));
  REQUIRE_FALSE(agent_running(b));

  agent_destroy(a);
  agent_destroy(b);
}

TEST_CASE("agent_running() is false after disconnect and true again after "
          "reconnect",
          "[agent_c_runtime]") {
  mads_test::RunningGuard guard;
  agent_t a = agent_create("crtE", "none");
  REQUIRE(agent_init(a, false) == 0);
  agent_set_pub_topic(a, "t");
  REQUIRE(agent_connect(a, 0) == 0);
  REQUIRE(agent_running(a));

  REQUIRE(agent_disconnect(a) == 0);
  REQUIRE_FALSE(agent_running(a));

  REQUIRE(agent_connect(a, 0) == 0); // re-arms the per-agent stop
  REQUIRE(agent_running(a));

  agent_destroy(a);
}

TEST_CASE("run-state functions guard against NULL handles",
          "[agent_c_runtime]") {
  REQUIRE(agent_stop(nullptr) == -1);
  REQUIRE_FALSE(agent_running(nullptr));
}
