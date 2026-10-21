/*
Internal helper: the per-user configuration directory holding persistent
preferences of the MADS GUI apps (e.g. mads-chat settings). Not part of the
installed SDK.

  macOS    ~/Library/Application Support/MADS
  Linux    $XDG_CONFIG_HOME/mads, else ~/.config/mads
  Windows  %APPDATA%\MADS   (else %USERPROFILE%\AppData\Roaming\MADS)

$MADS_CONFIG_DIR, when set and non-empty, overrides all of the above (tests,
portable setups).

The directory is created on first use with owner-only permissions (see
ensure_private_dir in runtime_dir.hpp).
*/
#pragma once

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

#ifndef _WIN32
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "runtime_dir.hpp"

namespace Mads::detail {

// Value of an environment variable, or nullopt when it is unset or empty.
inline std::optional<std::string> env_value(char const *name) {
  const char *value = std::getenv(name);
  if (value && *value)
    return std::string(value);
  return std::nullopt;
}

// The user's home directory: $HOME, else the passwd entry of the current user
// on POSIX, else %USERPROFILE% on Windows. Throws if none of them is known.
inline std::filesystem::path user_home_dir() {
  if (auto home = env_value("HOME"))
    return *home;
#ifndef _WIN32
  if (const struct passwd *pw = ::getpwuid(::getuid()))
    if (pw->pw_dir && *pw->pw_dir)
      return pw->pw_dir;
#else
  if (auto profile = env_value("USERPROFILE"))
    return *profile;
#endif
  throw std::runtime_error("Cannot determine the home directory: HOME is not set");
}

// Returns the per-user configuration directory, creating it if needed. Throws
// std::runtime_error if it cannot be determined or created.
inline std::filesystem::path config_dir() {
  namespace fs = std::filesystem;
  fs::path dir;
  if (auto override_dir = env_value("MADS_CONFIG_DIR")) {
    dir = fs::path(*override_dir);
  } else {
#if defined(_WIN32)
    if (auto appdata = env_value("APPDATA"))
      dir = fs::path(*appdata) / "MADS";
    else
      dir = user_home_dir() / "AppData" / "Roaming" / "MADS";
#elif defined(__APPLE__)
    dir = user_home_dir() / "Library" / "Application Support" / "MADS";
#else
    if (auto xdg = env_value("XDG_CONFIG_HOME"))
      dir = fs::path(*xdg) / "mads";
    else
      dir = user_home_dir() / ".config" / "mads";
#endif
  }
  ensure_private_dir(dir, "Config dir");
  return dir;
}

} // namespace Mads::detail
