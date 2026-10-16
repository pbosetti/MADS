/*
Internal helper: the broker side of the ipc:// local transport. Resolves the
[broker] ipc keys (see detail/transport.hpp) into the endpoints to bind next
to the tcp ones, binds them defensively, and builds the reply to the
`transport` settings command. Kept out of src/main/broker.cpp so the tests
can drive exactly the code the broker runs. Not part of the installed SDK.

Socket-file lifecycle, as libzmq 4.3.5 handles it (src/ipc_listener.cpp):
  - bind() unconditionally unlink()s the path first. A stale file left by a
    crashed broker is therefore harmless, but so would be a *live* broker's
    socket, silently stolen. bind_broker_ipc() probes the path first and
    refuses to take over one that still accepts connections.
  - close() does NOT unlink the file (only the temp files of wildcard ipc
    binds are removed), so the broker removes its own files on shutdown, but only if
    the file is still the one it created (IpcSocketFile).
*/
#pragma once

#include "endpoint.hpp"
#include "host_token.hpp"
#include "runtime_dir.hpp"
#include "transport.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <zmq.hpp>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace Mads::detail {

// ipc:// endpoints the broker offers; an empty string means "not offered".
struct BrokerIpcConfig {
  std::string frontend;
  std::string backend;
  std::string settings;
  std::vector<std::string> warnings; // to be printed by the caller
};

inline bool zmq_has_ipc() { return zmq_has("ipc") != 0; }

// Resolves the [broker] ipc keys. `section` is the broker's toml section
// (config[name]); the *_tcp arguments are the tcp bind addresses, whose port
// keys the derived ipc path. A role whose tcp address has no numeric port
// needs an explicit override, otherwise it is skipped with a warning.
template <typename Section>
BrokerIpcConfig resolve_broker_ipc(Section const &section,
                                   std::string const &frontend_tcp,
                                   std::string const &backend_tcp,
                                   std::string const &settings_tcp,
                                   bool ipc_available = zmq_has_ipc()) {
  BrokerIpcConfig out;
  if (!section["ipc"].value_or(BROKER_IPC_DEFAULT))
    return out;
  if (!ipc_available) {
    out.warnings.push_back(
        "this libzmq build has no ipc:// transport; serving tcp only");
    return out;
  }

  std::filesystem::path dir;
  std::string dir_error;
  try {
    if (auto d = section["ipc_dir"].template value<std::string>();
        d && !d->empty()) {
      dir = *d;
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      if (ec)
        dir_error = "cannot create ipc_dir " + *d + ": " + ec.message();
    } else {
      dir = runtime_dir();
    }
  } catch (std::exception const &e) {
    dir_error = e.what();
  }

  struct Role {
    const char *key;
    const char *role;
    std::string const &tcp;
    std::string &out;
  };
  Role roles[] = {{"frontend_ipc", "front", frontend_tcp, out.frontend},
                  {"backend_ipc", "back", backend_tcp, out.backend},
                  {"settings_ipc", "settings", settings_tcp, out.settings}};
  for (auto &r : roles) {
    if (auto o = section[r.key].template value<std::string>();
        o && !o->empty()) {
      auto e = parse_endpoint(*o);
      if (!e || !e->is_ipc()) {
        out.warnings.push_back(std::string(r.key) + " = \"" + *o +
                               "\" is not an ipc:// endpoint; ignored");
      } else if (e->path.size() > IPC_PATH_MAX) {
        out.warnings.push_back(std::string(r.key) + " path is too long (" +
                               std::to_string(e->path.size()) + " > " +
                               std::to_string(IPC_PATH_MAX) + "); ignored");
      } else {
        r.out = *o;
      }
      continue;
    }
    if (!dir_error.empty()) {
      out.warnings.push_back("no ipc endpoint for " + std::string(r.role) +
                             ": " + dir_error);
      continue;
    }
    auto tcp = parse_endpoint(r.tcp);
    auto port = tcp ? tcp->port_number() : std::nullopt;
    if (!port) {
      out.warnings.push_back("no ipc endpoint for " + std::string(r.role) +
                             ": " + r.tcp +
                             " has no numeric port, set " + r.key +
                             " explicitly");
      continue;
    }
    try {
      r.out = default_ipc_endpoint(dir, *port, r.role);
    } catch (std::exception const &e) {
      out.warnings.push_back("no ipc endpoint for " + std::string(r.role) +
                             ": " + e.what());
    }
  }
  return out;
}

enum class IpcSocketState {
  Absent,  // no file at the path
  Stale,   // a socket file nobody listens on (e.g. left by a crash)
  Live,    // a socket file some process still accepts connections on
  Foreign, // something that is not a socket: never unlink it
};

// What is currently at an ipc:// path. On Windows an AF_UNIX socket file
// cannot be told apart cheaply, so an existing file is reported as Stale
// (libzmq replaces it on bind, as it always did).
inline IpcSocketState probe_ipc_socket(std::string const &path) {
#ifdef _WIN32
  std::error_code ec;
  return std::filesystem::exists(path, ec) ? IpcSocketState::Stale
                                           : IpcSocketState::Absent;
#else
  struct stat st {};
  if (::lstat(path.c_str(), &st) != 0)
    return IpcSocketState::Absent;
  if (!S_ISSOCK(st.st_mode))
    return IpcSocketState::Foreign;
  sockaddr_un addr{};
  if (path.size() >= sizeof(addr.sun_path))
    return IpcSocketState::Stale;
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return IpcSocketState::Stale;
  // Non-blocking, so a listener with a full backlog cannot hang the probe
  // (Linux blocks a stream AF_UNIX connect() in that case): EAGAIN then
  // still means somebody is listening.
  ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, path.c_str(), path.size());
  const int rc =
      ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
  const int err = errno;
  ::close(fd);
  if (rc == 0 || err == EAGAIN || err == EINPROGRESS)
    return IpcSocketState::Live;
  return IpcSocketState::Stale;
#endif
}

// A socket file this process created. remove() deletes it only if it is
// still the same file, so a broker shutting down late never deletes the
// socket of a newer broker that has since bound the same path.
class IpcSocketFile {
public:
  void claim(std::string const &path) {
    _path = path;
#ifndef _WIN32
    struct stat st {};
    if (::lstat(path.c_str(), &st) == 0) {
      _dev = static_cast<uint64_t>(st.st_dev);
      _ino = static_cast<uint64_t>(st.st_ino);
    }
#endif
  }

  void remove() {
    if (_path.empty())
      return;
#ifndef _WIN32
    struct stat st {};
    if (::lstat(_path.c_str(), &st) == 0 &&
        static_cast<uint64_t>(st.st_dev) == _dev &&
        static_cast<uint64_t>(st.st_ino) == _ino)
      ::unlink(_path.c_str());
#else
    std::error_code ec;
    std::filesystem::remove(_path, ec);
#endif
    _path.clear();
  }

  std::string const &path() const { return _path; }

private:
  std::string _path;
  uint64_t _dev = 0;
  uint64_t _ino = 0;
};

// Binds `socket` a second time on `endpoint` (an ipc:// endpoint, or empty to
// do nothing). On any failure the endpoint is cleared -- so it is not offered
// to agents -- and a warning is appended; tcp service is never affected.
// Returns true if the ipc endpoint is bound.
inline bool bind_broker_ipc(zmq::socket_t &socket, std::string &endpoint,
                            IpcSocketFile &file,
                            std::vector<std::string> &warnings) {
  if (endpoint.empty())
    return false;
  auto e = parse_endpoint(endpoint);
  if (!e || !e->is_ipc()) {
    warnings.push_back(endpoint + " is not an ipc:// endpoint");
    endpoint.clear();
    return false;
  }
  switch (probe_ipc_socket(e->path)) {
  case IpcSocketState::Live:
    warnings.push_back(e->path +
                       " is in use by another process; not binding " +
                       endpoint);
    endpoint.clear();
    return false;
  case IpcSocketState::Foreign:
    warnings.push_back(e->path + " exists and is not a socket; not binding " +
                       endpoint);
    endpoint.clear();
    return false;
  default: // Absent, or Stale: libzmq unlinks it before binding
    break;
  }
  try {
    socket.bind(endpoint);
  } catch (zmq::error_t const &err) {
    warnings.push_back("could not bind " + endpoint + ": " + err.what());
    endpoint.clear();
    return false;
  }
  file.claim(e->path);
  return true;
}

// The one-frame reply to the `transport` settings command.
inline std::string transport_reply(BrokerIpcConfig const &ipc, bool curve) {
  TransportInfo info;
  info.host_token = host_token();
  info.frontend_ipc = ipc.frontend;
  info.backend_ipc = ipc.backend;
  info.settings_ipc = ipc.settings;
  info.curve = curve;
  return nlohmann::json(info).dump();
}

} // namespace Mads::detail
