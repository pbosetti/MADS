#include "goertzel/goertzel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <random>
#include <vector>

int main() {
  constexpr double sample_rate_hz = 8000.0;
  constexpr std::size_t sample_count = 400U;
  constexpr double low_frequency_hz = 770.0;
  constexpr double high_frequency_hz = 1477.0;

  const std::array<double, 4> low_group = {697.0, 770.0, 852.0, 941.0};
  const std::array<double, 4> high_group = {1209.0, 1336.0, 1477.0, 1633.0};

  std::vector<double> signal(sample_count, 0.0);
  std::mt19937 generator(6U);
  std::normal_distribution<double> noise(0.0, 0.30);

  for (std::size_t i = 0; i < sample_count; ++i) {
    const double time = static_cast<double>(i) / sample_rate_hz;
    signal[i] = 0.9 * std::sin(2.0 * std::acos(-1.0) * low_frequency_hz * time) +
                0.9 * std::sin(2.0 * std::acos(-1.0) * high_frequency_hz * time) + noise(generator);
  }

  std::vector<double> alpha;
  alpha.reserve(low_group.size() + high_group.size());
  for (const double frequency : low_group) {
    alpha.push_back(Goertzel::Analyzer::alpha_from_frequency(frequency, sample_rate_hz));
  }
  for (const double frequency : high_group) {
    alpha.push_back(Goertzel::Analyzer::alpha_from_frequency(frequency, sample_rate_hz));
  }

  Goertzel::Analyzer analyzer(alpha);
  analyzer.process(signal);

  std::array<double, 4> low_power{};
  std::array<double, 4> high_power{};
  for (std::size_t i = 0; i < low_group.size(); ++i) {
    low_power[i] = analyzer.power(i);
    high_power[i] = analyzer.power(i + low_group.size());
  }

  const auto low_it = std::max_element(low_power.begin(), low_power.end());
  const auto high_it = std::max_element(high_power.begin(), high_power.end());
  const std::size_t low_index = static_cast<std::size_t>(std::distance(low_power.begin(), low_it));
  const std::size_t high_index = static_cast<std::size_t>(std::distance(high_power.begin(), high_it));

  if (low_index != 1U || high_index != 2U) {
    std::cerr << "Expected DTMF key 6 frequencies at indexes (1, 2), got (" << low_index << ", "
              << high_index << ")\n";
    return 1;
  }

  return 0;
}
