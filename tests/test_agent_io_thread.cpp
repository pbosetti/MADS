// Unit tests for Agent's unified I/O thread (ZMQ_DEVELOPMENT.md §4.1):
// LKV delivery and threaded remote control used to run on two independent
// threads, each calling recv() on the same (non-thread-safe) _subscriber --
// undefined behaviour, and with both features enabled at once, a real
// functional bug: whichever thread's recv() won the race got the message,
// so a "control" command could be silently stored as an ordinary LKV value
// instead of reaching remote_control(), and an ordinary message could be
// silently dropped by the remote-control thread instead of reaching the LKV
// slot. Both are now serviced by one zmq_poll()-based thread.
//
// The primary scenario here (per explicit request when this bout was
// implemented) is the one that actually matters in the field: a fast
// publisher (e.g. a 10ms sensor loop) feeding a slower LKV consumer (e.g. a
// 100ms filter). The consumer must see the LATEST value on every poll, never
// a stale one and never a backlog delivered one message at a time.
//
// Port range for this file: 44250-44299.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent.hpp"
#include "mads_test_helpers.hpp"

using namespace std::chrono_literals;

namespace {

// Hub: binds (cross=true) at loopback(port), publishing only. Mirrors
// test_agent_pubsub.cpp's make_pub().
std::unique_ptr<Mads::Agent> make_pub(std::string name, uint16_t port) {
  auto a = std::make_unique<Mads::Agent>(name, "none");
  a->init(false, false);
  a->set_cross(true);
  a->set_sub_endpoint(mads_test::loopback(port));
  a->set_sub_topic({});
  a->connect(0ms);
  return a;
}

// LKV subscriber: connects to loopback(port), subscribing only.
std::unique_ptr<Mads::Agent> make_lkv_sub(std::string name, uint16_t port,
                                          std::vector<std::string> topics = {""}) {
  auto a = std::make_unique<Mads::Agent>(name, "none");
  a->init(false, false);
  a->set_sub_endpoint(mads_test::loopback(port));
  a->set_pub_topic("");
  a->set_sub_topic(topics);
  a->set_delivery(Mads::Delivery::LastKnownValue);
  REQUIRE(a->delivery() == Mads::Delivery::LastKnownValue);
  a->connect(0ms);
  return a;
}

// Plain (queued) subscriber, for the threaded-remote-control-only cases.
std::unique_ptr<Mads::Agent> make_sub(std::string name, uint16_t port,
                                      std::vector<std::string> topics = {""}) {
  auto a = std::make_unique<Mads::Agent>(name, "none");
  a->init(false, false);
  a->set_sub_endpoint(mads_test::loopback(port));
  a->set_pub_topic("");
  a->set_sub_topic(topics);
  a->connect(0ms);
  return a;
}

// Absorbs the ZMQ slow-joiner delay: republishes a marker until the
// subscriber sees it, so tests below don't race the very first message.
template <typename PublishFn, typename CheckFn>
bool warm_up(PublishFn publish_once, CheckFn check_received,
            std::chrono::milliseconds total = 3000ms,
            std::chrono::milliseconds settle = 150ms) {
  auto deadline = std::chrono::steady_clock::now() + total;
  while (std::chrono::steady_clock::now() < deadline) {
    publish_once();
    if (mads_test::wait_for(check_received, settle, 10ms))
      return true;
  }
  return false;
}

} // namespace

// ---------------------------------------------------------------------------
// LKV: no backlog, ever
// ---------------------------------------------------------------------------

TEST_CASE("LKV never accumulates a backlog under a sustained fast publisher",
          "[agent_io_thread]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 44251;
  auto pub = make_pub("pubNoBacklog", port);
  auto sub = make_lkv_sub("subNoBacklog", port, {"data"});

  bool warm = warm_up(
      [&] { pub->publish(nlohmann::json{{"seq", -1}}, "data"); },
      [&] { return sub->receive(true) == Mads::message_type::json; });
  REQUIRE(warm);

  // Sustained fast publisher, ~10ms period, nobody draining the subscriber
  // meanwhile -- exactly the condition under which a real queue would pile
  // up a backlog.
  const int total = 30;
  for (int seq = 0; seq < total; ++seq) {
    pub->publish(nlohmann::json{{"seq", seq}}, "data");
    std::this_thread::sleep_for(10ms);
  }

  // Give the I/O thread a moment to drain the last publish into the slot.
  std::this_thread::sleep_for(50ms);

  auto mt = sub->receive(true);
  REQUIRE(mt == Mads::message_type::json);
  auto [topic, doc] = sub->last_json();
  REQUIRE(topic == "data");
  // The latest value, not the first -- allow a small margin for a message
  // still in flight at the exact moment publishing stopped.
  REQUIRE(doc.value("seq", -1) >= total - 2);

  // And there is nothing else queued behind it: a second immediate
  // non-blocking receive must find the slot empty. If LKV had degenerated
  // into a real queue, this would instead return the next of 30 pending
  // messages.
  REQUIRE(sub->receive(true) == Mads::message_type::none);
}

TEST_CASE("LKV subscriber polling at 100ms sees the latest of a 10ms "
          "publisher on every tick, never a backlog",
          "[agent_io_thread]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 44252;
  auto pub = make_pub("pub10ms", port);
  auto sub = make_lkv_sub("sub100ms", port, {"loop"});

  bool warm = warm_up(
      [&] { pub->publish(nlohmann::json{{"seq", -1}}, "loop"); },
      [&] { return sub->receive(true) == Mads::message_type::json; });
  REQUIRE(warm);

  std::atomic<bool> stop_pub{false};
  std::atomic<int> last_published{-1};
  std::thread pub_thread([&] {
    int seq = 0;
    while (!stop_pub.load()) {
      pub->publish(nlohmann::json{{"seq", seq}}, "loop");
      last_published.store(seq);
      ++seq;
      std::this_thread::sleep_for(10ms);
    }
  });

  // Let the publisher get ahead before the first 100ms poll.
  std::this_thread::sleep_for(150ms);

  std::vector<int> received;
  const int ticks = 8;
  for (int i = 0; i < ticks; ++i) {
    std::this_thread::sleep_for(100ms);
    if (sub->receive(true) == Mads::message_type::json) {
      auto [topic, doc] = sub->last_json();
      received.push_back(doc.value("seq", -1));
    }
  }

  stop_pub.store(true);
  pub_thread.join();

  // A fresh value on (almost) every tick -- generous floor for CI jitter.
  REQUIRE(received.size() >= size_t(ticks - 2));

  // Never stale, never repeated, never goes backward.
  for (size_t i = 1; i < received.size(); ++i) {
    REQUIRE(received[i] > received[i - 1]);
  }

  // The actual point: each 100ms tick must skip ahead by roughly the 10:1
  // publish:poll timing ratio, not deliver every intermediate value one at a
  // time the way a queue would. A gap of ~1 on every tick would mean LKV had
  // degenerated into a plain queue; the true expectation is close to 10.
  double gap_sum = 0;
  for (size_t i = 1; i < received.size(); ++i)
    gap_sum += received[i] - received[i - 1];
  double avg_gap = gap_sum / double(received.size() - 1);
  REQUIRE(avg_gap > 3.0);

  // The last tick must reach close to whatever was actually last published,
  // not some earlier snapshot -- the "1 message every 10, and it must be the
  // latest" requirement.
  REQUIRE(received.back() >= last_published.load() - 3);
}

// ---------------------------------------------------------------------------
// Threaded remote control, on its own
// ---------------------------------------------------------------------------

TEST_CASE("threaded remote control still handles a shutdown command through "
          "the unified I/O thread",
          "[agent_io_thread]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 44253;
  auto hub = make_pub("hubShutdown", port);

  auto target = std::make_unique<Mads::Agent>("targetShutdown", "none");
  target->init(false, false);
  target->set_sub_endpoint(mads_test::loopback(port));
  target->set_pub_topic("");
  target->set_sub_topic({}); // enable_remote_control(true) adds "control"
  target->enable_remote_control(true);
  target->connect(0ms);
  std::this_thread::sleep_for(300ms); // absorb PUB/SUB slow-joiner
  REQUIRE(Mads::Runtime::process_running().load());

  hub->publish(nlohmann::json{{"cmd", "shutdown"}}, "control");

  REQUIRE(mads_test::wait_for(
      [&] { return !Mads::Runtime::process_running().load(); }, 2000ms, 20ms));
}

TEST_CASE("threaded remote control still handles a restart command through "
          "the unified I/O thread",
          "[agent_io_thread]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 44254;
  auto hub = make_pub("hubRestart", port);

  auto target = std::make_unique<Mads::Agent>("targetRestart", "none");
  target->init(false, false);
  target->set_sub_endpoint(mads_test::loopback(port));
  target->set_pub_topic("");
  target->set_sub_topic({});
  target->enable_remote_control(true);
  target->connect(0ms);
  REQUIRE_FALSE(target->restart());

  bool warm = warm_up(
      [&] { hub->publish(nlohmann::json{{"cmd", "restart"}}, "control"); },
      [&] { return target->restart(); }, 1500ms, 100ms);
  REQUIRE(warm);
  REQUIRE_FALSE(Mads::Runtime::process_running().load());
}

// ---------------------------------------------------------------------------
// LKV and threaded remote control together: the actual bug being fixed
// ---------------------------------------------------------------------------

TEST_CASE("LKV and threaded remote control together no longer race, and "
          "control commands still get through under repetition",
          "[agent_io_thread]") {
  // Before this bout, enabling LKV (queue_size=1) together with threaded
  // remote control meant two independent threads both calling recv() on the
  // same non-thread-safe _subscriber -- undefined behaviour, observable in
  // practice as messages landing on whichever thread's recv() happened to
  // win the race. receive() itself has always refused to run while threaded
  // remote control owns the socket (Agent::receive()'s _rc_owns_socket
  // guard, unchanged here), so LKV values in this combination were never
  // retrievable through the public API either before or after this bout --
  // what actually changes is that the combination no longer corrupts shared
  // state or drops control commands under load. That, and repetition to
  // shake out the race the old two-thread design had, is what this pins.
  mads_test::RunningGuard guard;
  const uint16_t port = 44255;
  auto hub = make_pub("hubCombined", port);

  auto target = std::make_unique<Mads::Agent>("targetCombined", "none");
  target->init(false, false);
  target->set_sub_endpoint(mads_test::loopback(port));
  target->set_pub_topic("");
  target->set_sub_topic({"data"}); // enable_remote_control(true) adds "control"
  target->set_delivery(Mads::Delivery::LastKnownValue);
  target->enable_remote_control(true);
  target->connect(0ms);
  std::this_thread::sleep_for(300ms); // absorb PUB/SUB slow-joiner

  // The guard applies regardless of LKV: pin that explicitly here, in the
  // combined configuration, rather than only in isolation elsewhere.
  REQUIRE_THROWS_AS(target->receive(), Mads::AgentError);

  // Heavy, repeated interleaving of ordinary "data" traffic and "control"
  // traffic: the actual stress case for the race the unified I/O thread
  // removes. Nothing here should crash, hang, or corrupt _dropped_messages.
  for (int round = 0; round < 5; ++round) {
    for (int i = 0; i < 20; ++i) {
      hub->publish(nlohmann::json{{"seq", round * 20 + i}}, "data");
      hub->publish(nlohmann::json{{"cmd", "none"}}, "control");
    }
    std::this_thread::sleep_for(20ms);
  }

  REQUIRE(Mads::Runtime::process_running().load());

  // The point that matters most: a real shutdown command must still get
  // through reliably, even amid heavy interleaved LKV-bound traffic.
  hub->publish(nlohmann::json{{"cmd", "shutdown"}}, "control");
  REQUIRE(mads_test::wait_for(
      [&] { return !Mads::Runtime::process_running().load(); }, 2000ms, 20ms));
}
