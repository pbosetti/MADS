/*
Internal helper: the contract between broker and agent for local transports
(ipc:// sockets and the shared-memory data plane). Not part of the installed
SDK.

Settings command `transport`
----------------------------
Sent by the agent on the settings REQ socket, after `settings`/`timecode`:

  request:  [ LIB_VERSION ][ "transport" ][ agent_name ]
  reply:    [ json text ]                  (one frame, see TransportInfo)

A broker that predates the command answers with an empty frame (the
"unexpected command" branch of the settings worker); agents treat an empty or
unparsable reply as "no ipc offered" and stay on tcp.

Configuration keys
------------------
  [broker]  ipc           bool    default true; bind ipc:// endpoints too
            ipc_dir       string  default Mads::detail::runtime_dir()
            frontend_ipc  string  explicit override of the derived endpoint
            backend_ipc   string  "
            settings_ipc  string  "
  [agents]  transport     string  "auto" (default) | "tcp" | "ipc";
  [<agent>] transport             the agent section wins over [agents];
                                  the --transport CLI option wins over both
  [agents]  ipc_sndbuf    int     kernel buffers of ipc:// connections only
            ipc_rcvbuf    int     (default 1 MiB, 0 = OS default; agent
                                  section first, then [agents]; the broker
                                  reads [broker], then [agents]); see
                                  detail::IpcBuffers in socket_options.hpp
  [<agent>] shm           bool    default false; publish blobs via a
                                  shared-memory ring
            shm_slots     int     default 8
            shm_slot_size int     default 16 MiB (bytes)
*/
#pragma once

#include "endpoint.hpp"

#include <cstdint>
#include <functional>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace Mads::detail {

constexpr const char *TRANSPORT_CMD = "transport";

constexpr bool BROKER_IPC_DEFAULT = true;
constexpr int SHM_SLOTS_DEFAULT = 8;
constexpr int64_t SHM_SLOT_SIZE_DEFAULT = 16 * 1024 * 1024;

// Ring geometry from the `shm_slots`/`shm_slot_size` settings, or nullopt
// (with a reason in *error) if out of range: at least 2 slots that fit a
// uint32_t, and a positive slot size that fits a size_t.
struct ShmGeometry {
  uint32_t slots = 0;
  size_t slot_size = 0;
};

inline std::optional<ShmGeometry> shm_geometry(int64_t slots,
                                               int64_t slot_size,
                                               std::string *error = nullptr) {
  const bool slots_ok = slots >= 2 && static_cast<uint64_t>(slots) <=
                                          (std::numeric_limits<uint32_t>::max)();
  const bool size_ok = slot_size > 0 && static_cast<uint64_t>(slot_size) <=
                                            (std::numeric_limits<size_t>::max)();
  if (slots_ok && size_ok)
    return ShmGeometry{static_cast<uint32_t>(slots),
                       static_cast<size_t>(slot_size)};
  if (error)
    *error = "shm_slots = " + std::to_string(slots) +
             " (expected 2..4294967295), shm_slot_size = " +
             std::to_string(slot_size) + " (expected > 0)";
  return std::nullopt;
}

enum class TransportMode { Auto, Tcp, Ipc };

inline std::optional<TransportMode> parse_transport_mode(std::string_view s) {
  if (s == "auto")
    return TransportMode::Auto;
  if (s == "tcp")
    return TransportMode::Tcp;
  if (s == "ipc")
    return TransportMode::Ipc;
  return std::nullopt;
}

inline const char *to_string(TransportMode m) {
  switch (m) {
  case TransportMode::Tcp:
    return "tcp";
  case TransportMode::Ipc:
    return "ipc";
  case TransportMode::Auto:
  default:
    return "auto";
  }
}

// Reply payload of the `transport` command. Empty ipc strings mean the broker
// does not offer ipc for that role (disabled, or zmq_has("ipc") is false).
struct TransportInfo {
  std::string host_token;   // Mads::detail::host_token() of the broker
  std::string frontend_ipc; // ipc://... bound on the XSUB frontend
  std::string backend_ipc;  // ipc://... bound on the XPUB backend
  std::string settings_ipc; // ipc://... bound on the settings ROUTER
  bool curve = false;       // CURVE is enabled on the broker sockets

  bool offers_ipc() const {
    return !frontend_ipc.empty() && !backend_ipc.empty();
  }
};

inline void to_json(nlohmann::json &j, TransportInfo const &t) {
  j = nlohmann::json{{"host_token", t.host_token},
                     {"ipc",
                      {{"frontend", t.frontend_ipc},
                       {"backend", t.backend_ipc},
                       {"settings", t.settings_ipc}}},
                     {"curve", t.curve}};
}

inline void from_json(nlohmann::json const &j, TransportInfo &t) {
  t.host_token = j.value("host_token", "");
  if (auto it = j.find("ipc"); it != j.end() && it->is_object()) {
    t.frontend_ipc = it->value("frontend", "");
    t.backend_ipc = it->value("backend", "");
    t.settings_ipc = it->value("settings", "");
  }
  t.curve = j.value("curve", false);
}

// Parse a `transport` reply frame; nullopt for an empty/legacy/garbled reply.
inline std::optional<TransportInfo> parse_transport_reply(std::string_view s) {
  if (s.empty())
    return std::nullopt;
  auto j = nlohmann::json::parse(s, nullptr, false);
  if (j.is_discarded() || !j.is_object())
    return std::nullopt;
  return j.get<TransportInfo>();
}

// The transport mode string by precedence: CLI/API override > [<agent>]
// transport > [agents] transport > "auto". Empty/absent values defer.
inline std::string
resolve_transport_mode(std::string const &override_mode,
                       std::optional<std::string> const &agent_value,
                       std::optional<std::string> const &agents_value) {
  if (!override_mode.empty())
    return override_mode;
  if (agent_value)
    return *agent_value;
  if (agents_value)
    return *agents_value;
  return "auto";
}

// Tells whether an ipc socket file is visible from here (injectable for
// tests; normally Mads::detail::socket_file_exists()).
using SocketExistsFn = std::function<bool(std::string const &path)>;

// Why an agent on this host cannot use the broker's ipc endpoints, or "" if
// it can. `info` is the parsed `transport` reply (nullopt: none, or a broker
// that predates the command). With settings fetched over ipc the broker is
// reachable through this host's filesystem already, so the host-token check
// is skipped (containers that share a socket directory need not share a
// hostname).
inline std::string ipc_unusable_reason(std::optional<TransportInfo> const &info,
                                       bool settings_over_ipc,
                                       std::string const &own_token,
                                       bool has_ipc,
                                       SocketExistsFn const &socket_exists) {
  if (!has_ipc)
    return "this libzmq build has no ipc transport";
  if (!info)
    return "the broker does not offer ipc (it predates the transport "
           "command)";
  if (!info->offers_ipc())
    return "the broker does not offer ipc endpoints (ipc disabled or "
           "unavailable on the broker)";
  if (!settings_over_ipc && info->host_token != own_token)
    return "the broker runs on another host";
  for (auto const *ep : {&info->frontend_ipc, &info->backend_ipc}) {
    auto e = parse_endpoint(*ep);
    if (!e || !e->is_ipc() || !socket_exists(e->path))
      return "the ipc socket " + *ep + " is not reachable from here";
  }
  return {};
}

// The transport an agent selects (Agent::_select_transport(), and what
// `mads doctor` predicts).
struct TransportChoice {
  bool ipc = false;    // use info's frontend/backend ipc endpoints
  std::string why_not; // why not ipc, when !ipc
  std::string error;   // non-empty: the agent refuses to start
};

inline TransportChoice
choose_transport(TransportMode mode, std::string const &settings_uri,
                 std::optional<TransportInfo> const &info,
                 std::string const &own_token, bool has_ipc,
                 SocketExistsFn const &socket_exists) {
  TransportChoice c;
  if (!is_zmq_uri(settings_uri)) {
    if (mode == TransportMode::Ipc)
      c.error = "transport = ipc needs settings from a broker, not " +
                settings_uri;
    else
      c.why_not = "settings come from a file, not from a broker";
    return c;
  }
  if (mode == TransportMode::Tcp) {
    c.why_not = "transport = tcp";
    return c;
  }
  auto s_ep = parse_endpoint(settings_uri);
  const bool settings_over_ipc = s_ep && !s_ep->is_tcp();
  c.why_not = ipc_unusable_reason(info, settings_over_ipc, own_token, has_ipc,
                                  socket_exists);
  if (c.why_not.empty()) {
    c.ipc = true;
    return c;
  }
  if (mode == TransportMode::Ipc)
    c.error = "transport = ipc requested, but " + c.why_not;
  else if (settings_over_ipc)
    c.error = "Settings were fetched over " + settings_uri + ", but " +
              c.why_not + ": use a tcp:// settings URI, or set transport = tcp";
  return c;
}

// Whether a shared-memory producer may publish ring references: its broker
// (and hence, normally, its consumers) must run on this host. Returns "" if
// so, or the reason why not. Pure, for testability; Agent passes its own
// state and Mads::detail::host_token().
//
//   - transport "ipc", or an ipc/inproc endpoint: local by construction;
//   - a `transport` reply naming the broker's host token: authoritative (so
//     an ssh tunnel on localhost that reaches a remote broker is not local);
//   - otherwise, from the tcp address: loopback, a wildcard bind, or this
//     host's name (the part of `own_token` before '/').
inline std::string shm_unusable_reason(std::string_view transport,
                                       std::string_view pub_endpoint,
                                       std::string_view transport_reply,
                                       std::string const &own_token) {
  if (transport == "ipc")
    return {};
  auto ep = parse_endpoint(pub_endpoint);
  if (ep && (ep->is_ipc() || ep->is_inproc()))
    return {};
  if (auto info = parse_transport_reply(transport_reply);
      info && !info->host_token.empty()) {
    if (info->host_token == own_token)
      return {};
    return "the broker runs on another host (" + info->host_token + ")";
  }
  if (!ep)
    return "cannot tell where the broker runs (endpoint '" +
           std::string(pub_endpoint) + "')";
  std::string host = to_lower(ep->host);
  if (host.size() > 2 && host.front() == '[' && host.back() == ']')
    host = host.substr(1, host.size() - 2);
  if (host == "localhost" || host == "::1" || host == "*" ||
      host == "0.0.0.0" || host.rfind("127.", 0) == 0)
    return {};
  // This host's name, short or fully qualified on either side.
  std::string own_host = to_lower(own_token.substr(0, own_token.find('/')));
  if (!own_host.empty() &&
      (host == own_host || own_host.rfind(host + ".", 0) == 0 ||
       host.rfind(own_host + ".", 0) == 0))
    return {};
  return "the broker at " + ep->host + " is not on this host";
}

} // namespace Mads::detail
