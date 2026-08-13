// Pins the semantics of libzmq's steerable-proxy commands, which mads-broker's
// interactive 'p'/'r' keys depend on.
//
// libzmq 4.3.5 has them inverted (src/proxy.cpp):
//
//   if (msiz == 5 && memcmp (command, "\x05PAUSE", 6))        state = active;
//   else if (msiz == 6 && 0 == memcmp (command, "RESUME", 6)) state = paused;
//
// The PAUSE arm is missing its `0 ==` and compares six bytes (with a stray
// \x05) against a five-byte command, so it is always true and selects
// `active`. The RESUME arm matches correctly but selects `paused`.
//
// broker.cpp therefore sends the opposite command of the effect it wants. If a
// libzmq upgrade ever fixes this upstream, this suite fails and broker.cpp's
// two `steer()` calls must be swapped back.
//
// Port range for this file: 43900-43999.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include <zmq.hpp>
#include <zmq_addon.hpp>

#include "mads_test_helpers.hpp"

using namespace std::chrono_literals;

namespace {

// Mirrors broker.cpp's proxy() and steer() helpers.
void run_proxy(zmq::socket_t &frontend, zmq::socket_t &backend,
               zmq::socket_t &ctrl) {
  zmq::proxy_steerable(frontend, backend, zmq::socket_ref(), ctrl);
}

zmq::multipart_t steer(zmq::socket_t &controller, std::string_view command) {
  controller.send(zmq::buffer(command), zmq::send_flags::none);
  zmq::multipart_t reply;
  reply.recv(controller);
  return reply;
}

// A steerable XSUB/XPUB proxy with a publisher and subscriber attached, so a
// test can ask "is traffic flowing right now?".
struct ProxyHarness {
  zmq::context_t ctx;
  zmq::socket_t frontend{ctx, zmq::socket_type::xsub};
  zmq::socket_t backend{ctx, zmq::socket_type::xpub};
  zmq::socket_t controlled{ctx, zmq::socket_type::rep};
  zmq::socket_t controller{ctx, zmq::socket_type::req};
  zmq::socket_t pub{ctx, zmq::socket_type::pub};
  zmq::socket_t sub{ctx, zmq::socket_type::sub};
  std::thread thread;

  ProxyHarness(uint16_t front_port, uint16_t back_port) {
    frontend.bind(mads_test::loopback(front_port));
    backend.bind(mads_test::loopback(back_port));
    controlled.bind("inproc://test-broker-ctrl");
    controller.connect("inproc://test-broker-ctrl");
    thread = std::thread(run_proxy, std::ref(frontend), std::ref(backend),
                         std::ref(controlled));

    pub.connect(mads_test::loopback(front_port));
    sub.set(zmq::sockopt::subscribe, "");
    sub.set(zmq::sockopt::rcvtimeo, 100);
    sub.connect(mads_test::loopback(back_port));
    std::this_thread::sleep_for(500ms); // connect + subscription propagation
  }

  ~ProxyHarness() {
    steer(controller, "TERMINATE");
    if (thread.joinable()) thread.join();
    pub.close();
    sub.close();
    controller.close();
    controlled.close();
    frontend.close();
    backend.close();
  }

  // Publishes exactly `n` two-frame messages ("t" + a 10-byte body) and
  // returns how many came back, so a test can predict the counters exactly.
  int pump_exactly(int n) {
    int received = 0;
    for (int i = 0; i < n; ++i) {
      zmq::multipart_t out;
      out.addstr("t");
      out.addstr("0123456789");
      out.send(pub);
      zmq::multipart_t in;
      if (in.recv(sub)) received++;
      std::this_thread::sleep_for(30ms);
    }
    return received;
  }

  // Publishes for `ms` and returns how many messages made it through.
  int pump(int ms) {
    int received = 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
      zmq::multipart_t out;
      out.addstr("t");
      out.addstr("payload");
      out.send(pub);
      zmq::multipart_t in;
      if (in.recv(sub)) received++;
      std::this_thread::sleep_for(10ms);
    }
    return received;
  }
};

} // namespace

TEST_CASE("the wire command mads-broker sends for 'pause' actually stops "
          "traffic",
          "[broker_steering]") {
  ProxyHarness h(43901, 43902);
  REQUIRE(h.pump(400) > 0); // baseline: the proxy forwards

  // What broker.cpp's 'p' key sends.
  steer(h.controller, "RESUME");
  std::this_thread::sleep_for(200ms);
  h.pump(200); // drain whatever was already queued
  REQUIRE(h.pump(400) == 0);

  // What broker.cpp's 'r' key sends.
  steer(h.controller, "PAUSE");
  std::this_thread::sleep_for(200ms);
  REQUIRE(h.pump(400) > 0);
}

TEST_CASE("libzmq's PAUSE/RESUME are still inverted; broker.cpp compensates",
          "[broker_steering]") {
  ProxyHarness h(43903, 43904);
  REQUIRE(h.pump(400) > 0);

  // Taken literally, "PAUSE" does NOT pause on the pinned libzmq. If this
  // assertion ever fails, upstream fixed the bug: swap the two steer() calls
  // in src/main/broker.cpp back to the literal commands and update the
  // comments there and here.
  steer(h.controller, "PAUSE");
  std::this_thread::sleep_for(200ms);
  REQUIRE(h.pump(400) > 0);
}

TEST_CASE("STATISTICS returns the eight counter frames the broker prints",
          "[broker_steering]") {
  ProxyHarness h(43905, 43906);
  h.pump(200);

  auto stats = steer(h.controller, "STATISTICS");
  // broker.cpp indexes stats[0..7] (messages/bytes in/out for both sockets).
  REQUIRE(stats.size() == 8);
  for (size_t i = 0; i < stats.size(); ++i) {
    REQUIRE(stats.at(i).size() == sizeof(uint64_t));
  }
}

// The counters are NATIVE-endian uint64_t. Reading them with a byte-order
// conversion applied (as broker.cpp did briefly during the cppzmq migration,
// where a leftover htonll() no longer had zmqpp's ntohll() to cancel it)
// turns a count of 14 into 1008806316530991104. Pinning plausible magnitudes
// catches that class of bug, which asserting on frame *sizes* alone does not.
TEST_CASE("STATISTICS counters are native-endian and plausibly sized",
          "[broker_steering]") {
  ProxyHarness h(43907, 43908);

  // Each message is two frames ("t" + a 10-byte body) = 11 bytes.
  const int sent = h.pump_exactly(7);
  REQUIRE(sent == 7);

  auto stats = steer(h.controller, "STATISTICS");
  REQUIRE(stats.size() == 8);

  auto counter = [&](size_t i) {
    uint64_t v = 0;
    std::memcpy(&v, stats.at(i).data(), sizeof(v));
    return v;
  };

  // Frontend messages-in counts frames, not messages: 7 x 2 parts.
  const uint64_t frontend_msgs_in = counter(0);
  const uint64_t frontend_bytes_in = counter(1);
  REQUIRE(frontend_msgs_in == 14);
  REQUIRE(frontend_bytes_in == 7 * 11);

  // Every counter must stay in a range a human would recognise. A byte-swapped
  // small number lands astronomically high, so this is the assertion that
  // fails loudly if a conversion ever creeps back in.
  for (size_t i = 0; i < stats.size(); ++i) {
    INFO("counter " << i << " = " << counter(i));
    REQUIRE(counter(i) < 1000000u);
  }
}
