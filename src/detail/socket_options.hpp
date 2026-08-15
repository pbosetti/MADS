/*
Internal helper: resolves plain libzmq transport-tuning knobs from settings
and applies them to a socket. Not part of the installed SDK (src/detail/ is
excluded from the LIB_HEADERS install glob in CMakeLists.txt).

See ZMQ_DEVELOPMENT.md §1.4.
*/
#pragma once

#include <optional>
#include <string_view>
#include <toml++/toml.hpp>
#include <zmq.hpp>

namespace Mads::detail {

// Resolves `key` the same way Agent::init() resolves wire_format/compression
// (src/agent.cpp:452-469): the agent's own section wins over the fleet-wide
// [agents] table, which wins over "not configured" (nullopt). A nullopt means
// "leave the libzmq default untouched" -- callers must not fall back to a
// hardcoded default themselves, or an unconfigured deployment would start
// issuing setsockopt() calls it never asked for.
template <typename T>
std::optional<T> resolve_setting(toml::node_view<toml::node> fleet_cfg,
                                  toml::node_view<toml::node> agent_cfg,
                                  std::string_view key) {
  if (auto v = agent_cfg[key].value<T>()) return v;
  if (auto v = fleet_cfg[key].value<T>()) return v;
  return std::nullopt;
}

// Plain transport-tuning knobs, none of which touch the wire format
// (ZMQ_DEVELOPMENT.md §1.4, §3.2-§3.5). Every field defaults to
// "unconfigured", which means apply() makes no setsockopt() call at all and
// the socket keeps libzmq's own default -- so an unedited mads.ini produces
// byte-identical behaviour to before this struct existed.
struct SocketOptions {
  std::optional<int> tcp_keepalive;
  std::optional<int> tcp_keepalive_idle;
  std::optional<int> tcp_keepalive_cnt;
  std::optional<int> tcp_keepalive_intvl;
  std::optional<int> sndbuf;
  std::optional<int> rcvbuf;
  // ZMTP heartbeats (§3.2). IVL/TIMEOUT are milliseconds; TTL is in units of
  // 100ms (libzmq caps it at 6553.5s). Leaving heartbeat_ivl unset keeps
  // heartbeats off, exactly as today.
  std::optional<int> heartbeat_ivl;
  std::optional<int> heartbeat_ttl;
  std::optional<int> heartbeat_timeout;
  // Reconnect backoff (§3.3).
  std::optional<int> reconnect_ivl;
  std::optional<int> reconnect_ivl_max;
  // Refuse to queue toward a not-yet-connected peer instead of buffering
  // into a pipe that may never drain (§3.4).
  std::optional<bool> immediate;
  // Rejects oversized frames at the transport by disconnecting the peer,
  // rather than silently allocating for them (§3.5). int64_t, NOT 0 for
  // "unlimited" -- libzmq's own sentinel is -1.
  std::optional<int64_t> max_msg_size;

  static SocketOptions resolve(toml::node_view<toml::node> fleet_cfg,
                                toml::node_view<toml::node> agent_cfg) {
    SocketOptions opts;
    opts.tcp_keepalive =
        resolve_setting<int>(fleet_cfg, agent_cfg, "tcp_keepalive");
    opts.tcp_keepalive_idle =
        resolve_setting<int>(fleet_cfg, agent_cfg, "tcp_keepalive_idle");
    opts.tcp_keepalive_cnt =
        resolve_setting<int>(fleet_cfg, agent_cfg, "tcp_keepalive_cnt");
    opts.tcp_keepalive_intvl =
        resolve_setting<int>(fleet_cfg, agent_cfg, "tcp_keepalive_intvl");
    opts.sndbuf = resolve_setting<int>(fleet_cfg, agent_cfg, "sndbuf");
    opts.rcvbuf = resolve_setting<int>(fleet_cfg, agent_cfg, "rcvbuf");
    opts.heartbeat_ivl =
        resolve_setting<int>(fleet_cfg, agent_cfg, "heartbeat_ivl");
    opts.heartbeat_ttl =
        resolve_setting<int>(fleet_cfg, agent_cfg, "heartbeat_ttl");
    opts.heartbeat_timeout =
        resolve_setting<int>(fleet_cfg, agent_cfg, "heartbeat_timeout");
    opts.reconnect_ivl =
        resolve_setting<int>(fleet_cfg, agent_cfg, "reconnect_ivl");
    opts.reconnect_ivl_max =
        resolve_setting<int>(fleet_cfg, agent_cfg, "reconnect_ivl_max");
    opts.immediate = resolve_setting<bool>(fleet_cfg, agent_cfg, "immediate");
    opts.max_msg_size =
        resolve_setting<int64_t>(fleet_cfg, agent_cfg, "max_msg_size");
    return opts;
  }

  void apply(zmq::socket_t &socket) const {
    if (tcp_keepalive)
      socket.set(zmq::sockopt::tcp_keepalive, *tcp_keepalive);
    if (tcp_keepalive_idle)
      socket.set(zmq::sockopt::tcp_keepalive_idle, *tcp_keepalive_idle);
    if (tcp_keepalive_cnt)
      socket.set(zmq::sockopt::tcp_keepalive_cnt, *tcp_keepalive_cnt);
    if (tcp_keepalive_intvl)
      socket.set(zmq::sockopt::tcp_keepalive_intvl, *tcp_keepalive_intvl);
    if (sndbuf) socket.set(zmq::sockopt::sndbuf, *sndbuf);
    if (rcvbuf) socket.set(zmq::sockopt::rcvbuf, *rcvbuf);
    if (heartbeat_ivl)
      socket.set(zmq::sockopt::heartbeat_ivl, *heartbeat_ivl);
    if (heartbeat_ttl)
      socket.set(zmq::sockopt::heartbeat_ttl, *heartbeat_ttl);
    if (heartbeat_timeout)
      socket.set(zmq::sockopt::heartbeat_timeout, *heartbeat_timeout);
    if (reconnect_ivl)
      socket.set(zmq::sockopt::reconnect_ivl, *reconnect_ivl);
    if (reconnect_ivl_max)
      socket.set(zmq::sockopt::reconnect_ivl_max, *reconnect_ivl_max);
    if (immediate)
      socket.set(zmq::sockopt::immediate, *immediate);
    if (max_msg_size)
      socket.set(zmq::sockopt::maxmsgsize, *max_msg_size);
  }
};

} // namespace Mads::detail
