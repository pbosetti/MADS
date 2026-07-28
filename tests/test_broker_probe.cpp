// Unit tests for src/broker_probe.hpp/.cpp: Mads::probe_broker() (backs
// `ready = "broker"`) and Mads::probe_tcp_port() (backs `ready = "port:<n>"`).
// Reuses the same fake-broker pattern as tests/test_agent_broker.cpp (a
// std::thread running a zmqpp REP socket on loopback), in this suite's own
// port range (42600-42699, per mads_test_helpers.hpp).
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include <zmqpp/zmqpp.hpp>

#include "broker_probe.hpp"
#include "mads_test_helpers.hpp"

using namespace std::chrono_literals;

namespace {

// A minimal REP-socket "broker": replies to *any* request with a
// well-formed 2-part reply, since probe_broker() only needs proof that
// something is there and speaking back -- it never inspects the payload.
class FakeBroker {
public:
  explicit FakeBroker(uint16_t port)
      : _ctx(), _sock(_ctx, zmqpp::socket_type::rep) {
    _sock.set(zmqpp::socket_option::receive_timeout, 100);
    _sock.bind(mads_test::loopback(port));
  }

  ~FakeBroker() { stop(); }

  void start() {
    _thread = std::thread([this] { run(); });
  }

  void stop() {
    if (_stopped.exchange(true)) return;
    if (_thread.joinable()) _thread.join();
  }

private:
  void run() {
    while (!_stopped) {
      zmqpp::message msg;
      if (!_sock.receive(msg)) continue;
      zmqpp::message reply;
      reply << std::string("v0.0") << std::string("{}");
      _sock.send(reply);
    }
  }

  zmqpp::context _ctx;
  zmqpp::socket _sock;
  std::thread _thread;
  std::atomic<bool> _stopped{false};
};

} // namespace

TEST_CASE("probe_broker succeeds against a reachable broker", "[broker_probe]") {
  const uint16_t port = 42600;
  FakeBroker broker(port);
  broker.start();

  REQUIRE(Mads::probe_broker(mads_test::loopback(port), 2000ms));
}

TEST_CASE("probe_broker times out against an unreachable broker",
         "[broker_probe]") {
  const uint16_t port = 42601; // intentionally nothing bound here
  const auto started = std::chrono::steady_clock::now();
  const bool ok = Mads::probe_broker(mads_test::loopback(port), 300ms);
  const auto elapsed = std::chrono::steady_clock::now() - started;

  REQUIRE_FALSE(ok);
  REQUIRE(elapsed < 3s); // bounded by the requested timeout, not a hang
}

TEST_CASE("probe_tcp_port succeeds against a listening port", "[broker_probe]") {
  const uint16_t port = 42602;
  FakeBroker broker(port); // any real bound TCP listener will do
  broker.start();

  REQUIRE(Mads::probe_tcp_port("127.0.0.1", port, 2000ms));
}

TEST_CASE("probe_tcp_port times out against a closed port", "[broker_probe]") {
  const uint16_t port = 42603;
  const auto started = std::chrono::steady_clock::now();
  const bool ok = Mads::probe_tcp_port("127.0.0.1", port, 300ms);
  const auto elapsed = std::chrono::steady_clock::now() - started;

  REQUIRE_FALSE(ok);
  REQUIRE(elapsed < 3s);
}
