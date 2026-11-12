#include "precise_wait.hpp"

#include <algorithm>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace {

#ifdef _WIN32
struct HiResTimer {
  HANDLE h = CreateWaitableTimerExW(nullptr, nullptr,
                                    CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                    TIMER_ALL_ACCESS);
  ~HiResTimer() {
    if (h) {
      CloseHandle(h);
    }
  }
};

// Sleep()/sleep_for wake on the 15.6 ms system tick; the high-resolution
// waitable timer does not, and unlike timeBeginPeriod it has no process-wide
// side effect.
thread_local HiResTimer t_timer;

void wait_slice(std::chrono::steady_clock::time_point until) {
  auto rel = std::chrono::duration_cast<std::chrono::nanoseconds>(
      until - std::chrono::steady_clock::now());
  if (rel <= std::chrono::nanoseconds(0)) {
    return;
  }
  if (t_timer.h) {
    LARGE_INTEGER due;
    // Negative value means relative time, in 100 ns units; at least 1 unit.
    due.QuadPart = -std::max<long long>(1, rel.count() / 100);
    if (SetWaitableTimer(t_timer.h, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(t_timer.h, INFINITE);
      return;
    }
  }
  std::this_thread::sleep_until(until);
}
#else
void wait_slice(std::chrono::steady_clock::time_point until) {
  std::this_thread::sleep_until(until);
}
#endif

} // namespace

namespace Mads {

bool precise_wait_until(std::chrono::steady_clock::time_point deadline,
                        const std::function<bool()> &keep_going,
                        std::chrono::nanoseconds max_slice) {
  while (true) {
    if (!keep_going()) {
      return false;
    }
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return true;
    }
    auto until = std::min(deadline, now + max_slice);
    wait_slice(until);
  }
}

} // namespace Mads
