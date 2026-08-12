// Unit tests for src/zmqpp_compat.hpp, the deprecated source-compatibility
// shim that lets a pre-cppzmq downstream Agent subclass keep compiling.
//
// Everything here is written in the OLD zmqpp idiom on purpose: this suite is
// a stand-in for downstream code, so if it needs editing to keep building, the
// shim has stopped doing its job.
//
// Port range for this file: 42900-42999.
#define MADS_NO_ZMQPP_COMPAT_WARNING // this file uses the deprecated API by design
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <thread>

#include "agent.hpp"
#include "mads_test_helpers.hpp"
#include "zmqpp_compat.hpp"

using namespace std::chrono_literals;

// A downstream-style subclass reaching into Agent's protected members through
// the shim's types -- exactly the pattern that would otherwise break.
namespace {

class LegacyStyleAgent : public Mads::Agent {
public:
  using Mads::Agent::Agent;

  // _context is a zmq::context_t; the shim's socket must still accept it.
  bool build_socket_from_protected_context() {
    zmqpp::socket s(_context, zmqpp::socket_type::push);
    s.set(zmqpp::socket_option::linger, 0);
    s.close();
    return true;
  }

  // _publisher is a zmq::socket_t; a zmqpp::socket& parameter must bind to it.
  static void takes_legacy_socket_ref(zmqpp::socket &) {}
  bool pass_protected_socket_by_reference() {
    takes_legacy_socket_ref(static_cast<zmqpp::socket &>(_publisher));
    return true;
  }
};

} // namespace

TEST_CASE("the shim's context and socket types still compose", "[compat]") {
  zmqpp::context ctx;
  zmqpp::socket sock(ctx, zmqpp::socket_type::req);
  sock.set(zmqpp::socket_option::linger, 0);
  sock.set(zmqpp::socket_option::receive_timeout, 50);
  sock.close();
  REQUIRE_NOTHROW(ctx.terminate()); // zmqpp's name for zmq_ctx_term()
}

TEST_CASE("message keeps zmqpp's streaming and accessor API", "[compat]") {
  zmqpp::message msg;
  msg << std::string("topic") << "payload";

  REQUIRE(msg.parts() == 2);
  REQUIRE(msg.get(0) == "topic");
  REQUIRE(msg.get(1) == "payload");
  REQUIRE(msg.size(1) == 7);
  REQUIRE(std::string(static_cast<const char *>(msg.raw_data(0)),
                      msg.size(0)) == "topic");

  // copy() is a deep copy that leaves the original usable.
  zmqpp::message dup = msg.copy();
  REQUIRE(dup.parts() == 2);
  REQUIRE(dup.get(0) == "topic");
  REQUIRE(msg.parts() == 2);

  // >> pops from the front, as it did in zmqpp.
  std::string first;
  msg >> first;
  REQUIRE(first == "topic");
  REQUIRE(msg.parts() == 1);
}

TEST_CASE("message::get<T> reads a fixed-size type back out", "[compat]") {
  const uint64_t value = 0x0102030405060708ULL;
  zmqpp::message msg;
  msg.add_raw(&value, sizeof(value));
  REQUIRE(msg.parts() == 1);
  REQUIRE(msg.get<uint64_t>(0) == value);
}

TEST_CASE("a PUB/SUB round trip works entirely through the shim", "[compat]") {
  const uint16_t port = 42901;
  zmqpp::context ctx;
  zmqpp::socket pub(ctx, zmqpp::socket_type::pub);
  pub.bind(mads_test::loopback(port));

  zmqpp::socket sub(ctx, zmqpp::socket_type::sub);
  sub.subscribe("");
  sub.set(zmqpp::socket_option::receive_timeout, 200);
  sub.connect(mads_test::loopback(port));
  std::this_thread::sleep_for(200ms); // subscription propagation

  bool got = mads_test::wait_for(
      [&] {
        zmqpp::message out;
        out << std::string("compat") << std::string("{\"v\":1}");
        pub.send(out);
        zmqpp::message in;
        if (!sub.receive(in)) return false;
        REQUIRE(in.parts() == 2);
        REQUIRE(in.get(0) == "compat");
        REQUIRE(in.get(1) == "{\"v\":1}");
        return true;
      },
      3000ms, 50ms);
  REQUIRE(got);
}

TEST_CASE("a REQ/REP round trip works entirely through the shim", "[compat]") {
  const uint16_t port = 42902;
  zmqpp::context ctx;
  zmqpp::socket rep(ctx, zmqpp::socket_type::rep);
  rep.set(zmqpp::socket_option::receive_timeout, 100);
  rep.bind(mads_test::loopback(port));

  std::atomic<bool> stopped{false};
  std::thread server([&] {
    while (!stopped) {
      zmqpp::message in;
      if (!rep.receive(in)) continue;
      zmqpp::message out;
      out << std::string("pong");
      rep.send(out);
    }
  });

  zmqpp::socket req(ctx, zmqpp::socket_type::req);
  req.set(zmqpp::socket_option::linger, 0);
  req.set(zmqpp::socket_option::receive_timeout, 2000);
  req.connect(mads_test::loopback(port));

  zmqpp::message out;
  out << std::string("ping");
  REQUIRE(req.send(out));
  zmqpp::message in;
  REQUIRE(req.receive(in));
  REQUIRE(in.get(0) == "pong");

  stopped = true;
  server.join();
  req.close();
  rep.close();
}

TEST_CASE("receive() reports a timeout as false rather than throwing",
          "[compat]") {
  zmqpp::context ctx;
  zmqpp::socket sub(ctx, zmqpp::socket_type::sub);
  sub.subscribe("");
  sub.set(zmqpp::socket_option::receive_timeout, 50);
  sub.connect(mads_test::loopback(42903)); // nothing is bound there

  zmqpp::message in;
  REQUIRE_FALSE(sub.receive(in));
  REQUIRE_FALSE(sub.receive(in, true)); // dont_block
}

TEST_CASE("curve::generate_keypair still returns a zmqpp-shaped keypair",
          "[compat]") {
  zmqpp::curve::keypair kp = zmqpp::curve::generate_keypair();
  REQUIRE(kp.public_key.size() == 40);
  REQUIRE(kp.secret_key.size() == 40);
}

TEST_CASE("CurveAuth still accepts the shim's socket type", "[compat]") {
  zmqpp::context ctx;
  Mads::CurveAuth auth(ctx); // takes zmq::context_t&, binds to the shim's
  zmqpp::socket sock(ctx, zmqpp::socket_type::req);
  auto client_kp = zmqpp::curve::generate_keypair();
  auto server_kp = zmqpp::curve::generate_keypair();
  auth.set_client_public_key(client_kp.public_key);
  auth.set_client_secret_key(client_kp.secret_key);
  auth.set_server_public_key(server_kp.public_key);
  // setup_curve_client() takes a zmq::socket_t&: the shim's socket must bind.
  REQUIRE_NOTHROW(auth.setup_curve_client(sock));
}

TEST_CASE("an Agent subclass can still reach the protected sockets",
          "[compat]") {
  LegacyStyleAgent agent("compat-agent", "none");
  REQUIRE(agent.build_socket_from_protected_context());
  REQUIRE(agent.pass_protected_socket_by_reference());
}
