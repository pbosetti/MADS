// Unit tests for Mads::Worker::pull() (ZMQ_DEVELOPMENT.md §4.1 remainder).
//
// pull() used to be an unbounded blocking recv() on the PULL socket: with
// no work queued, an idle worker's loop never came back from it, so it
// never reached its own receive() -- and receive() is what dispatches the
// broker's `control` topic. A fleet-wide shutdown or restart therefore
// never reached an idle worker at all. pull() is now polled with a
// deadline, returning an empty object when nothing arrives -- the same
// contract receive() already had.
//
// Port range for this file: 44350-44399 (see tests/fixtures/settings/worker.toml).
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>
#include <zmq.hpp>
#include <zmq_addon.hpp>

#include "mads_test_helpers.hpp"
#include "worker.hpp"

using namespace std::chrono_literals;
using json = nlohmann::json;

namespace {

std::string fixture() {
  return std::string(MADS_TEST_FIXTURES_DIR) + "/settings/worker.toml";
}

std::unique_ptr<Mads::Worker> make_worker() {
  auto w = std::make_unique<Mads::Worker>("pulltest", fixture());
  w->init(false, false);
  w->connect(0ms);
  return w;
}

// The dealer's side of the PUSH/PULL pair: binds where the fixture points
// the worker's PULL socket.
struct DealerStub {
  zmq::context_t ctx;
  zmq::socket_t sock;
  DealerStub() : sock(ctx, zmq::socket_type::push) {
    sock.set(zmq::sockopt::linger, 0);
    sock.bind(mads_test::loopback(44352));
  }
  void send(const std::string &payload) {
    zmq::multipart_t m;
    m.addstr(payload);
    m.send(sock);
  }
  ~DealerStub() { sock.close(); }
};

} // namespace

TEST_CASE("pull() returns empty instead of blocking when there is no work",
          "[worker][pull]") {
  auto w = make_worker();
  w->set_receive_timeout(200);

  const auto started = std::chrono::steady_clock::now();
  json got = w->pull();
  const auto elapsed = std::chrono::steady_clock::now() - started;

  REQUIRE(got.empty());
  // The point of the test is that it returns at all; the bound just pins
  // that it waited roughly its own timeout rather than either spinning or
  // hanging (the old behaviour would never reach this line).
  REQUIRE(elapsed >= 150ms);
  REQUIRE(elapsed < 2000ms);
}

TEST_CASE("pull(0ms) polls without blocking", "[worker][pull]") {
  auto w = make_worker();
  w->set_receive_timeout(5000); // deliberately large: must not be consulted

  const auto started = std::chrono::steady_clock::now();
  json got = w->pull(0ms);
  const auto elapsed = std::chrono::steady_clock::now() - started;

  REQUIRE(got.empty());
  REQUIRE(elapsed < 500ms);
}

TEST_CASE("pull() still delivers a work item pushed by the dealer",
          "[worker][pull]") {
  DealerStub dealer;
  auto w = make_worker();
  w->set_receive_timeout(200);

  // PUSH/PULL needs the connection established before a send is routed
  // anywhere; without a peer, PUSH blocks or queues rather than delivering.
  REQUIRE(mads_test::wait_for([&] {
    dealer.send(R"({"value":42})");
    json got = w->pull(100ms);
    return !got.empty() && got.value("value", 0) == 42;
  }, 4000ms, 50ms));
}

TEST_CASE("pull() reports malformed JSON as an error payload",
          "[worker][pull]") {
  DealerStub dealer;
  auto w = make_worker();
  w->set_receive_timeout(200);

  json got;
  REQUIRE(mads_test::wait_for([&] {
    dealer.send("{not json at all");
    got = w->pull(100ms);
    return !got.empty();
  }, 4000ms, 50ms));
  // A parse failure is reported, never fatal, and never mistaken for "no
  // work" -- so a caller can tell the two apart.
  REQUIRE(got.contains("error"));
}

TEST_CASE("an idle worker still services control messages between pulls",
          "[worker][pull]") {
  // The behaviour the bounded pull() actually buys. A worker's loop is
  // `pull(); receive();` -- and receive() is what dispatches the broker's
  // `control` topic to remote_control(). While pull() blocked indefinitely,
  // an idle worker never reached receive() at all, so a fleet-wide
  // shutdown/restart command simply never arrived. (A local Ctrl-C did get
  // through, but only because the signal interrupted the blocking recv()
  // with EINTR, which surfaced as an exception out of the loop.)
  mads_test::RunningGuard guard;

  // Publishing hub in place of a broker: cross=true makes connect_pub()
  // bind, so the worker's subscriber can connect to it.
  Mads::Agent hub("hub", "none");
  hub.init(false, false);
  hub.set_cross(true);
  hub.set_pub_endpoint(mads_test::loopback(44354));
  hub.set_sub_endpoint(mads_test::loopback(44353));
  hub.set_sub_topic({});
  hub.connect(0ms);

  Mads::Worker w("pulltest", fixture());
  w.init(false, false);
  w.set_sub_endpoint(mads_test::loopback(44353));
  w.set_pub_endpoint(mads_test::loopback(44354));
  w.set_receive_timeout(100);
  w.enable_remote_control(); // non-threaded: dispatched from receive()
  w.connect(0ms);

  // Nothing is bound at the fixture's dealer_address, so every pull() here
  // is an idle one -- exactly the case that used to block forever.
  const bool stopped = mads_test::wait_for([&] {
    hub.publish(json{{"cmd", "shutdown"}}, "control");
    json work = w.pull();
    REQUIRE(work.empty());
    w.receive();
    return !Mads::Runtime::process_running();
  }, 5000ms, 0ms);

  REQUIRE(stopped);
}
