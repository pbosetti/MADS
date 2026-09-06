# Settings: from `mads.ini` to `set_params()`

## Where the values come from

The host reads a TOML/INI configuration — either a local `mads.ini` or the copy
served by the broker — and hands the plugin **the section named after the
agent**, converted to a JSON object.

```
mads source mysensor.plugin            ──▶ agent name "mysensor" ──▶ section [mysensor]
mads source mysensor.plugin -n bench1  ──▶ agent name "bench1"   ──▶ section [bench1]
```

Two consequences that catch people out:

- **The section is named after the agent, not the plugin file.** With `-n` the
  same plugin binary can run twice with different configurations.
- **A missing section is a fatal error.** The agent refuses to start with
  `Invalid settings file: missing '<name>' section`. Always ship a documented
  `[<name>]` block in the project README, even when every key is optional.

Only that one section reaches the plugin. Keys in `[agents]` are framework-wide
agent settings (broker addresses, timecode, clock sync) and are consumed by the
host — the plugin never sees them.

In protocol P8, `kind()` does **not** select the settings section. It is only
checked against the driver name the plugin was loaded under, and a mismatch is
reported as a warning. (Older documentation and older template comments say
otherwise; they predate P8.)

## What the host adds

Beyond the INI keys, the host injects:

| Key | Value |
|---|---|
| `agent_name` | The resolved agent name (i.e. the section name). |
| `agent_id` | From `-i/--agent-id`, if given. `Source::set_params()` and friends copy it into `_agent_id`, which is why an override must call the parent method. |
| `prefix` | The MADS install prefix, the same path `mads -p` prints. Use it to locate installed resources instead of hardcoding paths. |

And `-o key=value` (repeatable) patches the object *after* the INI, so it wins.
Values are coerced: `true`/`false` become booleans, integers become integers,
decimals become doubles, anything else stays a string.

```bash
mads source mysensor.plugin -o address=/dev/ttyUSB0 -o baud_rate=115200 -o verbose=true
```

## Names you must not use

These keys are consumed by the agent itself. Reusing them for plugin parameters
either has no effect or silently changes the agent's behaviour:

| Key | Meaning |
|---|---|
| `pub_topic` | Topic this agent publishes on (default: the agent name). |
| `sub_topic` | Topics this agent subscribes to (string or array; `""` means all). |
| `period` | Loop period in ms, same as `-p`. |
| `time_step`, `time_step_us`, `high_res_loop`, `spin_margin_us` | Fine-grained loop pacing. |
| `receive_timeout` | How long a blocking receive waits. |
| `dont_block` | Non-blocking receive for filters (see `lifecycle.md`). |
| `wire_format` | `"json"` or `"msgpack"`. |
| `compression` | `"auto"`, `"snappy"` or `"none"`. |
| `queue_size` | ZMQ high-water mark (`high_watermark` is the deprecated spelling). |
| `driver` | Which registered driver to instantiate from the plugin file. |
| `attachment`, `attachment_ext` | OTA plugin delivery; see `deployment.md`. |
| `clock_source`, `clock_sync_responder` | Clock-offset behaviour. |
| `dummy` | Framework-wide dummy mode. |

## The `set_params()` idiom

```cpp
void set_params(const json &params) override {
  Source::set_params(params);        // 1. parent first: it captures agent_id
  _params["address"] = "";           // 2. your defaults
  _params["baud_rate"] = 115200;
  _params.merge_patch(params);       // 3. caller wins over defaults
  // 4. validate, then open devices / allocate / prepare state
  if (!_params["address"].get<string>().empty()) {
    // ... open the port; on failure set _error and remember to fail the
    //     first get_output() with return_type::critical
  }
}
```

Rules:

- Call the parent implementation **first**, or `_agent_id` is never set.
- Set defaults **before** `merge_patch`, never after.
- `merge_patch` (not `update`) so nested objects merge rather than replace.
- Read every parameter into a typed member here. Doing `_params["x"].get<int>()`
  inside the hot loop re-parses JSON on every tick.
- `set_params()` returns `void`: it cannot report failure. Validate here, store
  the problem in `_error`, and return `return_type::critical` from the first
  data call. Anything you print goes to the agent banner via `info()`.

## `info()`

Called once, right after `set_params()`; the map is printed as the agent's
startup banner. Return the *resolved* configuration — the device actually
opened, the file actually created, the mode actually selected. This is the only
place an operator sees what the plugin decided.

## Datastore (C++, `mads plugin --datastore`)

`Datastore` persists a JSON document between runs, in
`<system temp dir>/mads/<name>.json`. It is loaded by `prepare()` and written
back on `save()` and on destruction.

```cpp
_datastore.prepare(kind());   // in set_params(); ".json" is appended if missing
_datastore["counter"] = 0;    // read/write one key
_datastore.data();            // the whole json object
_datastore.save();            // force a save; it also saves on destruction
```

Report `_datastore.path()` from `info()` so the operator can find the file.
