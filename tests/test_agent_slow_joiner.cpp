// Unit tests for the slow-joiner grace in Agent::connect_pub()
// (ZMQ_DEVELOPMENT.md §2.1).
//
// A PUB socket silently discards anything sent while no subscription matches
// it, and the subscriptions of an already-running fleet only reach a freshly
// connected publisher *after* its ZMTP handshake. connect() therefore waits
// for ZMQ_EVENT_HANDSHAKE_SUCCEEDED and then holds for
// SUBSCRIPTION_SETTLE_DELAY before returning.
//
// This used to be a blind sleep of the whole `delay`, then briefly a bare
// ZMQ_EVENT_CONNECTED wait -- which fires at the TCP level, long before any
// subscription has arrived. Nothing caught the difference, because every
// other pub/sub suite here republishes in a retry loop (see
// test_agent_pubsub.cpp's retry_until) and so papers over exactly the window
// this file is about. A one-shot publisher cannot: `mads-bridge -m`, i.e.
// every `mads-command` invocation, connects, sends once and exits, and its
// single message was lost with no error anywhere. Hence the publish-exactly-
// once assertion below.
//
// Port range for this file: 44450-44499 (the former "(spare)" range; see
// mads_test_helpers.hpp).
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "agent.hpp"
#include "mads_test_helpers.hpp"

using namespace std::chrono_literals;

namespace {

/**
 * @brief A subscriber that BINDS (cross=true) and is drained continuously by
 * a thread of its own, so the agent under test is the one that CONNECTs --
 * the only arrangement in which the connecting side has to wait for
 * subscriptions to travel to it.
 *
 * The drainer is not a convenience: a SUB socket processes a newly attached
 * peer (and only then sends it the subscriptions it already holds) while the
 * application is inside a socket call, so a subscriber left idle between
 * connect() and the first receive() never announces itself at all. Every real
 * MADS subscriber sits in Agent::loop() calling receive(); a test hub that
 * did not would be measuring its own inactivity rather than the publisher's
 * settle window.
 */
class DrainedHub {
public:
  DrainedHub(std::string name, uint16_t port) {
    _agent = std::make_unique<Mads::Agent>(name, "none");
    _agent->init(false, false); // crypto=false, install_watchdog=false
    _agent->set_cross(true);
    _agent->set_pub_endpoint(mads_test::loopback(port));
    _agent->set_sub_endpoint(mads_test::loopback(port + 1));
    _agent->set_pub_topic(""); // subscriber only: connect_pub() never runs
    _agent->set_sub_topic({""});
    _agent->connect(0ms);
    _drainer = std::thread([this] {
      while (!_stop.load()) {
        if (_agent->receive(true) == Mads::message_type::json) {
          std::lock_guard<std::mutex> lock(_mtx);
          std::tie(_topic, _payload) = _agent->last_message();
          _got.store(true);
        }
        std::this_thread::sleep_for(5ms);
      }
    });
  }

  ~DrainedHub() {
    _stop.store(true);
    if (_drainer.joinable())
      _drainer.join();
  }

  bool got() const { return _got.load(); }

  std::pair<std::string, std::string> message() const {
    std::lock_guard<std::mutex> lock(_mtx);
    return {_topic, _payload};
  }

private:
  std::unique_ptr<Mads::Agent> _agent;
  std::thread _drainer;
  std::atomic<bool> _stop{false};
  std::atomic<bool> _got{false};
  mutable std::mutex _mtx;
  std::string _topic;
  std::string _payload;
};

// Publish-only agent that CONNECTs to `port`. Deliberately left unconnected:
// the connect() call is what each case below is measuring.
std::unique_ptr<Mads::Agent> make_pub(std::string name, uint16_t port) {
  auto a = std::make_unique<Mads::Agent>(name, "none");
  a->init(false, false);
  a->set_pub_endpoint(mads_test::loopback(port));
  a->set_sub_endpoint(mads_test::loopback(port + 1));
  a->set_sub_topic({}); // pure publisher: skip connect_sub entirely
  a->set_pub_topic("probe");
  return a;
}

} // namespace

TEST_CASE("a single publish straight after connect() is not lost to the "
          "slow joiner",
          "[agent][slow_joiner]") {
  // The subscriber is fully up, and receiving, before the publisher exists --
  // the real fleet's situation: the agents were already running when
  // `mads-command` was typed.
  DrainedHub hub("hub", 44450);
  std::this_thread::sleep_for(300ms);

  auto pub = make_pub("pub", 44450);
  pub->connect(CONNECT_DELAY);
  // Exactly one send, with no retry loop to hide a dropped first message --
  // the whole point of the case.
  pub->publish(nlohmann::json{{"cmd", "info"}});

  REQUIRE(mads_test::wait_for([&] { return hub.got(); }, 2000ms));
  auto [topic, text] = hub.message();
  REQUIRE(topic == "probe");
  REQUIRE(nlohmann::json::parse(text)["cmd"] == "info");
}

TEST_CASE("connect() spends no more than its delay budget when no peer "
          "answers",
          "[agent][slow_joiner]") {
  // Nothing is bound at 44460, so the handshake never completes and the wait
  // runs to its timeout. The settle grace must come out of the same budget,
  // never on top of it: `delay` is the worst case the blind sleep already
  // cost, and callers still size their startup around it.
  auto pub = make_pub("orphan", 44460);

  auto const started = std::chrono::steady_clock::now();
  pub->connect(CONNECT_DELAY);
  auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);

  REQUIRE(elapsed >= CONNECT_DELAY);
  REQUIRE(elapsed < CONNECT_DELAY + SUBSCRIPTION_SETTLE_DELAY);
}

TEST_CASE("connect() returns early once the handshake and the grace are done",
          "[agent][slow_joiner]") {
  // The counterpart to the budget cap: a reachable peer must not cost the
  // full `delay`, or waiting on a real event would buy nothing over the blind
  // sleep it replaced. On loopback the handshake lands in single-digit
  // milliseconds, so the grace dominates -- generously bounded here, since
  // what matters is that it is the grace being paid and not the timeout.
  DrainedHub hub("hub", 44470);
  auto pub = make_pub("pub", 44470);

  auto const started = std::chrono::steady_clock::now();
  pub->connect(CONNECT_DELAY);
  auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);

  REQUIRE(elapsed >= SUBSCRIPTION_SETTLE_DELAY);
  REQUIRE(elapsed < CONNECT_DELAY);
}
