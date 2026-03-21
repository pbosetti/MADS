#ifndef GOERTZEL_GOERTZEL_HPP
#define GOERTZEL_GOERTZEL_HPP

#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <initializer_list>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Goertzel {

class Analyzer {
public:
  Analyzer() = default;

  explicit Analyzer(std::vector<double> alpha) {
    set_alpha(std::move(alpha));
  }

  Analyzer(std::initializer_list<double> alpha) : Analyzer(std::vector<double>(alpha)) {}

  void set_alpha(std::vector<double> alpha) {
    _alpha = std::move(alpha);
    _state_1.assign(_alpha.size(), 0.0);
    _state_2.assign(_alpha.size(), 0.0);
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return _alpha.size();
  }

  void reset() {
    std::fill(_state_1.begin(), _state_1.end(), 0.0);
    std::fill(_state_2.begin(), _state_2.end(), 0.0);
  }

  void process_sample(double sample) {
    for (std::size_t i = 0; i < _alpha.size(); ++i) {
      const double next = sample + _alpha[i] * _state_1[i] - _state_2[i];
      _state_2[i] = _state_1[i];
      _state_1[i] = next;
    }
  }

  template <typename Iterator>
  void process(Iterator begin, Iterator end) {
    for (Iterator it = begin; it != end; ++it) {
      process_sample(static_cast<double>(*it));
    }
  }

  template <typename Container>
  void process(const Container &samples) {
    process(samples.begin(), samples.end());
  }

  [[nodiscard]] const std::vector<double> &alpha() const noexcept {
    return _alpha;
  }

  [[nodiscard]] std::array<double, 2> state_pair(std::size_t index) const {
    validate_index(index);
    return {_state_2[index], _state_1[index]};
  }

  [[nodiscard]] std::vector<double> state_vector() const {
    std::vector<double> states;
    states.reserve(_alpha.size() * 2U);
    for (std::size_t i = 0; i < _alpha.size(); ++i) {
      states.push_back(_state_2[i]);
      states.push_back(_state_1[i]);
    }
    return states;
  }

  [[nodiscard]] std::complex<double> dft_term_from_omega(std::size_t index, double omega) const {
    validate_index(index);
    const std::complex<double> w(std::cos(omega), std::sin(omega));
    return w * _state_1[index] - _state_2[index];
  }

  [[nodiscard]] std::complex<double> dft_term_from_bin(std::size_t index,
                                                       double bin,
                                                       std::size_t sample_count) const {
    return dft_term_from_omega(index, omega_from_bin(bin, sample_count));
  }

  [[nodiscard]] double power(std::size_t index) const {
    validate_index(index);
    return _state_1[index] * _state_1[index] + _state_2[index] * _state_2[index] -
           _alpha[index] * _state_1[index] * _state_2[index];
  }

  static double alpha_from_omega(double omega) {
    return 2.0 * std::cos(omega);
  }

  static double omega_from_bin(double bin, std::size_t sample_count) {
    if (sample_count == 0U) {
      throw std::invalid_argument("sample_count must be greater than zero");
    }
    return (2.0 * pi() * bin) / static_cast<double>(sample_count);
  }

  static double alpha_from_bin(double bin, std::size_t sample_count) {
    return alpha_from_omega(omega_from_bin(bin, sample_count));
  }

  static double omega_from_frequency(double frequency_hz, double sample_rate_hz) {
    if (sample_rate_hz <= 0.0) {
      throw std::invalid_argument("sample_rate_hz must be greater than zero");
    }
    return (2.0 * pi() * frequency_hz) / sample_rate_hz;
  }

  static double alpha_from_frequency(double frequency_hz, double sample_rate_hz) {
    return alpha_from_omega(omega_from_frequency(frequency_hz, sample_rate_hz));
  }

private:
  static constexpr double pi() noexcept {
    return 3.141592653589793238462643383279502884;
  }

  void validate_index(std::size_t index) const {
    if (index >= _alpha.size()) {
      throw std::out_of_range("Goertzel analyzer index out of range");
    }
  }

  std::vector<double> _alpha;
  std::vector<double> _state_1;
  std::vector<double> _state_2;
};

}  // namespace Goertzel

#endif  // GOERTZEL_GOERTZEL_HPP
