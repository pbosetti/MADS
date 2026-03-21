#include <SlidingDFT/sliding_dft.hpp>

#include <cmath>
#include <numbers>
#include <stdexcept>

namespace {

#if defined(__AVX2__) && defined(SLIDINGDFT_USE_DOUBLE)
using vec = __m256d;

inline vec load(const data_t *p) { return _mm256_load_pd(p); }

inline void store(data_t *p, vec v) { _mm256_store_pd(p, v); }
#elif defined(__AVX2__)
using vec = __m256;

inline vec load(const data_t *p) { return _mm256_load_ps(p); }

inline void store(data_t *p, vec v) { _mm256_store_ps(p, v); }
#elif defined(__ARM_NEON) && defined(SLIDINGDFT_USE_DOUBLE)
using vec = float64x2_t;

inline vec load(const data_t *p) { return vld1q_f64(p); }

inline void store(data_t *p, vec v) { vst1q_f64(p, v); }
#elif defined(__ARM_NEON)
using vec = float32x4_t;

inline vec load(const data_t *p) { return vld1q_f32(p); }

inline void store(data_t *p, vec v) { vst1q_f32(p, v); }
#endif

} // namespace

SlidingDFT::SlidingDFT(std::size_t window_size, std::size_t renorm_interval)
    : _n(window_size), _k(window_size / 2 + 1), _buffer(window_size, 0),
      _yr(_k, 0), _yi(_k, 0), _wr(_k, 0), _wi(_k, 0), _pr(_k, 1), _pi(_k, 0),
      _index(0), _filled(0), _count(0), _renorm_interval(renorm_interval) {
  if (_n == 0) {
    throw std::invalid_argument("N > 0 required");
  }

  const data_t two_pi_over_n = static_cast<data_t>(
      2.0 * std::numbers::pi_v<double> / static_cast<double>(_n));

  for (std::size_t k = 0; k < _k; ++k) {
    const data_t angle = two_pi_over_n * static_cast<data_t>(k);
    _wr[k] = std::cos(angle);
    _wi[k] = -std::sin(angle);
    _pr[k] = 1;
    _pi[k] = 0;
  }
}

void SlidingDFT::update(data_t sample) {
  const data_t old_sample = _buffer[_index];
  _buffer[_index] = sample;
  _index = (_index + 1) % _n;

  if (_filled < _n) {
    ++_filled;
  }

  const data_t delta = sample - old_sample;
  update_bins(delta);
  advance_phase();

  if (++_count % _renorm_interval == 0) {
    renormalize_phase();
    recompute_fft_like();
  }
}

void SlidingDFT::get_power(vec_t &power_spectrum) const {
  power_spectrum.resize(_k);
  power(power_spectrum.data());
}

std::size_t SlidingDFT::bins() const noexcept { return _k; }

void SlidingDFT::update_bins(data_t delta) {
#if defined(__AVX2__) && defined(SLIDINGDFT_USE_DOUBLE)
  const vec vdelta = _mm256_set1_pd(delta);
  std::size_t k = 0;
  for (; k + VEC_WIDTH <= _k; k += VEC_WIDTH) {
    vec yr = load(&_yr[k]);
    const vec yi = load(&_yi[k]);
    const vec wr = load(&_wr[k]);
    const vec wi = load(&_wi[k]);

    yr = _mm256_add_pd(yr, vdelta);

#if defined(__FMA__)
    const vec real = _mm256_fmsub_pd(yr, wr, _mm256_mul_pd(yi, wi));
    const vec imag = _mm256_fmadd_pd(yr, wi, _mm256_mul_pd(yi, wr));
#else
    const vec real =
        _mm256_sub_pd(_mm256_mul_pd(yr, wr), _mm256_mul_pd(yi, wi));
    const vec imag =
        _mm256_add_pd(_mm256_mul_pd(yr, wi), _mm256_mul_pd(yi, wr));
#endif

    store(&_yr[k], real);
    store(&_yi[k], imag);
  }

  for (; k < _k; ++k) {
    const data_t yr = _yr[k] + delta;
    const data_t yi = _yi[k];
    _yr[k] = yr * _wr[k] - yi * _wi[k];
    _yi[k] = yr * _wi[k] + yi * _wr[k];
  }
#elif defined(__AVX2__)
  const vec vdelta = _mm256_set1_ps(delta);
  std::size_t k = 0;
  for (; k + VEC_WIDTH <= _k; k += VEC_WIDTH) {
    vec yr = load(&_yr[k]);
    const vec yi = load(&_yi[k]);
    const vec wr = load(&_wr[k]);
    const vec wi = load(&_wi[k]);

    yr = _mm256_add_ps(yr, vdelta);

#if defined(__FMA__)
    const vec real = _mm256_fmsub_ps(yr, wr, _mm256_mul_ps(yi, wi));
    const vec imag = _mm256_fmadd_ps(yr, wi, _mm256_mul_ps(yi, wr));
#else
    const vec real =
        _mm256_sub_ps(_mm256_mul_ps(yr, wr), _mm256_mul_ps(yi, wi));
    const vec imag =
        _mm256_add_ps(_mm256_mul_ps(yr, wi), _mm256_mul_ps(yi, wr));
#endif

    store(&_yr[k], real);
    store(&_yi[k], imag);
  }

  for (; k < _k; ++k) {
    const data_t yr = _yr[k] + delta;
    const data_t yi = _yi[k];
    _yr[k] = yr * _wr[k] - yi * _wi[k];
    _yi[k] = yr * _wi[k] + yi * _wr[k];
  }
#elif defined(__ARM_NEON) && defined(SLIDINGDFT_USE_DOUBLE)
  const vec vdelta = vdupq_n_f64(delta);
  std::size_t k = 0;
  for (; k + VEC_WIDTH <= _k; k += VEC_WIDTH) {
    vec yr = load(&_yr[k]);
    const vec yi = load(&_yi[k]);
    const vec wr = load(&_wr[k]);
    const vec wi = load(&_wi[k]);

    yr = vaddq_f64(yr, vdelta);

    const vec real = vsubq_f64(vmulq_f64(yr, wr), vmulq_f64(yi, wi));
    const vec imag = vaddq_f64(vmulq_f64(yr, wi), vmulq_f64(yi, wr));

    store(&_yr[k], real);
    store(&_yi[k], imag);
  }

  for (; k < _k; ++k) {
    const data_t yr = _yr[k] + delta;
    const data_t yi = _yi[k];
    _yr[k] = yr * _wr[k] - yi * _wi[k];
    _yi[k] = yr * _wi[k] + yi * _wr[k];
  }
#elif defined(__ARM_NEON)
  const vec vdelta = vdupq_n_f32(delta);
  std::size_t k = 0;
  for (; k + VEC_WIDTH <= _k; k += VEC_WIDTH) {
    vec yr = load(&_yr[k]);
    const vec yi = load(&_yi[k]);
    const vec wr = load(&_wr[k]);
    const vec wi = load(&_wi[k]);

    yr = vaddq_f32(yr, vdelta);

    const vec real = vsubq_f32(vmulq_f32(yr, wr), vmulq_f32(yi, wi));
    const vec imag = vaddq_f32(vmulq_f32(yr, wi), vmulq_f32(yi, wr));

    store(&_yr[k], real);
    store(&_yi[k], imag);
  }

  for (; k < _k; ++k) {
    const data_t yr = _yr[k] + delta;
    const data_t yi = _yi[k];
    _yr[k] = yr * _wr[k] - yi * _wi[k];
    _yi[k] = yr * _wi[k] + yi * _wr[k];
  }
#else
  for (std::size_t k = 0; k < _k; ++k) {
    const data_t yr = _yr[k] + delta;
    const data_t yi = _yi[k];
    _yr[k] = yr * _wr[k] - yi * _wi[k];
    _yi[k] = yr * _wi[k] + yi * _wr[k];
  }
#endif
}

void SlidingDFT::advance_phase() {
#if defined(__AVX2__) && defined(SLIDINGDFT_USE_DOUBLE)
  std::size_t k = 0;
  for (; k + VEC_WIDTH <= _k; k += VEC_WIDTH) {
    const vec pr = load(&_pr[k]);
    const vec pi = load(&_pi[k]);
    const vec dr = load(&_wr[k]);
    const vec di = _mm256_sub_pd(_mm256_setzero_pd(), load(&_wi[k]));

#if defined(__FMA__)
    const vec nr = _mm256_fmsub_pd(pr, dr, _mm256_mul_pd(pi, di));
    const vec ni = _mm256_fmadd_pd(pr, di, _mm256_mul_pd(pi, dr));
#else
    const vec nr =
        _mm256_sub_pd(_mm256_mul_pd(pr, dr), _mm256_mul_pd(pi, di));
    const vec ni =
        _mm256_add_pd(_mm256_mul_pd(pr, di), _mm256_mul_pd(pi, dr));
#endif

    store(&_pr[k], nr);
    store(&_pi[k], ni);
  }

  for (; k < _k; ++k) {
    const data_t dr = _wr[k];
    const data_t di = -_wi[k];
    const data_t pr = _pr[k];
    const data_t pi = _pi[k];
    _pr[k] = pr * dr - pi * di;
    _pi[k] = pr * di + pi * dr;
  }
#elif defined(__AVX2__)
  std::size_t k = 0;
  for (; k + VEC_WIDTH <= _k; k += VEC_WIDTH) {
    const vec pr = load(&_pr[k]);
    const vec pi = load(&_pi[k]);
    const vec dr = load(&_wr[k]);
    const vec di = _mm256_sub_ps(_mm256_setzero_ps(), load(&_wi[k]));

#if defined(__FMA__)
    const vec nr = _mm256_fmsub_ps(pr, dr, _mm256_mul_ps(pi, di));
    const vec ni = _mm256_fmadd_ps(pr, di, _mm256_mul_ps(pi, dr));
#else
    const vec nr =
        _mm256_sub_ps(_mm256_mul_ps(pr, dr), _mm256_mul_ps(pi, di));
    const vec ni =
        _mm256_add_ps(_mm256_mul_ps(pr, di), _mm256_mul_ps(pi, dr));
#endif

    store(&_pr[k], nr);
    store(&_pi[k], ni);
  }

  for (; k < _k; ++k) {
    const data_t dr = _wr[k];
    const data_t di = -_wi[k];
    const data_t pr = _pr[k];
    const data_t pi = _pi[k];
    _pr[k] = pr * dr - pi * di;
    _pi[k] = pr * di + pi * dr;
  }
#elif defined(__ARM_NEON) && defined(SLIDINGDFT_USE_DOUBLE)
  std::size_t k = 0;
  for (; k + VEC_WIDTH <= _k; k += VEC_WIDTH) {
    const vec pr = load(&_pr[k]);
    const vec pi = load(&_pi[k]);
    const vec dr = load(&_wr[k]);
    const vec di = vnegq_f64(load(&_wi[k]));

    const vec nr = vsubq_f64(vmulq_f64(pr, dr), vmulq_f64(pi, di));
    const vec ni = vaddq_f64(vmulq_f64(pr, di), vmulq_f64(pi, dr));

    store(&_pr[k], nr);
    store(&_pi[k], ni);
  }

  for (; k < _k; ++k) {
    const data_t dr = _wr[k];
    const data_t di = -_wi[k];
    const data_t pr = _pr[k];
    const data_t pi = _pi[k];
    _pr[k] = pr * dr - pi * di;
    _pi[k] = pr * di + pi * dr;
  }
#elif defined(__ARM_NEON)
  std::size_t k = 0;
  for (; k + VEC_WIDTH <= _k; k += VEC_WIDTH) {
    const vec pr = load(&_pr[k]);
    const vec pi = load(&_pi[k]);
    const vec dr = load(&_wr[k]);
    const vec di = vnegq_f32(load(&_wi[k]));

    const vec nr = vsubq_f32(vmulq_f32(pr, dr), vmulq_f32(pi, di));
    const vec ni = vaddq_f32(vmulq_f32(pr, di), vmulq_f32(pi, dr));

    store(&_pr[k], nr);
    store(&_pi[k], ni);
  }

  for (; k < _k; ++k) {
    const data_t dr = _wr[k];
    const data_t di = -_wi[k];
    const data_t pr = _pr[k];
    const data_t pi = _pi[k];
    _pr[k] = pr * dr - pi * di;
    _pi[k] = pr * di + pi * dr;
  }
#else
  for (std::size_t k = 0; k < _k; ++k) {
    const data_t dr = _wr[k];
    const data_t di = -_wi[k];
    const data_t pr = _pr[k];
    const data_t pi = _pi[k];
    _pr[k] = pr * dr - pi * di;
    _pi[k] = pr * di + pi * dr;
  }
#endif
}

void SlidingDFT::power(data_t *out) const {
#if defined(__AVX2__) && defined(SLIDINGDFT_USE_DOUBLE)
  std::size_t k = 0;
  for (; k + VEC_WIDTH <= _k; k += VEC_WIDTH) {
    const vec yr = load(&_yr[k]);
    const vec yi = load(&_yi[k]);
    const vec pr = load(&_pr[k]);
    const vec pi = load(&_pi[k]);

#if defined(__FMA__)
    const vec xr = _mm256_fmsub_pd(yr, pr, _mm256_mul_pd(yi, pi));
    const vec xi = _mm256_fmadd_pd(yr, pi, _mm256_mul_pd(yi, pr));
    const vec pw = _mm256_fmadd_pd(xr, xr, _mm256_mul_pd(xi, xi));
#else
    const vec xr =
        _mm256_sub_pd(_mm256_mul_pd(yr, pr), _mm256_mul_pd(yi, pi));
    const vec xi =
        _mm256_add_pd(_mm256_mul_pd(yr, pi), _mm256_mul_pd(yi, pr));
    const vec pw =
        _mm256_add_pd(_mm256_mul_pd(xr, xr), _mm256_mul_pd(xi, xi));
#endif

    store(&out[k], pw);
  }

  for (; k < _k; ++k) {
    const data_t xr = _yr[k] * _pr[k] - _yi[k] * _pi[k];
    const data_t xi = _yr[k] * _pi[k] + _yi[k] * _pr[k];
    out[k] = xr * xr + xi * xi;
  }
#elif defined(__AVX2__)
  std::size_t k = 0;
  for (; k + VEC_WIDTH <= _k; k += VEC_WIDTH) {
    const vec yr = load(&_yr[k]);
    const vec yi = load(&_yi[k]);
    const vec pr = load(&_pr[k]);
    const vec pi = load(&_pi[k]);

#if defined(__FMA__)
    const vec xr = _mm256_fmsub_ps(yr, pr, _mm256_mul_ps(yi, pi));
    const vec xi = _mm256_fmadd_ps(yr, pi, _mm256_mul_ps(yi, pr));
    const vec pw = _mm256_fmadd_ps(xr, xr, _mm256_mul_ps(xi, xi));
#else
    const vec xr =
        _mm256_sub_ps(_mm256_mul_ps(yr, pr), _mm256_mul_ps(yi, pi));
    const vec xi =
        _mm256_add_ps(_mm256_mul_ps(yr, pi), _mm256_mul_ps(yi, pr));
    const vec pw =
        _mm256_add_ps(_mm256_mul_ps(xr, xr), _mm256_mul_ps(xi, xi));
#endif

    store(&out[k], pw);
  }

  for (; k < _k; ++k) {
    const data_t xr = _yr[k] * _pr[k] - _yi[k] * _pi[k];
    const data_t xi = _yr[k] * _pi[k] + _yi[k] * _pr[k];
    out[k] = xr * xr + xi * xi;
  }
#elif defined(__ARM_NEON) && defined(SLIDINGDFT_USE_DOUBLE)
  std::size_t k = 0;
  for (; k + VEC_WIDTH <= _k; k += VEC_WIDTH) {
    const vec yr = load(&_yr[k]);
    const vec yi = load(&_yi[k]);
    const vec pr = load(&_pr[k]);
    const vec pi = load(&_pi[k]);

    const vec xr = vsubq_f64(vmulq_f64(yr, pr), vmulq_f64(yi, pi));
    const vec xi = vaddq_f64(vmulq_f64(yr, pi), vmulq_f64(yi, pr));
    const vec pw = vaddq_f64(vmulq_f64(xr, xr), vmulq_f64(xi, xi));

    store(&out[k], pw);
  }

  for (; k < _k; ++k) {
    const data_t xr = _yr[k] * _pr[k] - _yi[k] * _pi[k];
    const data_t xi = _yr[k] * _pi[k] + _yi[k] * _pr[k];
    out[k] = xr * xr + xi * xi;
  }
#elif defined(__ARM_NEON)
  std::size_t k = 0;
  for (; k + VEC_WIDTH <= _k; k += VEC_WIDTH) {
    const vec yr = load(&_yr[k]);
    const vec yi = load(&_yi[k]);
    const vec pr = load(&_pr[k]);
    const vec pi = load(&_pi[k]);

    const vec xr = vsubq_f32(vmulq_f32(yr, pr), vmulq_f32(yi, pi));
    const vec xi = vaddq_f32(vmulq_f32(yr, pi), vmulq_f32(yi, pr));
    const vec pw = vaddq_f32(vmulq_f32(xr, xr), vmulq_f32(xi, xi));

    store(&out[k], pw);
  }

  for (; k < _k; ++k) {
    const data_t xr = _yr[k] * _pr[k] - _yi[k] * _pi[k];
    const data_t xi = _yr[k] * _pi[k] + _yi[k] * _pr[k];
    out[k] = xr * xr + xi * xi;
  }
#else
  for (std::size_t k = 0; k < _k; ++k) {
    const data_t xr = _yr[k] * _pr[k] - _yi[k] * _pi[k];
    const data_t xi = _yr[k] * _pi[k] + _yi[k] * _pr[k];
    out[k] = xr * xr + xi * xi;
  }
#endif
}

void SlidingDFT::renormalize_phase() {
  for (std::size_t k = 0; k < _k; ++k) {
    const data_t magnitude = std::hypot(_pr[k], _pi[k]);
    if (magnitude > 0) {
      _pr[k] /= magnitude;
      _pi[k] /= magnitude;
    } else {
      _pr[k] = 1;
      _pi[k] = 0;
    }
  }
}

void SlidingDFT::recompute_fft_like() {
  vec_t xr(_k, 0);
  vec_t xi(_k, 0);

  for (std::size_t k = 0; k < _k; ++k) {
    data_t sum_real = 0;
    data_t sum_imag = 0;
    for (std::size_t n = 0; n < _n; ++n) {
      const std::size_t pos = (_index + n) % _n;
      const data_t sample = _buffer[pos];
      const data_t angle = static_cast<data_t>(
          -2.0 * std::numbers::pi_v<double> * static_cast<double>(k) *
          static_cast<double>(n) / static_cast<double>(_n));
      sum_real += sample * std::cos(angle);
      sum_imag += sample * std::sin(angle);
    }
    xr[k] = sum_real;
    xi[k] = sum_imag;
  }

  for (std::size_t k = 0; k < _k; ++k) {
    _yr[k] = xr[k] * _pr[k] + xi[k] * _pi[k];
    _yi[k] = -xr[k] * _pi[k] + xi[k] * _pr[k];
  }
}
