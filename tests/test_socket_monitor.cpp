// Unit tests for Mads::SocketMonitor (src/socket_monitor.hpp), added in
// ZMQ_DEVELOPMENT.md §2.1: turns connection-lifecycle guesswork (a blind
// sleep, a bare receive timeout on a CURVE rejection) into real
// ZMQ_EVENT_* observations.
//
// Port range for this file: 44100-44149.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <thread>

#include <zmq.hpp>
#include <zmq_addon.hpp>

#include "mads_test_helpers.hpp"
#include "socket_monitor.hpp"
#include "zap_auth.hpp"

using namespace std::chrono_literals;

namespace {

// A REP server bound and answering on its own thread, so a REQ client has
// something to complete a ZMTP handshake against.
struct EchoServer {
  zmq::socket_t sock;
  std::thread thread;
  std::atomic<bool> stopped{false};

  EchoServer(zmq::context_t &ctx, const std::string &endpoint)
      : sock(ctx, zmq::socket_type::rep) {
    sock.set(zmq::sockopt::linger, 0);
    sock.set(zmq::sockopt::rcvtimeo, 100);
    sock.bind(endpoint);
    thread = std::thread([this] {
      while (!stopped.load()) {
        zmq::multipart_t m;
        if (!m.recv(sock)) continue;
        zmq::multipart_t r;
        r.addstr("pong");
        r.send(sock);
      }
    });
  }
  ~EchoServer() {
    stopped.store(true);
    if (thread.joinable()) thread.join();
    sock.close();
  }
};

} // namespace

TEST_CASE("wait_connected observes a real handshake against a live endpoint",
          "[socket_monitor]") {
  zmq::context_t ctx;
  EchoServer server(ctx, mads_test::loopback(44101));

  zmq::socket_t client(ctx, zmq::socket_type::req);
  client.set(zmq::sockopt::linger, 0);

  Mads::SocketMonitor monitor;
  monitor.start(client); // before connect(), per the documented contract
  client.connect(mads_test::loopback(44101));

  REQUIRE(monitor.wait_connected(2000ms));
  auto ev = monitor.last_event();
  REQUIRE((ev == Mads::LinkEvent::Connected ||
          ev == Mads::LinkEvent::HandshakeSucceeded));

  monitor.stop();
  client.close();
}

TEST_CASE("wait_connected times out against an unreachable endpoint",
          "[socket_monitor]") {
  zmq::context_t ctx;
  zmq::socket_t client(ctx, zmq::socket_type::req);
  client.set(zmq::sockopt::linger, 0);

  Mads::SocketMonitor monitor;
  monitor.start(client);
  client.connect(mads_test::loopback(44102)); // nothing bound there

  REQUIRE_FALSE(monitor.wait_connected(200ms));

  monitor.stop();
  client.close();
}

TEST_CASE("stop() is idempotent and safe without a prior start()",
          "[socket_monitor]") {
  Mads::SocketMonitor monitor;
  monitor.stop(); // never started
  monitor.stop(); // still safe

  zmq::context_t ctx;
  zmq::socket_t client(ctx, zmq::socket_type::req);
  monitor.start(client);
  monitor.stop();
  monitor.stop(); // safe after a real start()/stop() cycle too
  client.close();
}

TEST_CASE("a socket monitor does not stop the owning context from closing",
          "[socket_monitor]") {
  // The regression this pins: SocketMonitor::stop() must fully release the
  // monitor's own PAIR socket, or zmq::context_t::close() (zmq_ctx_term())
  // blocks forever waiting for it -- exactly the deadlock this test would
  // hang on if it ever came back.
  zmq::context_t ctx;
  zmq::socket_t client(ctx, zmq::socket_type::req);
  client.set(zmq::sockopt::linger, 0);

  Mads::SocketMonitor monitor;
  monitor.start(client);
  client.connect(mads_test::loopback(44103)); // unreachable is fine here
  monitor.stop();
  client.close();

  REQUIRE_NOTHROW(ctx.close());
}

TEST_CASE("an unauthorised CURVE client raises HandshakeFailedAuth",
          "[socket_monitor]") {
  zmq::context_t ctx;
  auto server_kp = Mads::generate_keypair();
  auto allowed_kp = Mads::generate_keypair();
  auto rogue_kp = Mads::generate_keypair(); // never allow-listed below

  Mads::ZapAuth zap(ctx);
  zap.configure_domain("*");
  zap.configure_curve(allowed_kp.public_key); // rogue_kp is deliberately absent
  zap.start();

  zmq::socket_t server(ctx, zmq::socket_type::rep);
  server.set(zmq::sockopt::linger, 0);
  server.set(zmq::sockopt::rcvtimeo, 100);
  server.set(zmq::sockopt::curve_server, 1);
  server.set(zmq::sockopt::curve_secretkey, server_kp.secret_key);
  server.bind(mads_test::loopback(44104));
  std::atomic<bool> stopped{false};
  std::thread server_thread([&] {
    while (!stopped.load()) {
      zmq::multipart_t m;
      m.recv(server);
    }
  });

  zmq::socket_t rogue(ctx, zmq::socket_type::req);
  rogue.set(zmq::sockopt::linger, 0);
  rogue.set(zmq::sockopt::curve_publickey, rogue_kp.public_key);
  rogue.set(zmq::sockopt::curve_secretkey, rogue_kp.secret_key);
  rogue.set(zmq::sockopt::curve_serverkey, server_kp.public_key);

  Mads::SocketMonitor monitor;
  monitor.start(rogue);
  rogue.connect(mads_test::loopback(44104));

  // wait_connected() alone would report success here: ZMQ_EVENT_CONNECTED
  // fires at the TCP level before ZAP even sees the key, so it is not a
  // signal that the handshake itself succeeded. wait_handshake_succeeded()
  // is the one that must see through the rejection.
  REQUIRE_FALSE(monitor.wait_handshake_succeeded(2000ms));
  REQUIRE(monitor.last_event() == Mads::LinkEvent::HandshakeFailedAuth);

  monitor.stop();
  rogue.close();
  stopped.store(true);
  server_thread.join();
  server.close();
  zap.stop();
}

TEST_CASE("an authorised CURVE client observes HandshakeSucceeded",
          "[socket_monitor]") {
  zmq::context_t ctx;
  auto server_kp = Mads::generate_keypair();
  auto client_kp = Mads::generate_keypair();

  Mads::ZapAuth zap(ctx);
  zap.configure_domain("*");
  zap.configure_curve(client_kp.public_key);
  zap.start();

  zmq::socket_t server(ctx, zmq::socket_type::rep);
  server.set(zmq::sockopt::linger, 0);
  server.set(zmq::sockopt::rcvtimeo, 100);
  server.set(zmq::sockopt::curve_server, 1);
  server.set(zmq::sockopt::curve_secretkey, server_kp.secret_key);
  server.bind(mads_test::loopback(44105));
  std::atomic<bool> stopped{false};
  std::thread server_thread([&] {
    while (!stopped.load()) {
      zmq::multipart_t m;
      m.recv(server);
    }
  });

  zmq::socket_t client(ctx, zmq::socket_type::req);
  client.set(zmq::sockopt::linger, 0);
  client.set(zmq::sockopt::curve_publickey, client_kp.public_key);
  client.set(zmq::sockopt::curve_secretkey, client_kp.secret_key);
  client.set(zmq::sockopt::curve_serverkey, server_kp.public_key);

  Mads::SocketMonitor monitor;
  monitor.start(client);
  client.connect(mads_test::loopback(44105));

  REQUIRE(monitor.wait_handshake_succeeded(2000ms));
  REQUIRE(monitor.last_event() == Mads::LinkEvent::HandshakeSucceeded);

  monitor.stop();
  client.close();
  stopped.store(true);
  server_thread.join();
  server.close();
  zap.stop();
}
