# fdmethods

Frequency Domain Methods: a combined library providing the
[Goertzel algorithm](https://github.com/pbosetti/goertzel) and
[SlidingDFT](https://github.com/pbosetti/SlidingDFT) in a single package.

Both methods share a common CMake build system and install under consistent
include paths. The Goertzel algorithm also ships with R and Python interfaces;
SlidingDFT provides a C API and a Python ctypes wrapper.

## Contents

| Component | Description |
|-----------|-------------|
| `include/SlidingDFT/` | C++20 Sliding DFT headers |
| `include/goertzel/` | C++20 Goertzel analyzer headers |
| `src/sliding_dft.cpp` | SlidingDFT implementation (SIMD-accelerated) |
| `src/c_api.cpp` | SlidingDFT C shared-library wrapper |
| `src/goertzel_c.cpp` | Goertzel C static-library wrapper |
| `python/goertzel_module.cpp` | Python C extension for Goertzel |
| `python/sliding_dft/` | Python ctypes wrapper for SlidingDFT |
| `R/goertzel.R` | R interface (Goertzel) |
| `tests/dtmf_test.cpp` | Goertzel DTMF regression test |

## Build

Configure and build:

```sh
cmake -B build
cmake --build build
```

Run examples and tests:

```sh
./build/sliding_dft_example
cmake -B build -DFDMETHODS_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Enable double precision for SlidingDFT:

```sh
cmake -B build -DFDMETHODS_USE_DOUBLE=ON
cmake --build build
```

Build Python extension modules:

```sh
cmake -B build -DFDMETHODS_BUILD_PYTHON=ON
cmake --build build
```

## Install

```sh
cmake -B build
cmake --build build
cmake --install build --prefix /path/to/prefix
```

Installed artifacts:

- `lib/libSlidingDFT.a`
- `lib/libSlidingDFTC.so` (or `.dylib` / `.dll`)
- `lib/libgoertzel_c.a`
- `include/SlidingDFT/*.hpp`
- `include/goertzel/*.hpp`, `include/goertzel/*.h`
- `lib/cmake/fdmethods/`

## Use With CMake

After installation:

```cmake
find_package(fdmethods CONFIG REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE fdmethods::SlidingDFT fdmethods::goertzel)
```

Or with FetchContent:

```cmake
include(FetchContent)

FetchContent_Declare(
  fdmethods
  GIT_REPOSITORY https://github.com/pbosetti/fdmethods.git
  GIT_TAG        main
)
FetchContent_MakeAvailable(fdmethods)

target_link_libraries(my_app PRIVATE fdmethods::SlidingDFT fdmethods::goertzel)
```

## C++ Usage

### SlidingDFT

```cpp
#include <SlidingDFT/sliding_dft.hpp>

int main() {
  SlidingDFT sdft(256);
  sdft.update(1.0f);

  SlidingDFT::vec_t power;
  sdft.get_power(power);
  return 0;
}
```

### Goertzel

```cpp
#include "goertzel/goertzel.hpp"

#include <vector>

int main() {
  std::vector<double> signal = {1.0, 0.0, -1.0, 0.0};
  Goertzel::Analyzer analyzer({Goertzel::Analyzer::alpha_from_bin(1.0, signal.size())});
  analyzer.process(signal);
  const auto value = analyzer.dft_term_from_bin(0, 1.0, signal.size());
  return value.real() != 0.0;
}
```

## C Usage

### SlidingDFT

```c
#include "SlidingDFT/c_api.h"

int main(void) {
  SlidingDFTHandle *h = sliding_dft_create(256, 4096);
  sliding_dft_update(h, 1.0f);
  sliding_dft_destroy(h);
  return 0;
}
```

### Goertzel

```c
#include "goertzel/goertzel_c.h"

int main(void) {
  double alpha[1] = {goertzel_alpha_from_bin(6.0, 128)};
  goertzel_analyzer_t *analyzer = goertzel_analyzer_create(alpha, 1);
  double samples[128] = {0.0};
  goertzel_analyzer_process_buffer(analyzer, samples, 128);
  goertzel_analyzer_destroy(analyzer);
  return 0;
}
```

## Python Usage

### SlidingDFT

```sh
PYTHONPATH=python python3
```

```python
from math import pi, sin
from sliding_dft import SlidingDFT

sdft = SlidingDFT(256)
for n in range(5000):
    sample = sin(2.0 * pi * 17.0 * n / 256)
    sdft.update(sample)

power = sdft.get_power()
print(power[:8])
```

### Goertzel

Build with `-DFDMETHODS_BUILD_PYTHON=ON`, then:

```python
import goertzel

samples = [0.0, 1.0, 0.0, -1.0] * 32
values = goertzel.goertzel_frequencies(samples, [770.0, 1477.0], 8000.0)
print(values)
```

## R Usage

Install the R package from a checkout:

```r
install.packages(".", repos = NULL, type = "source")
```

Or from GitHub:

```r
devtools::install_github("pbosetti/fdmethods")
```

Example:

```r
library(fdmethods)

signal <- sin(2 * pi * 6 * (0:127) / 128)
goertzel(signal, 6)
```
