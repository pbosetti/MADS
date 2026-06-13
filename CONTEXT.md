# MADS Framework Context for Coding Agents

## What MADS is
- **MADS** is a distributed, message-driven framework for industrial monitoring/control.
- Core communication model is **ZeroMQ pub/sub** with a central **broker**.
- Data payloads are typically **JSON** (`nlohmann::json`).

## Mental model of runtime architecture
- **Broker** (`mads-broker`) is the network hub:
  - routes publisher/subscriber traffic
  - serves settings to agents over a settings endpoint (REQ/REP)
- Agent roles:
  - **Source**: produces data and publishes
  - **Filter**: subscribes, transforms, republishes
  - **Sink**: subscribes and consumes locally (log/UI/bridge/etc.)
- Agents can be:
  - **Monolithic executables** (purpose-built C++ apps)
  - **Plugin-based** (generic loaders + runtime-loaded `.plugin` libraries)

## Main executables to know
- Monolithic/common apps in `src/main/` include: `broker`, `logger`, `bridge`, `dealer`, `worker`, `feedback`, etc.
- Generic plugin loaders are built from `src/main/plugin_loader.cpp` as:
  - `mads-source`
  - `mads-filter`
  - `mads-sink`

## Settings model (critical)
- Settings are TOML/INI (`mads.ini`).
- Shared settings go under `[agents]`.
- Each agent/plugin instance reads from a section named after its **runtime agent name**:
  - `mads-logger` → `[logger]`
  - `mads-source my.plugin` → `[my]` by default (plugin stem)
  - `mads-source my.plugin -n custom_name` → `[custom_name]`
- Frequent keys:
  - `pub_topic` (single topic string)
  - `sub_topic` (array of topics; `[""]` means subscribe to all)

## Custom monolithic agent workflow
1. Add agent class in `src/<agent>.hpp` deriving from `Mads::Agent`.
2. Override `load_settings()` for agent-specific config.
3. Add executable entrypoint in `src/main/<agent>.cpp`.
4. Typical loop:
   - initialize and connect (`connect_pub()` and/or `connect_sub()`)
   - `receive()` when consuming
   - process JSON/state
   - `publish(...)` when producing

## Custom plugin workflow
- Preferred approach: scaffold from `mads_plugin` template project.
- Plugin categories map to loader type:
  - Source plugin for `mads-source`
  - Filter plugin for `mads-filter`
  - Sink plugin for `mads-sink`
- Keep plugin logic focused on transformation/IO behavior; MADS runtime concerns are handled by the loader.
- Plugin settings are passed from the selected TOML section into plugin runtime parameters.

## Build and toolchain facts
- CMake-based, out-of-source builds.
- Typical local build:
  - `cmake -Bbuild -DCMAKE_BUILD_TYPE=Release -GNinja`
  - `cmake --build build -j6`
- Current top-level CMake enforces **C++20** (`CMAKE_CXX_STANDARD 20`), even though some docs mention C++17.
- Formatting convention is LLVM style (`clang-format` guidance in docs/instructions).

## Key dependencies and integration points
- Messaging: `libzmq` + `zmqpp`
- Config parsing: `toml++`
- JSON: `nlohmann/json`
- Plugin loading: `pugg`
- Optional logging backend: MongoDB C++ driver (`mongocxx`/`bsoncxx`)

## Practical coding guidance for LLM agents
- Preserve role semantics (source/filter/sink) and pub/sub directionality.
- Do not hardcode settings that belong in TOML sections.
- Respect section-name resolution rules (`-n` changes the section).
- Keep new agent/plugin code minimal and cohesive; avoid cross-cutting refactors.
- Follow existing naming/style in repository:
  - classes/namespaces: `CamelCase`
  - functions/variables: `snake_case`
  - member fields: leading underscore
- Prefer extending existing abstractions (`Mads::Agent`, plugin drivers) over re-implementing transport/config plumbing.

## Common pitfalls
- Running agents before broker/settings endpoint is available.
- Mismatch between runtime agent name and TOML section name.
- Confusion between `pub_topic` (string) and `sub_topic` (array).
- On Windows, skipping install step can leave required DLLs unavailable at runtime.

## Useful reference files in this repo
- `/home/runner/work/MADS/MADS/pbosetti/MADS/README.md`
- `/home/runner/work/MADS/MADS/pbosetti/MADS/mads.ini`
- `/home/runner/work/MADS/MADS/pbosetti/MADS/src/agent.hpp`
- `/home/runner/work/MADS/MADS/pbosetti/MADS/src/main/plugin_loader.cpp`
- `/home/runner/work/MADS/MADS/pbosetti/MADS/COMPILE.md`
- `/home/runner/work/MADS/MADS/pbosetti/MADS/.github/copilot-instructions.md`
