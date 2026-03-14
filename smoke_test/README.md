# MADS Smoke Test Suite

Standalone CMake project that validates an installed MADS distribution. It checks executables, plugins, the plugin stub generator, C++ and C library linkage, ZeroMQ messaging patterns, plugin runtime loading, and the Python wrapper.

## Prerequisites

- **MADS installed** and available in PATH (i.e., `mads -p` returns the install prefix)
- **CMake** ≥ 3.22
- **Python 3** (for the test runner and Python wrapper tests)
- **A C++20 / C11 compiler** (same toolchain used to build MADS)

## Quick Start

From the `smoke_test/` directory:

```bash
# Configure (auto-detects MADS install prefix)
cmake -B build -DCMAKE_PREFIX_PATH=$(mads -p)

# Build the compiled test targets
cmake --build build

# Run all tests that don't need a broker
python3 run_smoke_tests.py --no-broker

# Run broker-dependent tests (starts/stops broker automatically)
python3 run_smoke_tests.py --label broker_required
```

Or use the all-in-one pipeline script:

```bash
python3 setup_and_run.py --skip-install
```

## Directory Structure

```
smoke_test/
├── CMakeLists.txt          # Test project definition (32 tests in 6 phases)
├── mads_smoke.ini          # TOML config using non-standard ports (19090-19093)
├── run_smoke_tests.py      # Test runner (broker lifecycle + CTest invocation)
├── setup_and_run.py        # Full pipeline: install → configure → build → test
├── scripts/
│   ├── run_plugin_test.py      # Helper: starts a plugin, waits 3s, sends SIGTERM
│   └── test_python_agent.py    # Python ctypes wrapper test
└── src/
    ├── test_cpp_agent.cpp      # C++ Agent API: create, init, connect, publish
    ├── test_c_agent.c          # C wrapper (agent_c.h) API test
    └── test_messaging.cpp      # 5 messaging patterns: nonblocking, blocking,
                                #   LKV, queue sizes, receive timeout
```

## Test Phases

| # | Phase | Tests | Broker | Description |
|---|-------|------:|--------|-------------|
| 1 | Executable help | 10 | No | Runs `--help` on every MADS executable |
| 2 | Plugin existence | 4 | No | Checks that default `.plugin` files are installed |
| 3 | Plugin stub generation | 4 | No | Generates a plugin project, configures and builds it |
| 4 | Compile & link | 3 | No | Builds C++, C, and messaging test binaries against `libMadsCore` |
| 5 | Runtime | 8 | **Yes** | Runs C++/C agents, 5 messaging tests, 3 plugin load tests |
| 6 | Python wrapper | 1 | **Yes** | Tests `mads_agent.py` ctypes interface |

**Total: 32 tests** (21 no-broker + 11 broker-required)

## Scripts

### `run_smoke_tests.py`

Main test runner. Optionally starts a broker, invokes CTest, then cleans up.

```
usage: run_smoke_tests.py [-h] [--mads-prefix PATH] [--config PATH]
                          [--build-dir PATH] [--label LABEL]
                          [--no-broker] [--verbose]
```

| Option | Default | Description |
|--------|---------|-------------|
| `--mads-prefix PATH` | Auto-detect from `CMakeCache.txt` or `mads -p` | Path to MADS install prefix |
| `--config PATH` | `mads_smoke.ini` | Path to the smoke test TOML config file |
| `--build-dir PATH` | `build` | CTest build directory |
| `--label LABEL` | *(all tests)* | Run only tests matching this CTest label (e.g. `broker_required`, `no_broker`, `messaging`, `plugin`, `python`) |
| `--no-broker` | off | Skip broker startup; automatically filters to `no_broker` label |
| `--verbose` | off | Pass `-V` to CTest for verbose output |

**Examples:**

```bash
# Run everything (starts broker, runs all 32 tests)
python3 run_smoke_tests.py

# Run only no-broker tests (fast, no network)
python3 run_smoke_tests.py --no-broker

# Run only messaging tests (broker auto-started)
python3 run_smoke_tests.py --label messaging

# Run only Python test with verbose output
python3 run_smoke_tests.py --label python --verbose

# Point to a custom install prefix
python3 run_smoke_tests.py --mads-prefix /opt/mads
```

### `setup_and_run.py`

All-in-one pipeline: installs MADS, configures/builds the smoke tests, and runs them.

```
usage: setup_and_run.py [-h] [--mads-build-dir PATH] [--prefix PATH]
                        [--build-dir PATH] [--skip-install] [--skip-build]
                        [--label LABEL] [--no-broker]
```

| Option | Default | Description |
|--------|---------|-------------|
| `--mads-build-dir PATH` | `../build` | MADS build directory (for the install step) |
| `--prefix PATH` | Auto-detect via `mads -p` | MADS install prefix |
| `--build-dir PATH` | `build` | Smoke test build directory |
| `--skip-install` | off | Skip `cmake --install` (use existing installation) |
| `--skip-build` | off | Skip configure + build steps (use existing build) |
| `--label LABEL` | *(all tests)* | Only run tests matching this CTest label |
| `--no-broker` | off | Skip broker, run only `no_broker` tests |

**Examples:**

```bash
# Full pipeline from scratch
python3 setup_and_run.py

# Already installed MADS, just configure+build+test
python3 setup_and_run.py --skip-install

# Already built, just run tests
python3 setup_and_run.py --skip-install --skip-build

# Custom MADS build directory and prefix
python3 setup_and_run.py --mads-build-dir ~/mads/build --prefix ~/usr/local
```

## CTest Labels

Tests are tagged with labels for selective execution:

| Label | Description |
|-------|-------------|
| `no_broker` | Tests that run without a broker (phases 1–4) |
| `broker_required` | Tests that need a running broker (phases 5–6) |
| `executable` | `--help` tests for all executables |
| `plugin` | Plugin existence and runtime loading tests |
| `plugin_gen` | Plugin stub generation, configure, and build |
| `compile` | Compile & link tests |
| `runtime` | Runtime execution tests (C++, C, messaging, plugins) |
| `messaging` | ZeroMQ messaging pattern tests |
| `python` | Python wrapper tests |

Run tests by label directly with CTest:

```bash
ctest --test-dir build -L messaging --output-on-failure
```

## Configuration

The smoke tests use `mads_smoke.ini` with **non-standard ports** to avoid conflicts with a running MADS instance:

| Service | Port |
|---------|------|
| XSUB frontend | 19090 |
| XPUB backend | 19091 |
| Settings REP | 19092 |
| Dealer | 19093 |

Edit `mads_smoke.ini` to change ports or agent-specific settings.

## Troubleshooting

- **`find_package(Mads)` fails**: Ensure `-DCMAKE_PREFIX_PATH=$(mads -p)` is passed at configure time.
- **Plugin stub configure is slow**: The first run downloads dependencies (pugg, nlohmann/json, mads_plugin) via FetchContent. Subsequent runs use the CMake cache.
- **Broker tests time out**: Make sure ports 19090–19093 are free. Check for stale broker processes with `pgrep -f mads-broker`.
- **Python test fails with "library not found"**: Verify that `libMadsCore` is installed in `$(mads -p)/lib/`.
