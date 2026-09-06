---
name: mads-plugin
description: Write, build, test and debug MADS plugins (source, filter or sink) in C++ or Rust. Covers the plugin lifecycle, the exact order in which a host agent calls each plugin method, the effect of every return_type in every host, how settings reach the plugin, topic and binary-blob handling, deployment and protocol migration. Use whenever working inside a MADS plugin project.
---

# MADS plugins

A MADS plugin is a **shared library** that a generic *host agent* loads at
runtime. The host owns everything an agent needs — CLI, INI settings, broker
connection, ZMQ sockets, topics, timing, events, remote control — and calls
into your class for the data work only.

Consequences worth internalising before writing code:

- **You never link against MADS.** A plugin project needs only `pugg`,
  `nlohmann/json` and the `mads_plugin` headers, all fetched by the generated
  `CMakeLists.txt`. `mads -p` is used to locate the *install prefix*, not headers.
- **You cannot see the host loop from inside the plugin.** Every method you
  write is a callback. What happens to your output — whether it is published at
  all, on which topic, with which extra fields — is decided by the host and is
  documented in `reference/return-types.md`. Do not guess it.
- **The plugin never drives the loop.** Sleeping, retrying and polling inside a
  method blocks the whole agent. Use `next_loop_duration` / `return_type::retry`.

This skill documents MADS **{{mads_version}}**, plugin protocol
**P{{plugin_protocol}}** (hosts accept P{{plugin_min_protocol}} and newer).

## Non-negotiables

These are the failures that actually happen. Check every one before saying a
plugin is done.

1. **`kind()` must return `PLUGIN_NAME`.** The host looks the driver up *by
   name*; a mismatch is either "cannot find plugin driver" at load time or a
   warning plus the wrong settings section. `PLUGIN_NAME` is defined by the
   `add_plugin()` CMake macro from the target name (with `_${PLUGIN_SUFFIX}`
   appended when a suffix is set), so never hardcode a different literal.
2. **A behavior is rigid.** A source plugin cannot be loaded by `mads filter`.
   Pick the behavior first; changing it later means a new class and a new host.
3. **All setup goes in `set_params()`, not the constructor.** The host
   constructs the object before it has settings; `set_params()` is called once,
   after the agent is connected and before the first loop iteration.
4. **Never let an exception escape a plugin method.** Catch, store the message
   in `_error`, and return `return_type::error` or `critical`.
5. **Never block.** No `sleep_for`, no unbounded read, no busy wait.
6. **A filter must not publish to a topic it subscribes to.** The host drops
   such frames with a warning to break the loop, so the plugin looks dead.
7. **`out` is cleared by the host each tick, but the plugin must fill it
   completely.** Publishing an empty `out` gets you an auto-inserted warning
   field instead of data.

## Lifecycle

Identical for source, filter and sink; only the loop body differs.

```
mads <source|filter|sink> <name.plugin> [-n agent_name] [-o key=value]
  │
  ├─ resolve the plugin file:  --plugin > broker-served attachment (OTA) > default
  │                            not found? retry under <prefix>/lib (Unix) or <prefix>/bin (Windows)
  ├─ fetch settings for the agent name (local mads.ini or from the broker)
  ├─ dlopen  ──▶  register_pugg_plugin()  ──▶  MADS_REGISTER_PLUGINS(...) installs the driver(s)
  ├─ pick the driver by name:  --driver > the 'driver' setting > the file stem
  ├─ reject the plugin if its protocol is older than P{{plugin_min_protocol}}
  ├─ warn if kind() differs from the driver name it was loaded under
  ├─ driver->create()                          ← your constructor runs here, with no settings yet
  ├─ agent.connect()
  ├─ plugin->set_params(settings)              ← ONCE. all acquisition setup belongs here
  ├─ plugin->info()                            ← ONCE, printed to the console
  ├─ plugin->blob_format()                     ← ONCE, sources only
  ├─ optional --delay
  ├─ startup event
  │
  ├─ loop  ─────────────────────────────────┐
  │    source: get_output(out, blob)        │  see reference/lifecycle.md for the
  │    filter: load_data(...) then process()│  per-tick flow of each host, and
  │    sink:   load_data(...)               │  reference/return-types.md for what
  │                                         │  each return value does
  └────────────────────────────────────────┘
       ▼ SIGINT or a critical return
  shutdown event ─▶ disconnect ─▶ destructor ─▶ (restart if requested)
```

## The three behaviors

| Behavior | Host agent | Methods you implement | Publishes? |
|---|---|---|---|
| `Source` | `mads source` | `kind`, `get_output`, `info`, (`set_params`) | yes, one frame per tick |
| `Filter` | `mads filter`, `mads worker` | `kind`, `load_data`, `process`, `info`, (`set_params`) | yes, one frame per input |
| `Sink`   | `mads sink`   | `kind`, `load_data`, `info`, (`set_params`) | never |

`kind()`, `info()` and the data methods are pure virtual: forgetting one is a
compile error. `set_params()` is virtual with a default — override it and call
the parent version first, or `agent_id` is lost.

Rust plugins use the same three behaviors and the same hosts under different
names (`mads rsource` / `rfilter` / `rsink`); see `reference/rust.md`.

## Return values, in one table

`success` is the only value that means "publish what I produced". Everything
else is documented per host in `reference/return-types.md` — read it, the
differences between hosts are real and silent.

| Value | Intent |
|---|---|
| `success` | Output is valid; publish it. |
| `retry` | Nothing to say this tick (no complete frame yet, waiting for more input). Nothing is published, no error is recorded. **This is the normal idle path — not an error.** |
| `warning` | Output is valid but degraded; it is published *with* your `_error` text attached under a `warning` key. |
| `error` | This tick failed. Nothing goes out on the data topic; the message is emitted as an agent event on the `agent_event` topic. The agent keeps running. |
| `critical` | Unrecoverable. The agent stops. Use only when continuing is meaningless (hardware gone, config invalid). |

Always set `_error` to a human-readable message before returning `warning`,
`error` or `critical`. The host reads it via `error()` and it is the only
diagnostic the operator gets.

## Settings

The INI section named after the **agent** (not the plugin file) is handed to
`set_params()` as a JSON object, plus keys the host injects. The idiom is:

```cpp
void set_params(const json &params) override {
  Source::set_params(params);          // keeps _agent_id
  _params["some_field"] = "default";   // your defaults first
  _params.merge_patch(params);         // caller wins
}
```

Some keys are consumed by the *agent* before the plugin sees them (`period`,
`sub_topic`, `pub_topic`, `receive_timeout`, `wire_format`, `compression`,
`queue_size`, `dont_block`, `driver`, `attachment`); do not repurpose those
names. Full list, injected keys and the `-o key=value` override path:
`reference/settings.md`.

## Build, test, verify

The generated project builds both a `.plugin` shared library and a standalone
executable from the *same* source file — the `main()` at the bottom is compiled
only in the executable and is where plugin logic gets unit tested without a
broker, a network or MADS running.

```bash
cmake -Bbuild -DCMAKE_INSTALL_PREFIX="$(mads -p)"   # configure
cmake --build build -j4                             # build .plugin + test executable
./build/<name>                                      # run the test main()
mads inspect_plugin build/<name>.plugin             # behavior, driver names, protocol, json ABI
sudo cmake --install build                          # install into the MADS prefix
mads <source|filter|sink> <name>.plugin             # run it for real
```

Before declaring work complete, run through `reference/testing.md`; it lists
the checks that catch the common silent failures (driver-name mismatch, json
ABI mismatch, a plugin that loads but never publishes).

## Reference

- `reference/lifecycle.md` — per-host loop flow, threading, what runs once vs. per tick.
- `reference/return-types.md` — the full return-value matrix per method and per host, including how `mads worker` differs from `mads filter`.
- `reference/settings.md` — INI to `set_params()`, injected keys, reserved keys, `-o` overrides, the Datastore.
- `reference/io.md` — JSON frames, topics, binary blobs, `agent_id`, wire format.
- `reference/testing.md` — the standalone `main()`, `mads inspect_plugin`, running against a live broker, debugging checklist.
- `reference/deployment.md` — installing, `PLUGIN_SUFFIX`, OTA delivery via the broker, multi-driver libraries.
- `reference/rust.md` — the Rust API and how it maps onto the C++ one.
- `reference/migration.md` — protocol versions and `mads plugin --update`.

<!-- Generated by `mads plugin` from MADS {{mads_version}}. Refresh with `mads plugin --update`. -->
