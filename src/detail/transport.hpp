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
  [<agent>] shm           bool    default false; publish blobs via a
                                  shared-memory ring
            shm_slots     int     default 8
            shm_slot_size int     default 16 MiB (bytes)
*/
#pragma once

#include "endpoint.hpp"

#include <cctype>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace Mads::detail {

constexpr const char *TRANSPORT_CMD = "transport";

constexpr bool BROKER_IPC_DEFAULT = true;
constexpr int SHM_SLOTS_DEFAULT = 8;
constexpr int64_t SHM_SLOT_SIZE_DEFAULT = 16 * 1024 * 1024;

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
  auto lower = [](std::string s) {
    for (auto &c : s)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  std::string host = lower(ep->host);
  if (host.size() > 2 && host.front() == '[' && host.back() == ']')
    host = host.substr(1, host.size() - 2);
  if (host == "localhost" || host == "::1" || host == "*" ||
      host == "0.0.0.0" || host.rfind("127.", 0) == 0)
    return {};
  // This host's name, short or fully qualified on either side.
  std::string own_host = lower(own_token.substr(0, own_token.find('/')));
  if (!own_host.empty() &&
      (host == own_host || own_host.rfind(host + ".", 0) == 0 ||
       host.rfind(own_host + ".", 0) == 0))
    return {};
  return "the broker at " + ep->host + " is not on this host";
}

} // namespace Mads::detail
