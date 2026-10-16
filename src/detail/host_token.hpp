/*
Internal helper: a string that identifies this machine, used to decide
whether a peer (the broker, or the producer of a shared-memory descriptor)
runs on the same host. Not part of the installed SDK.

The token is "<hostname>/<machine id>":
  Linux    /etc/machine-id (fallback /proc/sys/kernel/random/boot_id)
  macOS    gethostuuid()
  Windows  HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid

It is a fast "same host?" pre-check only: callers still verify that the ipc
socket file exists, and shared-memory consumers verify the segment nonce,
because containers can share a hostname without sharing a filesystem.
*/
#pragma once

#include <array>
#include <fstream>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#pragma comment(lib, "advapi32.lib")
#else
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <uuid/uuid.h>
#endif

namespace Mads::detail {

inline std::string machine_id() {
#if defined(_WIN32)
  char buf[64] = {};
  DWORD len = sizeof(buf);
  if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography",
                   "MachineGuid", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
                   nullptr, buf, &len) == ERROR_SUCCESS)
    return std::string(buf);
  return {};
#elif defined(__APPLE__)
  uuid_t id;
  struct timespec wait = {1, 0};
  if (gethostuuid(id, &wait) != 0)
    return {};
  uuid_string_t s;
  uuid_unparse_lower(id, s);
  return std::string(s);
#else
  for (const char *p :
       {"/etc/machine-id", "/proc/sys/kernel/random/boot_id"}) {
    std::ifstream f(p);
    std::string id;
    if (f && std::getline(f, id) && !id.empty())
      return id;
  }
  return {};
#endif
}

inline std::string host_token() {
  static const std::string token = [] {
    std::array<char, 256> name{};
#ifdef _WIN32
    // gethostname() needs WSAStartup(), which may not have happened yet.
    DWORD len = static_cast<DWORD>(name.size());
    if (!GetComputerNameExA(ComputerNameDnsHostname, name.data(), &len))
      name[0] = '\0';
#else
    if (gethostname(name.data(), name.size() - 1) != 0)
      name[0] = '\0';
#endif
    return std::string(name.data()) + "/" + machine_id();
  }();
  return token;
}

} // namespace Mads::detail
