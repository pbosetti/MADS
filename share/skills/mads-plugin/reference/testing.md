# Testing and debugging a plugin

## The standalone executable

The generated `CMakeLists.txt` compiles the **same source file** twice: once as
the `.plugin` shared library, once as a plain executable whose entry point is
the `main()` at the bottom of the file. That `main()` is where plugin logic
gets tested — no broker, no network, no MADS process.

| Platform | Shared library | Test executable |
|---|---|---|
| macOS | `build/<name>.plugin` (also *is* the executable) | `./build/<name>.plugin` |
| Linux | `build/<name>.plugin` | `./build/<name>` |
| Windows | `build/<name>.plugin` (DLL) | `build/<name>.exe` (under the config directory in a multi-config build; its import library and PDB are redirected to `build/exe` so they do not collide with the plugin's) |

Write it as a real test driver, not a demo:

- exercise the plugin the way the host does — `set_params()`, then repeated
  `get_output()` / `load_data()`+`process()` calls;
- assert on both the returned `return_type` **and** the JSON payload;
- feed deterministic input (no randomness, no hardware, no wall-clock
  dependence) so failures are reproducible;
- print enough context to diagnose a failure;
- exit non-zero when a check fails, so CI and `cmake --build` scripting can use it.

Cover at least: the nominal frame, an input that must produce `retry`
(incomplete/absent data), a malformed input, and parameter defaults versus
overrides.

Note that `PLUGIN_NAME` is only defined for the library target; the test
executable falls back to the `#define` at the top of the source file. With a
`PLUGIN_SUFFIX` set, the two therefore report different `kind()` values — the
library's is the authoritative one.

## Inspecting the built plugin

```bash
mads inspect_plugin build/<name>.plugin        # human readable
mads inspect_plugin build/<name>.plugin -j     # JSON, for scripting
```

It reports whether the library loads, which drivers it registered (type, name
and protocol version), and which `nlohmann/json` version it was built against.
Exit codes: `0` current, `1` not loadable or usage error, `2` loaded but the
protocol is older than this MADS.

This is the fastest answer to the three most common failures:

| Symptom | What `inspect_plugin` shows |
|---|---|
| `cannot load plugin file` | `loadable: no` plus the loader error — a missing dependency, or `register_pugg_plugin` not exported (a forgotten `MADS_REGISTER_PLUGINS`). |
| `cannot find plugin driver <x>` | The driver names actually registered. The host looked for the file stem (or the `driver` setting / `--driver`); make one match the other. |
| Loads, then behaves oddly | `json_found` differs from `json_expected` — the plugin was built against a different `nlohmann/json`, whose RTTI does not match the host's. Rebuild with the pinned version. |

## Running it for real

```bash
mads broker                                  # terminal 1 (needs a mads.ini)
mads source <name>.plugin                    # terminal 2 — or filter / sink
mads feedback                                # terminal 3 — prints every frame
```

Useful during bring-up:

- `-o key=value` overrides a setting without editing `mads.ini`.
- `-n <name>` runs the same plugin under a different INI section.
- `-p <ms>` forces the loop period.
- `mads monitor` shows per-topic rates; `mads top` shows the whole fleet.

## Debugging checklist

1. **The agent exits with "missing '<name>' section".** `mads.ini` needs a
   section named after the *agent* (the plugin file stem, or `-n`).
2. **The agent starts but nothing is published.** Are you returning `retry`
   forever? Is `out` empty on `success` (look for the auto-inserted
   `warning.get_output`)? Is a filter subscribed to its own `pub_topic`?
3. **A filter receives nothing.** Check `sub_topic` against the upstream
   agent's `pub_topic`; subscriptions are prefix matches, and a frame arriving
   on the filter's own `pub_topic` is dropped with a warning.
4. **Settings look empty in `set_params()`.** The plugin gets *only* its own
   INI section. Keys in `[agents]` never reach it.
5. **`kind()` warning at startup.** The class name and the driver name
   disagree; the settings section is chosen by the agent name regardless, so
   this usually means the file was renamed after being built.
6. **Nothing in the logs when a tick fails.** `error` returns publish to the
   `agent_event` topic, not the data topic. Subscribe to `agent_event` (or run
   `mads feedback`, which subscribes to everything) to see them.
7. **The agent dies on the first tick.** A `critical` return throws out of the
   loop; also check that nothing throws out of a plugin method.
