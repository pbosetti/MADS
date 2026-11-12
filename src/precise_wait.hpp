/*
Precise, interruptible wait for paced playback.

Waits in short slices so a caller-supplied stop predicate is polled regularly.
Each slice is sized from the absolute deadline, so repeated calls do not
accumulate sleep error. No Windows types are exposed; the high-resolution
timer lives in precise_wait.cpp.

Author(s): Paolo Bosetti
*/

#pragma once

#include <chrono>
#include <functional>

namespace Mads {

/**
 * @brief Blocks until `deadline` or until `keep_going()` returns false.
 *
 * @param deadline Absolute steady-clock time to wait for.
 * @param keep_going Polled at least once per `max_slice`; returning false
 * stops the wait early.
 * @param max_slice Longest single wait between two `keep_going()` calls.
 * @return true if the deadline was reached, false if stopped early.
 */
bool precise_wait_until(std::chrono::steady_clock::time_point deadline,
                        const std::function<bool()> &keep_going,
                        std::chrono::nanoseconds max_slice =
                            std::chrono::milliseconds(100));

}
