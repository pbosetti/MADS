#include "goertzel/goertzel_c.h"

#include "goertzel/goertzel.hpp"

#include <algorithm>
#include <exception>
#include <vector>

struct goertzel_analyzer_t {
  explicit goertzel_analyzer_t(std::vector<double> alpha) : analyzer(std::move(alpha)) {}

  Goertzel::Analyzer analyzer;
};

goertzel_analyzer_t *goertzel_analyzer_create(const double *alpha, size_t alpha_count) {
  if (alpha == nullptr && alpha_count != 0U) {
    return nullptr;
  }

  try {
    std::vector<double> coefficients;
    if (alpha_count != 0U) {
      coefficients.assign(alpha, alpha + alpha_count);
    }
    return new goertzel_analyzer_t(std::move(coefficients));
  } catch (const std::exception &) {
    return nullptr;
  }
}

void goertzel_analyzer_destroy(goertzel_analyzer_t *analyzer) {
  delete analyzer;
}

size_t goertzel_analyzer_size(const goertzel_analyzer_t *analyzer) {
  return analyzer == nullptr ? 0U : analyzer->analyzer.size();
}

void goertzel_analyzer_reset(goertzel_analyzer_t *analyzer) {
  if (analyzer != nullptr) {
    analyzer->analyzer.reset();
  }
}

int goertzel_analyzer_process_buffer(goertzel_analyzer_t *analyzer,
                                     const double *samples,
                                     size_t sample_count) {
  if (analyzer == nullptr || (samples == nullptr && sample_count != 0U)) {
    return -1;
  }

  if (sample_count != 0U) {
    analyzer->analyzer.process(samples, samples + sample_count);
  }
  return 0;
}

int goertzel_analyzer_state_vector(const goertzel_analyzer_t *analyzer,
                                   double *states_out,
                                   size_t state_count) {
  if (analyzer == nullptr || states_out == nullptr) {
    return -1;
  }

  const auto states = analyzer->analyzer.state_vector();
  if (state_count < states.size()) {
    return -2;
  }

  std::copy(states.begin(), states.end(), states_out);
  return 0;
}

int goertzel_analyzer_dft_terms(const goertzel_analyzer_t *analyzer,
                                const double *omega,
                                size_t omega_count,
                                double *real_out,
                                double *imag_out) {
  if (analyzer == nullptr || omega == nullptr || real_out == nullptr || imag_out == nullptr) {
    return -1;
  }

  if (omega_count != analyzer->analyzer.size()) {
    return -2;
  }

  try {
    for (size_t i = 0; i < omega_count; ++i) {
      const auto value = analyzer->analyzer.dft_term_from_omega(i, omega[i]);
      real_out[i] = value.real();
      imag_out[i] = value.imag();
    }
  } catch (const std::exception &) {
    return -3;
  }

  return 0;
}

double goertzel_alpha_from_bin(double bin, size_t sample_count) {
  try {
    return Goertzel::Analyzer::alpha_from_bin(bin, sample_count);
  } catch (const std::exception &) {
    return 0.0;
  }
}

double goertzel_omega_from_bin(double bin, size_t sample_count) {
  try {
    return Goertzel::Analyzer::omega_from_bin(bin, sample_count);
  } catch (const std::exception &) {
    return 0.0;
  }
}

double goertzel_alpha_from_frequency(double frequency_hz, double sample_rate_hz) {
  try {
    return Goertzel::Analyzer::alpha_from_frequency(frequency_hz, sample_rate_hz);
  } catch (const std::exception &) {
    return 0.0;
  }
}

double goertzel_omega_from_frequency(double frequency_hz, double sample_rate_hz) {
  try {
    return Goertzel::Analyzer::omega_from_frequency(frequency_hz, sample_rate_hz);
  } catch (const std::exception &) {
    return 0.0;
  }
}
