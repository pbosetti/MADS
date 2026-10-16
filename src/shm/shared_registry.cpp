// Portable part of the shared-memory segment registry (see
// shared_segment.hpp): reading it. Writing and sweeping are per platform.
#include "shm/shared_segment.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>

namespace Mads::shm {

std::vector<RegistryEntry>
scan_registry(std::filesystem::path const &dir,
              std::function<bool(int64_t)> const &is_alive) {
  std::vector<RegistryEntry> out;
  try {
    std::error_code ec;
    for (auto const &entry : std::filesystem::directory_iterator(dir, ec)) {
      std::string name = entry.path().filename().string();
      if (name.rfind("mads_", 0) != 0)
        continue;
      RegistryEntry e;
      e.name = std::move(name);
      std::ifstream f(entry.path());
      long long pid = 0;
      if (f >> pid && pid > 0) {
        e.pid = pid;
        e.alive = is_alive(pid);
      }
      out.push_back(std::move(e));
    }
  } catch (...) {
  }
  std::sort(out.begin(), out.end(),
            [](auto const &a, auto const &b) { return a.name < b.name; });
  return out;
}

} // namespace Mads::shm
