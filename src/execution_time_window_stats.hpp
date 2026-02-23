/*
  _____                     _   _                 
 | ____|_  _____  ___ _   _| |_(_) ___  _ __  ___ 
 |  _| \ \/ / _ \/ __| | | | __| |/ _ \| '_ \/ __|
 | |___ >  <  __/ (__| |_| | |_| | (_) | | | \__ \
 |_____/_/\_\___|\___|\__,_|\__|_|\___/|_| |_|___/

Running window statistics for execution times.
*/

#ifndef EXECUTION_TIME_WINDOW_STATS_HPP
#define EXECUTION_TIME_WINDOW_STATS_HPP

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <deque>

namespace Mads {

/**
 * @brief Running window statistics for execution time samples.
 *
 * This header-only utility stores the latest execution durations (in
 * milliseconds) up to a configurable window size and provides:
 * - arithmetic mean (`average_ms()`)
 * - population standard deviation (`stddev_ms()`)
 *
 * Typical usage:
 * @code
 * Mads::ExecutionTimeWindowStats stats(100);
 * stats.tic();
 * // ... code under test ...
 * double elapsed_ms = stats.toc();
 * @endcode
 */
class ExecutionTimeWindowStats {
public:
  /**
   * @brief Construct a statistics window with a maximum number of samples.
   *
   * @param window_width Maximum number of latest samples retained.
   */
  explicit ExecutionTimeWindowStats(std::size_t window_width = 100)
      : _window_width(window_width) {}

  /**
   * @brief Mark the start time of the next measured interval.
   *
   * Call `tic()` before the code section you want to time, then call `toc()`
   * to store the elapsed duration.
   */
  void tic() {
    _start = clock_t::now();
    _ticking = true;
  }

  /**
   * @brief Mark the stop time, compute elapsed time, and append it.
   *
   * If `tic()` was not called, this method returns `0.0` and does not append
   * any sample.
   *
   * @return Elapsed duration in milliseconds.
   */
  double toc() {
    const auto stop = clock_t::now();
    if (!_ticking) {
      return 0.0;
    }
    _ticking = false;
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(stop - _start).count();
    append_duration(elapsed_ms);
    return elapsed_ms;
  }

  /**
   * @brief Set the running window width at runtime.
   *
   * If reduced, the oldest samples are dropped immediately. If increased,
   * existing samples are preserved.
   *
   * @param window_width New maximum number of retained samples.
   */
  void set_window_width(std::size_t window_width) {
    _window_width = window_width;
    trim_to_window();
  }

  /**
   * @brief Get the current configured window width.
   *
   * @return Maximum number of retained samples.
   */
  std::size_t window_width() const { return _window_width; }

  /**
   * @brief Get the number of currently stored samples.
   *
   * @return Current sample count in the running window.
   */
  std::size_t size() const { return _samples.size(); }

  /**
   * @brief Get the arithmetic mean of stored samples.
   *
   * @return Mean duration in milliseconds, or `0.0` when empty.
   */
  double average_ms() const {
    if (_samples.empty()) {
      return 0.0;
    }
    return _sum / static_cast<double>(_samples.size());
  }

  /**
   * @brief Get the population standard deviation of stored samples.
   *
   * Uses:
   * \f[
   * \sigma = \sqrt{E[x^2] - E[x]^2}
   * \f]
   *
   * @return Standard deviation in milliseconds, or `0.0` when empty.
   */
  double stddev_ms() const {
    if (_samples.empty()) {
      return 0.0;
    }
    const double n = static_cast<double>(_samples.size());
    const double mean = _sum / n;
    const double variance = std::max(0.0, (_sum_sq / n) - (mean * mean));
    return std::sqrt(variance);
  }

  /**
   * @brief Clear all stored samples and reset timing state.
   */
  void clear() {
    _samples.clear();
    _sum = 0.0;
    _sum_sq = 0.0;
    _ticking = false;
  }

private:
  using clock_t = std::chrono::steady_clock;

  /**
   * @brief Append one measured duration to the running window.
   *
   * @param duration_ms Duration in milliseconds.
   */
  void append_duration(double duration_ms) {
    if (_window_width == 0) {
      return;
    }
    _samples.push_back(duration_ms);
    _sum += duration_ms;
    _sum_sq += duration_ms * duration_ms;
    trim_to_window();
  }

  /**
   * @brief Enforce current window width by removing oldest samples.
   */
  void trim_to_window() {
    while (_samples.size() > _window_width) {
      const double oldest = _samples.front();
      _samples.pop_front();
      _sum -= oldest;
      _sum_sq -= oldest * oldest;
    }
    if (_samples.empty()) {
      _sum = 0.0;
      _sum_sq = 0.0;
    }
  }

  std::size_t _window_width = 0;
  std::deque<double> _samples;
  double _sum = 0.0;
  double _sum_sq = 0.0;
  clock_t::time_point _start{};
  bool _ticking = false;
};

} // namespace Mads

#endif // EXECUTION_TIME_WINDOW_STATS_HPP
