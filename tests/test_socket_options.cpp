// Pins Mads::detail::SocketOptions (src/detail/socket_options.hpp):
// resolution precedence between [agents] and an agent's own section, and
// that a resolved option actually reaches the socket.
//
// Port range for this file: 44000-44099 (none of these cases bind a socket,
// but the range is reserved for consistency with the rest of the suite).
#include <catch2/catch_test_macros.hpp>

#include <toml++/toml.hpp>
#include <zmq.hpp>

#include "detail/socket_options.hpp"

using namespace Mads::detail;

namespace {

toml::table parse(std::string_view src) {
  return toml::parse(src);
}

} // namespace

TEST_CASE("resolve_setting: per-agent section overrides [agents]",
          "[socket_options]") {
  auto cfg = parse(R"(
    [agents]
    sndbuf = 1000

    [myagent]
    sndbuf = 2000
  )");
  auto v = resolve_setting<int>(cfg["agents"], cfg["myagent"], "sndbuf");
  REQUIRE(v.has_value());
  REQUIRE(*v == 2000);
}

TEST_CASE("resolve_setting: falls back to the fleet-wide [agents] value",
          "[socket_options]") {
  auto cfg = parse(R"(
    [agents]
    sndbuf = 1000

    [myagent]
  )");
  auto v = resolve_setting<int>(cfg["agents"], cfg["myagent"], "sndbuf");
  REQUIRE(v.has_value());
  REQUIRE(*v == 1000);
}

TEST_CASE("resolve_setting: nullopt when neither table carries the key",
          "[socket_options]") {
  auto cfg = parse(R"(
    [agents]

    [myagent]
  )");
  auto v = resolve_setting<int>(cfg["agents"], cfg["myagent"], "sndbuf");
  REQUIRE_FALSE(v.has_value());
}

TEST_CASE("SocketOptions::resolve: absent keys leave every field unconfigured",
          "[socket_options]") {
  auto cfg = parse(R"(
    [agents]
    frontend_address = "tcp://localhost:9090"

    [myagent]
  )");
  auto opts = SocketOptions::resolve(cfg["agents"], cfg["myagent"]);
  REQUIRE_FALSE(opts.tcp_keepalive.has_value());
  REQUIRE_FALSE(opts.tcp_keepalive_idle.has_value());
  REQUIRE_FALSE(opts.tcp_keepalive_cnt.has_value());
  REQUIRE_FALSE(opts.tcp_keepalive_intvl.has_value());
  REQUIRE_FALSE(opts.sndbuf.has_value());
  REQUIRE_FALSE(opts.rcvbuf.has_value());
  REQUIRE_FALSE(opts.heartbeat_ivl.has_value());
  REQUIRE_FALSE(opts.heartbeat_ttl.has_value());
  REQUIRE_FALSE(opts.heartbeat_timeout.has_value());
  REQUIRE_FALSE(opts.reconnect_ivl.has_value());
  REQUIRE_FALSE(opts.reconnect_ivl_max.has_value());
  REQUIRE_FALSE(opts.immediate.has_value());
  REQUIRE_FALSE(opts.max_msg_size.has_value());
}

TEST_CASE("SocketOptions::apply: an unconfigured struct touches no option",
          "[socket_options]") {
  zmq::context_t ctx;
  zmq::socket_t sock(ctx, zmq::socket_type::pub);
  // libzmq's own default: no override, keepalive left to the OS.
  REQUIRE(sock.get(zmq::sockopt::tcp_keepalive) == -1);

  SocketOptions opts; // every field unset
  opts.apply(sock);

  REQUIRE(sock.get(zmq::sockopt::tcp_keepalive) == -1);
  sock.close();
}

TEST_CASE("SocketOptions::apply: a configured option reaches the socket",
          "[socket_options]") {
  auto cfg = parse(R"(
    [agents]
    tcp_keepalive = 1
    tcp_keepalive_idle = 60
    tcp_keepalive_cnt = 3
    tcp_keepalive_intvl = 10

    [myagent]
  )");
  auto opts = SocketOptions::resolve(cfg["agents"], cfg["myagent"]);

  zmq::context_t ctx;
  zmq::socket_t sock(ctx, zmq::socket_type::pub);
  opts.apply(sock);

  // ZMQ_TCP_KEEPALIVE and friends are stored verbatim by libzmq (no
  // kernel-side transformation like SO_SNDBUF/SO_RCVBUF can undergo on some
  // platforms), so an exact round-trip is portable across macOS/Linux/Windows.
  REQUIRE(sock.get(zmq::sockopt::tcp_keepalive) == 1);
  REQUIRE(sock.get(zmq::sockopt::tcp_keepalive_idle) == 60);
  REQUIRE(sock.get(zmq::sockopt::tcp_keepalive_cnt) == 3);
  REQUIRE(sock.get(zmq::sockopt::tcp_keepalive_intvl) == 10);
  sock.close();
}

TEST_CASE("SocketOptions::apply: heartbeats/reconnect/immediate/max_msg_size "
          "reach the socket",
          "[socket_options]") {
  auto cfg = parse(R"(
    [agents]
    heartbeat_ivl = 5000
    heartbeat_ttl = 10000
    heartbeat_timeout = 8000
    reconnect_ivl = 200
    reconnect_ivl_max = 30000
    immediate = true
    max_msg_size = 1048576

    [myagent]
  )");
  auto opts = SocketOptions::resolve(cfg["agents"], cfg["myagent"]);
  REQUIRE(*opts.heartbeat_ivl == 5000);
  REQUIRE(*opts.heartbeat_ttl == 10000);
  REQUIRE(*opts.heartbeat_timeout == 8000);
  REQUIRE(*opts.reconnect_ivl == 200);
  REQUIRE(*opts.reconnect_ivl_max == 30000);
  REQUIRE(*opts.immediate == true);
  REQUIRE(*opts.max_msg_size == 1048576);

  zmq::context_t ctx;
  zmq::socket_t sock(ctx, zmq::socket_type::pub);
  opts.apply(sock);

  // All pure libzmq-internal state (unlike SNDBUF/RCVBUF, which some kernels
  // transform), so an exact round-trip is portable.
  REQUIRE(sock.get(zmq::sockopt::heartbeat_ivl) == 5000);
  REQUIRE(sock.get(zmq::sockopt::heartbeat_ttl) == 10000);
  REQUIRE(sock.get(zmq::sockopt::heartbeat_timeout) == 8000);
  REQUIRE(sock.get(zmq::sockopt::reconnect_ivl) == 200);
  REQUIRE(sock.get(zmq::sockopt::reconnect_ivl_max) == 30000);
  REQUIRE(sock.get(zmq::sockopt::immediate) == 1);
  REQUIRE(sock.get(zmq::sockopt::maxmsgsize) == 1048576);
  sock.close();
}

TEST_CASE("SocketOptions::apply: max_msg_size defaults to -1 (unlimited), "
          "not 0",
          "[socket_options]") {
  zmq::context_t ctx;
  zmq::socket_t sock(ctx, zmq::socket_type::pub);
  // Pinning ZMQ_DEVELOPMENT.md §3.5's correction: an untouched socket's
  // MAXMSGSIZE is -1 (unlimited), never 0 (which would reject everything).
  REQUIRE(sock.get(zmq::sockopt::maxmsgsize) == -1);
  sock.close();
}
