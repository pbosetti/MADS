/*
Internal helper: a stable identifier for "the clock this process reads" (a
*clock domain*), used to key clock-offset consensus (clock_offset.hpp's
ClockConsensus) so that several agents sharing one host's clock adopt one
identical offset rather than each measuring independently and disagreeing by
measurement noise.

The domain is the *kernel*, not the host name: on Linux it is
/proc/sys/kernel/random/boot_id, a value the kernel generates once at boot
and keeps stable for its lifetime. This is the case that matters -- several
containers sharing one kernel have different hostnames but read the exact
same clock, so keying on hostname would split one clock domain into several
and reintroduce the very disagreement this exists to prevent.

macOS, Windows and any other platform fall back to the hostname. Correct for
every non-container deployment; containers are not a macOS/Windows MADS
target, so this is a documented limitation rather than a fragile
uptime-based reconstruction of boot time.

Not part of the installed SDK (src/detail/ is excluded from the LIB_HEADERS
install glob in CMakeLists.txt, same as detail/plugin_cache.hpp).
*/
#pragma once

#include <fstream>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <unistd.h>
#endif

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 255
#endif

namespace Mads::detail {

/**
 * @brief Stable identifier for the clock this process reads. Computed once
 * (function-local static, thread-safe init) and cached for the life of the
 * process.
 */
inline const std::string &clock_domain_id() {
  static const std::string id = [] {
#ifdef __linux__
    std::ifstream f("/proc/sys/kernel/random/boot_id");
    if (f) {
      std::string line;
      std::getline(f, line);
      if (!line.empty())
        return line;
    }
#endif
    char hostname[HOST_NAME_MAX + 1] = {0};
    if (gethostname(hostname, HOST_NAME_MAX) == 0)
      return std::string(hostname);
    return std::string("unknown");
  }();
  return id;
}

} // namespace Mads::detail
