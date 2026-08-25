// Unit tests for Agent::link_state() (ZMQ_DEVELOPMENT.md §2.1 remainder).
//
// SocketMonitor already reported every ZMQ_EVENT_*, but nothing consumed
// them: an agent whose broker vanished kept retrying silently and had no way
// to say so. link_state() is the accessor that closes that gap, and
// `mads top`'s status bar is its first consumer -- which is why the two
// things asserted here are the two things that bar has to get right: it must
// go Down when the peer really goes away, and it must not claim to know
// anything in the one topology where the underlying events cannot be
// condensed into a single link (set_cross(), where the agent binds).
//
// Port range for this file: 44300-44349.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>

#include "agent.hpp"
#include "mads_test_helpers.hpp"

using namespace std::chrono_literals;

namespace {

// Both endpoints are always set explicitly, never left at the compiled-in
// tcp://localhost:9090/9091 defaults: a cross agent binds one of them, and a
// default would put that bind outside this file's port range and on top of
// any real broker running on the machine.
void set_endpoints(Mads::Agent &a, uint16_t pub_port, uint16_t sub_port) {
  a.set_pub_endpoint(mads_test::loopback(pub_port));
  a.set_sub_endpoint(mads_test::loopback(sub_port));
}

// Publishing hub: cross=true means connect_pub() BINDS at `port`, so an
// ordinary subscriber Agent can connect to it as it would to a broker.
std::unique_ptr<Mads::Agent> make_pub_hub(std::string name, uint16_t port) {
  auto a = std::make_unique<Mads::Agent>(name, "none");
  a->init(false, false);
  a->set_cross(true);
  set_endpoints(*a, port + 1, port);
  a->set_sub_topic({}); // publisher only: connect_sub() never runs
  a->connect(0ms);
  return a;
}

// Subscribing hub: cross=true makes connect_sub() BIND at `port`, giving a
// publish-only Agent a real peer to complete a ZMTP handshake against.
std::unique_ptr<Mads::Agent> make_sub_hub(std::string name, uint16_t port) {
  auto a = std::make_unique<Mads::Agent>(name, "none");
  a->init(false, false);
  a->set_cross(true);
  set_endpoints(*a, port, port + 1);
  a->set_pub_topic(""); // subscriber only: connect_pub() never runs
  a->set_sub_topic({""});
  a->connect(0ms);
  return a;
}

std::unique_ptr<Mads::Agent> make_sub(std::string name, uint16_t port) {
  auto a = std::make_unique<Mads::Agent>(name, "none");
  a->init(false, false);
  set_endpoints(*a, port + 1, port);
  a->set_pub_topic("");
  a->set_sub_topic({""});
  a->connect(0ms);
  return a;
}

} // namespace

TEST_CASE("link_state() reports Up once a subscriber reaches its peer",
          "[agent][link_state]") {
  auto hub = make_pub_hub("hub", 44300);
  auto sub = make_sub("sub", 44300);

  REQUIRE(mads_test::wait_for(
      [&] { return sub->link_state().status == Mads::LinkStatus::Up; },
      3000ms));
  auto st = sub->link_state();
  REQUIRE(st.last_handshake == Mads::LinkEvent::HandshakeSucceeded);
  REQUIRE(st.drops == 0);
  REQUIRE(st.changed_at.has_value());
}

TEST_CASE("link_state() goes Down when the broker vanishes",
          "[agent][link_state]") {
  auto hub = make_pub_hub("hub", 44302);
  auto sub = make_sub("sub", 44302);

  REQUIRE(mads_test::wait_for(
      [&] { return sub->link_state().status == Mads::LinkStatus::Up; },
      3000ms));

  hub.reset(); // the exact situation that used to be undetectable

  REQUIRE(mads_test::wait_for(
      [&] { return sub->link_state().status == Mads::LinkStatus::Down; },
      3000ms));
  REQUIRE(sub->link_state().drops == 1);
}

TEST_CASE("a publish-only agent falls back to its publisher's link",
          "[agent][link_state]") {
  // No sub_topic means connect_sub() -- and with it the subscriber's monitor
  // -- never runs, so the publisher is the only socket that can answer.
  auto hub = make_sub_hub("hub", 44304);

  auto pub = std::make_unique<Mads::Agent>("pub", "none");
  pub->init(false, false);
  set_endpoints(*pub, 44304, 44305);
  pub->set_sub_topic({});
  pub->set_pub_topic("probe");
  pub->connect(0ms);

  REQUIRE(mads_test::wait_for(
      [&] { return pub->link_state().status == Mads::LinkStatus::Up; },
      3000ms));
}

TEST_CASE("link_state() reports Unknown for a cross (binding) agent",
          "[agent][link_state]") {
  // A bound socket gets ZMQ_EVENT_DISCONNECTED per departing peer and no
  // handshake event per arriving one, so the events cannot be condensed into
  // one link status. Reporting Unknown is the honest answer; the alternative
  // is a hub that flips to "down" the moment any one of its subscribers
  // quits.
  auto hub = make_pub_hub("hub", 44306);
  auto sub = make_sub("sub", 44306);

  REQUIRE(mads_test::wait_for(
      [&] { return sub->link_state().status == Mads::LinkStatus::Up; },
      3000ms));
  // The subscriber is up, so the hub has definitely seen peer activity --
  // and still reports nothing, by design.
  auto st = hub->link_state();
  REQUIRE(st.status == Mads::LinkStatus::Unknown);
  REQUIRE(st.drops == 0);
  REQUIRE_FALSE(st.changed_at.has_value());

  sub.reset(); // a departing peer must not make the hub look broken
  std::this_thread::sleep_for(200ms);
  REQUIRE(hub->link_state().status == Mads::LinkStatus::Unknown);
}

TEST_CASE("link_state() is Unknown before connect()", "[agent][link_state]") {
  Mads::Agent a("idle", "none");
  a.init(false, false);
  set_endpoints(a, 44309, 44308);
  REQUIRE(a.link_state().status == Mads::LinkStatus::Unknown);
  REQUIRE(a.link_state().last_event == Mads::LinkEvent::None);
  REQUIRE(a.link_state().last_handshake == Mads::LinkEvent::None);
}
