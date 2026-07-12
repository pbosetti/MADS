# MADS unit tests

Unit tests use [Catch2 v3](https://github.com/catchorg/Catch2) and run under
CTest. They are self-contained: no broker, no MongoDB, no internet access.
Tests that exercise ZeroMQ messaging do so entirely in-process, over
`tcp://127.0.0.1` loopback sockets.

## Building and running

```sh
CC=clang CXX=clang++ cmake -B build-cov -GNinja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DMADS_BUILD_TESTS=ON -DMADS_COVERAGE=ON \
  -DMADS_BUILD_APPS=OFF -DMADS_DIRECTOR=OFF -DMADS_ENABLE_MONGOCXX=OFF
cmake --build build-cov -j
ctest --test-dir build-cov --output-on-failure
```

With `MADS_COVERAGE=ON` and `gcovr` installed, a combined test + report run is
available as:

```sh
cmake --build build-cov --target coverage
# report: build-cov/coverage_html/index.html and ./coverage.xml
```

The coverage denominator is curated in `gcovr.cfg` (mirrored by `codecov.yml`):
vendored code, network/DB/hardware-bound units, executable glue, and runtime
plugins are excluded. Integration-level behavior is covered separately by
`smoke_test/`.

## Adding tests

Drop a `test_<unit>.cpp` file in this directory — the build globs
`test_*.cpp` and creates one executable per file; no CMake edits are needed.
Fixture data goes in `tests/fixtures/` and is reachable at compile time via
the `MADS_TEST_FIXTURES_DIR` macro (`MADS_PROJECT_SOURCE_DIR` points at the
repository root). Shared conventions (loopback helper, port ranges per suite,
`RunningGuard` for `Agent::loop()` tests, `wait_for` polling) live in
`mads_test_helpers.hpp` — read it before writing a new suite.
