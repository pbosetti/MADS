#pragma once

#include <SlidingDFT/allocator.hpp>
#include <SlidingDFT/types.hpp>

/**
 * @class SlidingDFT
 * @brief Sliding discrete Fourier transform for real-valued input streams.
 *
 * The class maintains a running DFT over a fixed-size window of the most recent
 * samples. SIMD acceleration is selected at compile time when AVX2 or NEON is
 * available; otherwise a scalar implementation is used.
 */
class SlidingDFT {
public:
  using vec_t = std::vector<data_t, AlignedAllocator<data_t, 32>>;

  /**
   * @brief Construct a sliding DFT for a fixed window length.
   * @param window_size Number of samples in the analysis window.
   * @param renorm_interval Number of updates between phase renormalizations.
   * @throws std::invalid_argument If @p window_size is zero.
   */
  SlidingDFT(std::size_t window_size, std::size_t renorm_interval = 4096);

  /**
   * @brief Push one new sample into the window and update all bins.
   * @param sample New sample value.
   */
  void update(data_t sample);

  /**
   * @brief Compute the current power spectrum.
   * @param power_spectrum Output vector resized to `bins()` elements.
   */
  void get_power(vec_t &power_spectrum) const;

  /**
   * @brief Return the number of frequency bins.
   * @return `window_size / 2 + 1`.
   */
  [[nodiscard]] std::size_t bins() const noexcept;

private:
  void update_bins(data_t delta);
  void advance_phase();
  void power(data_t *out) const;
  void renormalize_phase();
  void recompute_fft_like();

  std::size_t _n;
  std::size_t _k;
  vec_t _buffer;
  vec_t _yr;
  vec_t _yi;
  vec_t _wr;
  vec_t _wi;
  vec_t _pr;
  vec_t _pi;
  std::size_t _index;
  std::size_t _filled;
  std::size_t _count;
  std::size_t _renorm_interval;
};

using SlidingDFT_AVX2 = SlidingDFT;
