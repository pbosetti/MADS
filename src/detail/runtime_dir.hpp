/*
Internal helper: the per-user runtime directory holding MADS's local IPC
artifacts (broker ipc:// socket files, the shared-memory segment registry).
Not part of the installed SDK.

  Linux    $XDG_RUNTIME_DIR/mads, else /tmp/mads-<uid>
  macOS    /tmp/mads-<uid>   ($TMPDIR is too long for sun_path)
  Windows  %LOCALAPPDATA%\mads\run

The directory is created on first use with owner-only permissions, so the
filesystem is the access control for ipc:// endpoints.
*/
#pragma once

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Mads::detail {

// AF_UNIX sun_path is 104 bytes on macOS/BSD, 108 on Linux and Windows; keep
// a margin for the terminating NUL.
constexpr size_t IPC_PATH_MAX = 100;

// Creates `dir` if missing and makes sure it is private to this user: on
// POSIX it must be owned by the current user (a directory owned by someone
// else would let them impersonate the sockets in it) and is chmod'ed 0700.
// Throws std::runtime_error otherwise. `what` names the directory in errors.
inline void ensure_private_dir(std::filesystem::path const &dir,
                               std::string const &what = "Runtime dir") {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec)
    throw std::runtime_error("Cannot create " + what + " " + dir.string() +
                             ": " + ec.message());
#ifndef _WIN32
  struct stat st {};
  if (::stat(dir.c_str(), &st) != 0 || st.st_uid != ::getuid())
    throw std::runtime_error(what + " " + dir.string() +
                             " is not owned by the current user");
  ::chmod(dir.c_str(), 0700);
#endif
}

inline std::filesystem::path runtime_dir() {
  namespace fs = std::filesystem;
  fs::path dir;
#ifdef _WIN32
  const char *local = std::getenv("LOCALAPPDATA");
  if (local && *local)
    dir = fs::path(local) / "mads" / "run";
  else
    dir = fs::temp_directory_path() / "mads" / "run";
#else
  const char *xdg = std::getenv("XDG_RUNTIME_DIR");
  if (xdg && *xdg)
    dir = fs::path(xdg) / "mads";
  else
    dir = fs::path("/tmp") / ("mads-" + std::to_string(::getuid()));
#endif
  ensure_private_dir(dir);
  return dir;
}

// True if something exists at an ipc:// socket path. symlink_status(): a
// Windows AF_UNIX socket file is a reparse point that status() may fail to
// follow.
inline bool socket_file_exists(std::filesystem::path const &path) {
  std::error_code ec;
  return std::filesystem::exists(std::filesystem::symlink_status(path, ec));
}

// Default ipc:// endpoint for a broker role ("front", "back", "settings"),
// keyed by the broker's tcp port so that several brokers on one host do not
// collide. Throws if the path would not fit in sun_path.
inline std::string default_ipc_endpoint(std::filesystem::path const &dir,
                                        unsigned port, std::string const &role) {
  auto p = dir / ("broker-" + std::to_string(port) + "-" + role + ".ipc");
  std::string s = p.string();
  if (s.size() > IPC_PATH_MAX)
    throw std::runtime_error("ipc socket path too long (" +
                             std::to_string(s.size()) + " > " +
                             std::to_string(IPC_PATH_MAX) + "): " + s +
                             "; set a shorter ipc_dir");
  return "ipc://" + s;
}

} // namespace Mads::detail
