/*
Internal helper: a ZeroMQ endpoint split into its parts. Replaces the ad-hoc
`find("tcp://")` checks and the `(\w+://)([\w\.-]+):(\d+)` regex that assumed
every address is `tcp://host:port`, which an `ipc://` (or `inproc://`)
address is not. Not part of the installed SDK (src/detail/ is excluded from
the LIB_HEADERS install glob in CMakeLists.txt).

  tcp://host:port      -> scheme "tcp", host, port   (host may be "*", a
                          name, an IPv4 or a bracketed IPv6 literal; port may
                          be "*" for an ephemeral bind)
  ipc://path           -> scheme "ipc", path         (a filesystem path; on
                          Windows an AF_UNIX path like C:\...\x.ipc)
  inproc://name        -> scheme "inproc", path
*/
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Mads::detail {

struct Endpoint {
  std::string scheme; // "tcp", "ipc", "inproc"
  std::string host;   // tcp only
  std::string port;   // tcp only, digits or "*"
  std::string path;   // ipc/inproc only

  bool is_tcp() const { return scheme == "tcp"; }
  bool is_ipc() const { return scheme == "ipc"; }
  bool is_inproc() const { return scheme == "inproc"; }

  // Numeric port, if this is a tcp endpoint with a concrete port.
  std::optional<uint16_t> port_number() const {
    if (!is_tcp() || port.empty() || port == "*")
      return std::nullopt;
    unsigned long v = 0;
    for (char c : port) {
      if (c < '0' || c > '9')
        return std::nullopt;
      v = v * 10 + static_cast<unsigned long>(c - '0');
      if (v > 65535)
        return std::nullopt;
    }
    return static_cast<uint16_t>(v);
  }

  std::string to_string() const {
    if (is_tcp())
      return scheme + "://" + host + ":" + port;
    return scheme + "://" + path;
  }

  // Same endpoint, different host (tcp only; other schemes are returned
  // unchanged). Used by Agent::init() to point the [agents] addresses at the
  // host the settings were fetched from.
  Endpoint with_host(std::string const &new_host) const {
    Endpoint e = *this;
    if (e.is_tcp())
      e.host = new_host;
    return e;
  }
};

// True if `s` names a ZeroMQ endpoint MADS understands (as opposed to a
// settings file path or "none").
inline bool is_zmq_uri(std::string_view s) {
  return s.rfind("tcp://", 0) == 0 || s.rfind("ipc://", 0) == 0 ||
         s.rfind("inproc://", 0) == 0;
}

inline std::optional<Endpoint> parse_endpoint(std::string_view s) {
  auto sep = s.find("://");
  if (sep == std::string_view::npos || sep == 0)
    return std::nullopt;
  Endpoint e;
  e.scheme = std::string(s.substr(0, sep));
  std::string_view rest = s.substr(sep + 3);
  if (e.scheme == "ipc" || e.scheme == "inproc") {
    if (rest.empty())
      return std::nullopt;
    e.path = std::string(rest);
    return e;
  }
  if (e.scheme != "tcp")
    return std::nullopt;
  // tcp: host may be a bracketed IPv6 literal, so split on the last ':'
  // outside the brackets.
  size_t colon;
  if (!rest.empty() && rest.front() == '[') {
    auto close = rest.find(']');
    if (close == std::string_view::npos || close + 1 >= rest.size() ||
        rest[close + 1] != ':')
      return std::nullopt;
    colon = close + 1;
  } else {
    colon = rest.rfind(':');
    if (colon == std::string_view::npos)
      return std::nullopt;
  }
  e.host = std::string(rest.substr(0, colon));
  e.port = std::string(rest.substr(colon + 1));
  if (e.host.empty() || e.port.empty())
    return std::nullopt;
  if (e.port != "*" && !e.port_number())
    return std::nullopt;
  return e;
}

inline Endpoint parse_endpoint_or_throw(std::string_view s) {
  auto e = parse_endpoint(s);
  if (!e)
    throw std::invalid_argument("Invalid endpoint: " + std::string(s));
  return *e;
}

} // namespace Mads::detail
