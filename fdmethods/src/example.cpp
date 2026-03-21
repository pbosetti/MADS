#include <SlidingDFT/sliding_dft.hpp>
#include <iostream>

int main() {
  constexpr std::size_t N = 256;
  SlidingDFT sdft(N, 4096);

  for (std::size_t n = 0; n < 5000; ++n) {
    data_t x = static_cast<data_t>(
        std::sin(2.0 * std::numbers::pi_v<double> * 17.0 *
                 static_cast<double>(n) / static_cast<double>(N)));
    sdft.update(x);
  }

  SlidingDFT::vec_t power;
  sdft.get_power(power);

  for (std::size_t k = 0; k < 32; ++k) {
    std::cout << k << "  " << power[k] << "\n";
  }
}
