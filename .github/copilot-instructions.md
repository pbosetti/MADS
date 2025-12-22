# MADS Codebase AI Assistant Instructions

## Architecture Overview
MADS is a Multi-Agent Distributed System for industrial monitoring using ZeroMQ pub/sub messaging. Agents connect via a central broker that handles network discovery and settings distribution. Agents are either:
- **Monolithic**: Single executables inheriting from `Mads::Agent` (e.g., `logger`, `broker`)
- **Plugins**: Shared libraries loaded by generic executables (`mads-source`, `mads-filter`, `mads-sink`)

Data flows: Sources publish JSON messages → Filters process/transform → Sinks consume (log, visualize, bridge). Broker enables dynamic agent discovery.

## Key Patterns & Conventions

### Agent Implementation
- **Monolithic agents**: Create `src/new_agent.hpp` (inherit `Mads::Agent`, override `load_settings()`) and `src/main/new_agent.cpp`
- **Plugin agents**: Use [mads_plugin template](https://github.com/pbosetti/mads_plugin), derive from `SourcePlugin`/`FilterPlugin`/`SinkPlugin`
- Settings loaded from TOML sections matching executable/plugin name (e.g., `[logger]` for `mads-logger`)
- Main loop: `agent.connect_sub()` (sinks/filters) or `agent.connect_pub()` (sources), then `agent.receive()` and `agent.publish(json)`

### Coding Standards
- **C++17** with `CamelCase` classes/namespaces, `snake_case` variables/functions, `_leading_underscore` member vars
- **Header-only libs**: `cxxopts`, `toml++`, `nlohmann/json` via FetchContent
- **Static linking** preferred for external libs (ZeroMQ, MongoDB driver)
- **clang-format** with LLVM style for formatting

### Build System
- **Out-of-source builds**: `cmake -Bbuild -DCMAKE_BUILD_TYPE=Release`
- **Compile**: `cmake --build build -j8` (use clang if available)
- **Install**: `cmake --install build` (required on Windows for DLLs)
- **Options**: `-DMADS_ENABLE_LOGGER=OFF` to disable MongoDB-dependent targets
- **Universal binaries** on macOS: Add `-DCMAKE_OSX_ARCHITECTURES="x86_64;arm64"`

### Settings & Configuration
- **TOML format** in `mads.ini`, sections per agent (e.g., `[agents]` for shared settings)
- **Broker distributes settings** via REQ/REP on port 9092
- **Topics**: `pub_topic` for publishing, `sub_topic` array for subscribing (empty string subscribes to all)
- **Example**:
  ```toml
  [logger]
  mongo_uri = "mongodb://localhost:27017"
  sub_topic = [""]  # subscribe to all
  pub_topic = "logger"
  ```

### Development Workflows
- **Run broker first**: `./mads-broker mads.ini` (starts on ports 9090/9091/9092)
- **Agent execution**: `./mads-logger -s tcp://localhost:9092` (connects to broker for settings)
- **Plugin loading**: `./mads-source my_plugin.plugin -n custom_name` (loads plugin, uses `[custom_name]` settings section)
- **Testing**: Build with `-DMADS_BUILD_TESTS=ON`, run `curve_server`/`curve_client` for ZeroMQ auth tests
- **Docker**: `docker run --name mads-mongo -p27017:27017 -d mongo` for database

### External Dependencies
- **ZeroMQ** (libzmq + zmqpp wrapper) for messaging
- **MongoDB driver** (mongocxx) for logging agent
- **Snappy** for optional compression
- **pugg** for plugin loading (dlopen/dlsym abstraction)

### Common Pitfalls
- **Windows**: Always `cmake --install build` to copy DLLs to `products/bin/`
- **Settings mismatch**: Agent name must match INI section (e.g., `mads-logger` → `[logger]`)
- **Plugin naming**: Use `-n` flag for custom settings sections when loading same plugin multiple times
- **Broker timing**: Start broker before agents; uses timeouts for settings retrieval</content>
<parameter name="filePath">/Users/p4010/Develop/MADS_curve/.github/copilot-instructions.md