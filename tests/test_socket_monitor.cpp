// Unit tests for Mads::SocketMonitor (src/socket_monitor.hpp), added in
// ZMQ_DEVELOPMENT.md §2.1: turns connection-lifecycle guesswork (a blind
// sleep, a bare receive timeout on a CURVE rejection) into real
// ZMQ_EVENT_* observations.
//
// Port range for this file: 44100-44149.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <functional>
#include <memory>
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
  // last_handshake, not last_event: libzmq fires DISCONNECTED straight after
  // the rejection and then retries, so which event is "last" here is a race
  // -- one this assertion lost on Linux until the outcome became sticky.
  REQUIRE(monitor.state().last_handshake ==
          Mads::LinkEvent::HandshakeFailedAuth);

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
  REQUIRE(monitor.state().last_handshake ==
          Mads::LinkEvent::HandshakeSucceeded);

  monitor.stop();
  client.close();
  stopped.store(true);
  server_thread.join();
  server.close();
  zap.stop();
}

// --- state(): the events condensed into a link status (§2.1 remainder) ---
//
// last_event() answers "what happened last"; state() answers "is the link
// usable right now, and how often has it dropped" -- which is what a caller
// such as `mads top`'s status bar reports.

TEST_CASE("state() starts Unknown and rises to Up on a real handshake",
          "[socket_monitor]") {
  zmq::context_t ctx;
  zmq::socket_t client(ctx, zmq::socket_type::req);
  client.set(zmq::sockopt::linger, 0);

  Mads::SocketMonitor monitor;
  REQUIRE(monitor.state().status == Mads::LinkStatus::Unknown);
  REQUIRE_FALSE(monitor.state().changed_at.has_value());

  monitor.start(client);
  EchoServer server(ctx, mads_test::loopback(44106));
  client.connect(mads_test::loopback(44106));

  REQUIRE(mads_test::wait_for(
      [&] { return monitor.state().status == Mads::LinkStatus::Up; }, 2000ms));
  auto st = monitor.state();
  REQUIRE(st.last_event == Mads::LinkEvent::HandshakeSucceeded);
  REQUIRE(st.drops == 0);
  REQUIRE(st.recoveries == 0); // a first connection is not a recovery
  REQUIRE(st.changed_at.has_value());

  monitor.stop();
  client.close();
}

TEST_CASE("state() goes Down and counts a drop when the peer disappears",
          "[socket_monitor]") {
  zmq::context_t ctx;
  auto server = std::make_unique<EchoServer>(ctx, mads_test::loopback(44107));

  zmq::socket_t client(ctx, zmq::socket_type::req);
  client.set(zmq::sockopt::linger, 0);
  Mads::SocketMonitor monitor;
  monitor.start(client);
  client.connect(mads_test::loopback(44107));

  REQUIRE(mads_test::wait_for(
      [&] { return monitor.state().status == Mads::LinkStatus::Up; }, 2000ms));
  const auto up_at = monitor.state().changed_at;

  server.reset(); // the "broker" vanishes

  REQUIRE(mads_test::wait_for(
      [&] { return monitor.state().status == Mads::LinkStatus::Down; },
      2000ms));
  auto st = monitor.state();
  REQUIRE(st.drops == 1);
  REQUIRE(st.recoveries == 0);
  REQUIRE(st.changed_at.has_value());
  REQUIRE(*st.changed_at > *up_at); // the timestamp tracks the transition

  monitor.stop();
  client.close();
}

TEST_CASE("a peer that stays away is one drop, not one per retry",
          "[socket_monitor]") {
  // libzmq keeps retrying a dead endpoint every reconnect_ivl (100ms by
  // default), firing ZMQ_EVENT_CONNECT_RETRIED each time. state() counts
  // transitions, so all of those collapse into the single drop that
  // actually happened.
  zmq::context_t ctx;
  auto server = std::make_unique<EchoServer>(ctx, mads_test::loopback(44108));

  zmq::socket_t client(ctx, zmq::socket_type::req);
  client.set(zmq::sockopt::linger, 0);
  Mads::SocketMonitor monitor;
  monitor.start(client);
  client.connect(mads_test::loopback(44108));

  REQUIRE(mads_test::wait_for(
      [&] { return monitor.state().status == Mads::LinkStatus::Up; }, 2000ms));
  server.reset();
  REQUIRE(mads_test::wait_for(
      [&] { return monitor.state().status == Mads::LinkStatus::Down; },
      2000ms));

  std::this_thread::sleep_for(700ms); // several reconnect intervals
  auto st = monitor.state();
  REQUIRE(st.status == Mads::LinkStatus::Down);
  REQUIRE(st.drops == 1);

  monitor.stop();
  client.close();
}

TEST_CASE("state() counts a recovery when the peer comes back",
          "[socket_monitor]") {
  zmq::context_t ctx;
  auto server = std::make_unique<EchoServer>(ctx, mads_test::loopback(44109));

  zmq::socket_t client(ctx, zmq::socket_type::req);
  client.set(zmq::sockopt::linger, 0);
  Mads::SocketMonitor monitor;
  monitor.start(client);
  client.connect(mads_test::loopback(44109));

  REQUIRE(mads_test::wait_for(
      [&] { return monitor.state().status == Mads::LinkStatus::Up; }, 2000ms));
  server.reset();
  REQUIRE(mads_test::wait_for(
      [&] { return monitor.state().status == Mads::LinkStatus::Down; },
      2000ms));

  // Same endpoint, back from the dead: libzmq reconnects on its own and the
  // monitor observes it without anyone having to re-issue connect().
  server = std::make_unique<EchoServer>(ctx, mads_test::loopback(44109));
  REQUIRE(mads_test::wait_for(
      [&] { return monitor.state().status == Mads::LinkStatus::Up; }, 4000ms));
  auto st = monitor.state();
  REQUIRE(st.drops == 1);
  REQUIRE(st.recoveries == 1);

  monitor.stop();
  client.close();
}

TEST_CASE("a CURVE rejection reads as Down without a spurious drop",
          "[socket_monitor]") {
  // The reason state() ignores ZMQ_EVENT_CONNECTED: it fires at the TCP
  // level before ZAP has seen the key, so counting it as Up would score
  // every rejected connection attempt as a connection immediately followed
  // by a drop -- turning a link that was never up into a flapping one.
  zmq::context_t ctx;
  auto server_kp = Mads::generate_keypair();
  auto allowed_kp = Mads::generate_keypair();
  auto rogue_kp = Mads::generate_keypair();

  Mads::ZapAuth zap(ctx);
  zap.configure_domain("*");
  zap.configure_curve(allowed_kp.public_key);
  zap.start();

  zmq::socket_t server(ctx, zmq::socket_type::rep);
  server.set(zmq::sockopt::linger, 0);
  server.set(zmq::sockopt::rcvtimeo, 100);
  server.set(zmq::sockopt::curve_server, 1);
  server.set(zmq::sockopt::curve_secretkey, server_kp.secret_key);
  server.bind(mads_test::loopback(44110));
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
  rogue.connect(mads_test::loopback(44110));

  REQUIRE_FALSE(monitor.wait_handshake_succeeded(2000ms));
  auto st = monitor.state();
  REQUIRE(st.status == Mads::LinkStatus::Down);
  REQUIRE(st.last_handshake == Mads::LinkEvent::HandshakeFailedAuth);
  REQUIRE(st.drops == 0); // never up, so nothing was lost
  REQUIRE(st.recoveries == 0);

  // The reason must outlive the events that bury it. libzmq disconnects and
  // then retries after a refusal, so wait for last_event to move on and
  // check that the diagnosis is still there -- this is what `mads top`'s
  // "(broker rejected our key)" and `mads doctor --crypto`'s RejectedAuth
  // both read. (On a platform that fires no follow-up event the wait simply
  // times out and the assertion below still holds.)
  mads_test::wait_for(
      [&] {
        return monitor.state().last_event !=
               Mads::LinkEvent::HandshakeFailedAuth;
      },
      2000ms);
  REQUIRE(monitor.state().last_handshake ==
          Mads::LinkEvent::HandshakeFailedAuth);
  REQUIRE(monitor.state().status == Mads::LinkStatus::Down);

  monitor.stop();
  rogue.close();
  stopped.store(true);
  server_thread.join();
  server.close();
  zap.stop();
}

TEST_CASE("stop() resets the reported state", "[socket_monitor]") {
  // A detached monitor has no link to describe, so it must not keep
  // reporting a status and counters from an observation that has ended.
  zmq::context_t ctx;
  EchoServer server(ctx, mads_test::loopback(44111));

  zmq::socket_t client(ctx, zmq::socket_type::req);
  client.set(zmq::sockopt::linger, 0);
  Mads::SocketMonitor monitor;
  monitor.start(client);
  client.connect(mads_test::loopback(44111));

  REQUIRE(mads_test::wait_for(
      [&] { return monitor.state().status == Mads::LinkStatus::Up; }, 2000ms));
  monitor.stop();

  auto st = monitor.state();
  REQUIRE(st.status == Mads::LinkStatus::Unknown);
  REQUIRE(st.last_event == Mads::LinkEvent::None);
  REQUIRE(st.last_handshake == Mads::LinkEvent::None);
  REQUIRE(st.drops == 0);
  REQUIRE_FALSE(st.changed_at.has_value());

  client.close();
}

// --- attach()/pollable()/process_pending(): one caller's poll loop can
// drive several monitors, instead of each running a thread (§4.1). ---

namespace {

// Drives an attached monitor the way Agent::_start_io_thread() does, until
// `predicate` holds or the deadline passes.
bool pump_until(Mads::SocketMonitor &monitor,
                const std::function<bool()> &predicate,
                std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    auto ref = monitor.pollable();
    if (ref.handle() == nullptr) continue;
    zmq::pollitem_t item;
    item = zmq::pollitem_t{ref.handle(), 0, ZMQ_POLLIN, 0};
    zmq::poll(&item, 1, 50ms);
    if (item.revents & ZMQ_POLLIN) monitor.process_pending();
  }
  return predicate();
}

} // namespace

TEST_CASE("attach() lets a caller drive the monitor from its own poll loop",
          "[socket_monitor]") {
  zmq::context_t ctx;
  EchoServer server(ctx, mads_test::loopback(44112));

  zmq::socket_t client(ctx, zmq::socket_type::req);
  client.set(zmq::sockopt::linger, 0);

  Mads::SocketMonitor monitor;
  REQUIRE(monitor.pollable().handle() == nullptr); // nothing to poll yet
  monitor.attach(client);
  REQUIRE(monitor.pollable().handle() != nullptr);
  client.connect(mads_test::loopback(44112));

  // No thread of its own: nothing advances until the caller pumps it.
  REQUIRE(pump_until(
      monitor, [&] { return monitor.state().status == Mads::LinkStatus::Up; },
      3000ms));
  REQUIRE(monitor.last_event() == Mads::LinkEvent::HandshakeSucceeded);

  monitor.stop();
  REQUIRE(monitor.pollable().handle() == nullptr); // and nothing after stop()
  client.close();
}

TEST_CASE("an attached monitor still releases its socket on stop()",
          "[socket_monitor]") {
  // Same deadlock this file already pins for start(), on the path that has
  // no thread: stop() used to bail out early when the monitor had none, so
  // the PAIR socket stayed open and zmq_ctx_term() would block forever.
  zmq::context_t ctx;
  zmq::socket_t client(ctx, zmq::socket_type::req);
  client.set(zmq::sockopt::linger, 0);

  Mads::SocketMonitor monitor;
  monitor.attach(client);
  client.connect(mads_test::loopback(44113)); // unreachable is fine here
  monitor.stop();
  client.close();

  REQUIRE_NOTHROW(ctx.close());
}

TEST_CASE("attach() is a no-op on an already-attached monitor",
          "[socket_monitor]") {
  // A reconnecting caller calls it again; re-arming zmq_socket_monitor()
  // would replace the PAIR socket and orphan the old one.
  zmq::context_t ctx;
  zmq::socket_t client(ctx, zmq::socket_type::req);
  client.set(zmq::sockopt::linger, 0);

  Mads::SocketMonitor monitor;
  monitor.attach(client);
  void *first = monitor.pollable().handle();
  monitor.attach(client);
  REQUIRE(monitor.pollable().handle() == first);

  monitor.stop();
  client.close();
  REQUIRE_NOTHROW(ctx.close());
}
