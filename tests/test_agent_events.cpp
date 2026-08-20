// Unit tests for Agent::register_event() lifecycle safety (src/agent.cpp).
// The startup event is published from a delayed thread owned by the agent;
// these tests guard the historical use-after-free where that thread was
// detached and could outlive a short-lived agent, and verify that events
// actually reach a subscriber on the METADATA_TOPIC ("agent_event") topic.
// Port range: 42400-42499.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "agent.hpp"
#include "mads_test_helpers.hpp"

using namespace std::chrono_literals;

namespace {

// Publisher agent paired with a subscriber listening on METADATA_TOPIC.
struct EventHarness {
  std::unique_ptr<Mads::Agent> pub;
  std::unique_ptr<Mads::Agent> sub;

  explicit EventHarness(uint16_t port, const std::string &name) {
    pub = std::make_unique<Mads::Agent>(name, "none");
    pub->init(false, false);
    pub->set_cross(true);
    pub->set_sub_endpoint(mads_test::loopback(port));
    // set_cross(true) makes connect_sub() BIND using _pub_endpoint's port
    // (src/agent.cpp Agent::connect_sub). Without this it would be the
    // compiled-in default 9090 -- outside this file's declared range, and a
    // collision with any real broker running on the machine.
    pub->set_pub_endpoint(mads_test::loopback(port + 50));
    pub->connect(0ms);

    sub = std::make_unique<Mads::Agent>(name + "_sub", "none");
    sub->init(false, false);
    sub->set_sub_endpoint(mads_test::loopback(port));
    sub->set_pub_topic("");
    sub->set_sub_topic({METADATA_TOPIC});
    sub->connect(0ms);

    // Absorb the ZMQ slow-joiner delay before the test relies on delivery.
    bool warm = mads_test::wait_for(
        [&] {
          pub->register_event(Mads::event_type::marker); // synchronous publish
          return mads_test::wait_for(
              [&] { return sub->receive(true) == Mads::message_type::json; },
              150ms, 10ms);
        },
        3000ms, 200ms);
    REQUIRE(warm);
  }

  // Wait for the next event with the given name on METADATA_TOPIC.
  bool wait_event(const std::string &event_name,
                  std::chrono::milliseconds timeout = 2000ms) {
    return mads_test::wait_for(
        [&] {
          if (sub->receive(true) != Mads::message_type::json)
            return false;
          auto [topic, doc] = sub->last_json();
          return topic == METADATA_TOPIC && doc.value("event", "") == event_name;
        },
        timeout, 10ms);
  }
};

} // namespace

TEST_CASE("startup event registered then immediate shutdown joins promptly "
          "and does not crash",
          "[agent_events]") {
  auto t0 = std::chrono::steady_clock::now();
  {
    Mads::Agent a("evA", "none");
    a.init(false, false);
    a.set_cross(true);
    a.set_sub_endpoint(mads_test::loopback(42401));
    a.set_pub_endpoint(mads_test::loopback(42451)); // see EventHarness
    a.connect(0ms);
    a.register_event(Mads::event_type::startup);
    // Destruction runs shutdown(), which must wake the delayed publisher
    // thread and join it while the sockets are still open. Historically the
    // thread was detached and this pattern was a use-after-free.
  }
  auto elapsed = std::chrono::steady_clock::now() - t0;
  // The join must be prompt (cv wake), not a full 500 ms delay per agent,
  // and certainly not a hang. Generous bound to avoid flakes.
  REQUIRE(elapsed < 3000ms);
  SUCCEED("agent destroyed cleanly with a pending startup event");
}

TEST_CASE("startup event registered then disconnect() joins the publisher "
          "thread",
          "[agent_events]") {
  Mads::Agent a("evB", "none");
  a.init(false, false);
  a.set_cross(true);
  a.set_sub_endpoint(mads_test::loopback(42402));
  a.set_pub_endpoint(mads_test::loopback(42452)); // see EventHarness
  a.connect(0ms);
  a.register_event(Mads::event_type::startup);
  a.disconnect(); // joins the event thread before touching the sockets
  SUCCEED("disconnect completed with a pending startup event");
}

TEST_CASE("startup event is delivered to a metadata subscriber",
          "[agent_events]") {
  EventHarness h(42403, "evC");
  h.pub->register_event(Mads::event_type::startup);
  // Delivery happens after the ~500 ms startup delay.
  REQUIRE(h.wait_event("startup"));
  auto [topic, doc] = h.sub->last_json();
  REQUIRE(doc.at("name") == "evC");
  REQUIRE(doc.contains("settings"));
}

TEST_CASE("non-startup events are published synchronously",
          "[agent_events]") {
  EventHarness h(42404, "evD");
  nlohmann::json info = {{"note", "sync"}};
  h.pub->register_event(Mads::event_type::message, info);
  REQUIRE(h.wait_event("message", 1000ms));
  auto [topic, doc] = h.sub->last_json();
  REQUIRE(doc.at("info").at("note") == "sync");
}

TEST_CASE("registering startup twice joins the previous publisher first",
          "[agent_events]") {
  EventHarness h(42405, "evE");
  h.pub->register_event(Mads::event_type::startup);
  h.pub->register_event(Mads::event_type::startup); // joins the first thread
  REQUIRE(h.wait_event("startup"));
  SUCCEED("second registration did not crash or leak the first thread");
}
