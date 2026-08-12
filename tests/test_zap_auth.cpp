// Unit tests for Mads::ZapAuth (src/zap_auth.hpp), the RFC 27 ZAP handler that
// replaced zmqpp::auth in the cppzmq migration.
//
// The security-critical property here is *rejection*: an unauthorised CURVE
// client must not complete a handshake. test_curve.cpp only exercises the happy
// path, so the negative cases live here.
//
// Port range for this file: 42800-42899 (the former "(spare)" range; see
// mads_test_helpers.hpp).
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <thread>

#include <zmq.hpp>
#include <zmq_addon.hpp>

#include "mads_test_helpers.hpp"
#include "zap_auth.hpp"

using namespace std::chrono_literals;

namespace {

// Attempts one CURVE-encrypted REQ/REP round trip and reports whether the
// reply came back. A rejected handshake shows up as a receive timeout, which
// is exactly how libzmq surfaces a ZAP denial to the client.
bool curve_round_trip(zmq::context_t &ctx, const std::string &endpoint,
                      const Mads::CurveKeypair &client_kp,
                      const std::string &server_public_key,
                      int timeout_ms = 800) {
  zmq::socket_t req(ctx, zmq::socket_type::req);
  req.set(zmq::sockopt::linger, 0);
  req.set(zmq::sockopt::rcvtimeo, timeout_ms);
  req.set(zmq::sockopt::sndtimeo, timeout_ms);
  req.set(zmq::sockopt::curve_publickey, client_kp.public_key);
  req.set(zmq::sockopt::curve_secretkey, client_kp.secret_key);
  req.set(zmq::sockopt::curve_serverkey, server_public_key);
  req.connect(endpoint);

  zmq::multipart_t out;
  out.addstr("ping");
  if (!out.send(req)) return false;
  zmq::multipart_t in;
  return in.recv(req);
}

// A CURVE server socket plus a thread that answers a bounded number of
// requests, so every test tears down deterministically.
struct CurveServer {
  zmq::socket_t sock;
  std::thread thread;
  std::atomic<bool> stopped{false};

  CurveServer(zmq::context_t &ctx, const std::string &endpoint,
              const std::string &secret_key)
      : sock(ctx, zmq::socket_type::rep) {
    sock.set(zmq::sockopt::linger, 0);
    sock.set(zmq::sockopt::rcvtimeo, 100);
    sock.set(zmq::sockopt::curve_server, 1);
    sock.set(zmq::sockopt::curve_secretkey, secret_key);
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
  ~CurveServer() {
    stopped.store(true);
    if (thread.joinable()) thread.join();
    sock.close();
  }
};

} // namespace

// ---------------------------------------------------------------------------
// Key/encoding helpers
// ---------------------------------------------------------------------------

TEST_CASE("generate_keypair returns two distinct 40-character Z85 keys",
          "[zap]") {
  auto kp = Mads::generate_keypair();
  REQUIRE(kp.public_key.size() == 40);
  REQUIRE(kp.secret_key.size() == 40);
  REQUIRE(kp.public_key != kp.secret_key);

  auto other = Mads::generate_keypair();
  REQUIRE(other.public_key != kp.public_key);
}

TEST_CASE("z85_encode round-trips a 32-byte key and rejects bad lengths",
          "[zap]") {
  auto kp = Mads::generate_keypair();
  // Decode with the C API, re-encode with ours: the text must come back.
  uint8_t raw[32] = {0};
  REQUIRE(zmq_z85_decode(raw, kp.public_key.c_str()) != nullptr);
  const std::string binary(reinterpret_cast<const char *>(raw), sizeof(raw));
  REQUIRE(Mads::z85_encode(binary) == kp.public_key);

  // Z85 only encodes buffers whose length is a multiple of 4.
  REQUIRE(Mads::z85_encode("abc").empty());
  REQUIRE(Mads::z85_encode("").empty());
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

TEST_CASE("start() is idempotent and stop() may be called without start()",
          "[zap]") {
  zmq::context_t ctx;
  Mads::ZapAuth auth(ctx);
  REQUIRE_NOTHROW(auth.stop()); // never started
  REQUIRE_NOTHROW(auth.start());
  REQUIRE_NOTHROW(auth.start()); // second call is a no-op, not a double bind
  REQUIRE_NOTHROW(auth.stop());
  REQUIRE_NOTHROW(auth.stop());
}

TEST_CASE("a second handler cannot bind the ZAP endpoint of the same context",
          "[zap]") {
  zmq::context_t ctx;
  Mads::ZapAuth first(ctx);
  first.start();
  Mads::ZapAuth second(ctx);
  REQUIRE_THROWS_AS(second.start(), zmq::error_t);
}

// ---------------------------------------------------------------------------
// CURVE allowlist -- the security-critical behaviour
// ---------------------------------------------------------------------------

TEST_CASE("an unauthorised CURVE client is rejected while an authorised one "
          "gets through",
          "[zap]") {
  const uint16_t port = 42801;
  auto server_kp = Mads::generate_keypair();
  auto good_kp = Mads::generate_keypair();
  auto bad_kp = Mads::generate_keypair();

  zmq::context_t ctx;
  Mads::ZapAuth auth(ctx);
  auth.configure_domain("*");
  auth.allow("127.0.0.1");
  auth.configure_curve(good_kp.public_key); // bad_kp is deliberately absent
  auth.start();

  CurveServer server(ctx, mads_test::loopback(port), server_kp.secret_key);

  REQUIRE(curve_round_trip(ctx, mads_test::loopback(port), good_kp,
                           server_kp.public_key));
  REQUIRE_FALSE(curve_round_trip(ctx, mads_test::loopback(port), bad_kp,
                                 server_kp.public_key));

  REQUIRE(auth.granted() >= 1);
  REQUIRE(auth.denied() >= 1);
}

TEST_CASE("an empty CURVE allowlist accepts any client key", "[zap]") {
  const uint16_t port = 42802;
  auto server_kp = Mads::generate_keypair();
  auto client_kp = Mads::generate_keypair();

  zmq::context_t ctx;
  Mads::ZapAuth auth(ctx);
  auth.allow("127.0.0.1");
  auth.start(); // no configure_curve() call at all

  CurveServer server(ctx, mads_test::loopback(port), server_kp.secret_key);
  REQUIRE(curve_round_trip(ctx, mads_test::loopback(port), client_kp,
                           server_kp.public_key));
  REQUIRE(auth.granted() >= 1);
}

TEST_CASE("configure_curve(\"*\") accepts a client that is not on the list",
          "[zap]") {
  const uint16_t port = 42803;
  auto server_kp = Mads::generate_keypair();
  auto listed_kp = Mads::generate_keypair();
  auto other_kp = Mads::generate_keypair();

  zmq::context_t ctx;
  Mads::ZapAuth auth(ctx);
  auth.allow("127.0.0.1");
  auth.configure_curve(listed_kp.public_key);
  auth.configure_curve("*");
  auth.start();

  CurveServer server(ctx, mads_test::loopback(port), server_kp.secret_key);
  REQUIRE(curve_round_trip(ctx, mads_test::loopback(port), other_kp,
                           server_kp.public_key));
}

// ---------------------------------------------------------------------------
// Address whitelist
// ---------------------------------------------------------------------------

TEST_CASE("a peer whose address is not whitelisted is rejected", "[zap]") {
  const uint16_t port = 42804;
  auto server_kp = Mads::generate_keypair();
  auto client_kp = Mads::generate_keypair();

  zmq::context_t ctx;
  Mads::ZapAuth auth(ctx);
  // Whitelist an address the loopback client cannot be coming from, so the
  // exclusive-whitelist rule must reject it even though its key is allowed.
  auth.allow("10.255.255.1");
  auth.configure_curve(client_kp.public_key);
  auth.start();

  CurveServer server(ctx, mads_test::loopback(port), server_kp.secret_key);
  REQUIRE_FALSE(curve_round_trip(ctx, mads_test::loopback(port), client_kp,
                                 server_kp.public_key));
  REQUIRE(auth.denied() >= 1);
}

// ---------------------------------------------------------------------------
// Domain filtering
// ---------------------------------------------------------------------------

TEST_CASE("configure_domain restricts the handler to one ZAP domain",
          "[zap]") {
  const uint16_t port = 42805;
  auto server_kp = Mads::generate_keypair();
  auto client_kp = Mads::generate_keypair();

  zmq::context_t ctx;
  Mads::ZapAuth auth(ctx);
  auth.configure_domain("other-domain");
  auth.allow("127.0.0.1");
  auth.configure_curve(client_kp.public_key);
  auth.start();

  // The server socket advertises no ZAP domain, so requests arrive with an
  // empty domain and must not match "other-domain".
  CurveServer server(ctx, mads_test::loopback(port), server_kp.secret_key);
  REQUIRE_FALSE(curve_round_trip(ctx, mads_test::loopback(port), client_kp,
                                 server_kp.public_key));
  REQUIRE(auth.denied() >= 1);
}
