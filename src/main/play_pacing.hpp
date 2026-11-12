/*
 mads-play's --rate pacing schedule.

 Pacer maps each record's recorded timestamp to the offset, from the start
 of replay, at which the record is due. Offsets are computed from a running
 total of bag time, so rounding error never accumulates. Timestamps that do
 not advance (equal or backward) add no bag time. --loop continues the
 schedule across passes via seam().

 Header-only so it is directly unit-testable (tests/test_play_pacing.cpp)
 without linking src/main/play.cpp's main(); mirrors the precedent of
 src/main/play_restamp.hpp.

 Author(s): Paolo Bosetti
*/
#ifndef MADS_MAIN_PLAY_PACING_HPP
#define MADS_MAIN_PLAY_PACING_HPP

#include <chrono>
#include <cmath>
#include <cstdint>

namespace Mads {
namespace Play {

class Pacer {
public:
  explicit Pacer(double rate) : _rate(rate) {}

  // Offset from the start of replay at which the record at ts_ns is due.
  std::chrono::nanoseconds offset(int64_t ts_ns) {
    if (_have_prev && ts_ns > _prev_ts) {
      _elapsed_ns += ts_ns - _prev_ts;
    }
    _prev_ts = ts_ns;
    _have_prev = true;
    return std::chrono::nanoseconds(std::llround(double(_elapsed_ns) / _rate));
  }

  // --loop: continue one span after the previous pass.
  void seam(int64_t span_ns) { _prev_ts -= span_ns; }

  // Total bag time consumed so far, for the summary line.
  int64_t elapsed_ns() const { return _elapsed_ns; }

private:
  double _rate;
  bool _have_prev = false;
  int64_t _prev_ts = 0;
  int64_t _elapsed_ns = 0;
};

} // namespace Play
} // namespace Mads

#endif // MADS_MAIN_PLAY_PACING_HPP
