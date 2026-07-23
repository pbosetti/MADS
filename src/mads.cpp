#include "mads.hpp"

namespace Mads {

std::atomic<bool> &Runtime::process_running() noexcept {
  static std::atomic<bool> flag{true};
  return flag;
}

} // namespace Mads
