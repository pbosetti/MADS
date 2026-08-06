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
- `sub_topic` entries support MQTT-style wildcards (`Mads::topic_match()` /
  `Mads::literal_prefix()` in `src/topic_match.hpp`): `+` matches exactly one
  topic level and `#` matches this level and everything below it (including
  the level it replaces, e.g. `sensors/#` also matches the bare topic
  `sensors`), and is only legal as the final token. A plain entry with
  neither character keeps subscribing exactly as before (unchanged ZMQ
  `SUBSCRIBE` frame); only entries containing `+`/`#` pay for the extra
  in-process match, applied after the (broader) ZMQ-level subscribe and
  before the message reaches `receive()`/callbacks.
- Watch out: a plain entry is matched by ZeroMQ's raw **byte prefix** rule,
  not by equality or by topic level -- `sub_topic = ["sensors"]` also
  receives `sensors/imu/raw` (and `sensors_2`). `Mads::subscription_match()`
  (same header) answers "would this entry receive this topic?" for both entry
  kinds at once and reports how it matched (exact / prefix / wildcard); it is
  what `Agent` and `mads doctor --graph` both use, so the topology graph is
  never a different rule from the wire.

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
- Current top-level CMake sets **C++20** (`CMAKE_CXX_STANDARD 20`); some docs/instructions still mention C++17, so align new code with the active build configuration.
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
- `README.md`
- `mads.ini`
- `src/agent.hpp`
- `src/main/plugin_loader.cpp`
- `COMPILE.md`
- `.github/copilot-instructions.md`
