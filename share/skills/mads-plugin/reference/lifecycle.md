# Lifecycle: what the host calls, when

The host agent (`mads source`, `mads filter`, `mads sink`, `mads worker`) is a
generic executable. It owns the process, the broker connection and the timing;
the plugin only supplies callbacks. This file describes exactly when each of
them runs.

## Once, at startup — in this order

| # | Host action | Plugin method | Notes |
|---|---|---|---|
| 1 | resolve the plugin file | — | `--plugin` argument > a broker-served attachment (OTA) > the host's built-in default. If the path does not exist it is retried under `<prefix>/lib` (Unix) or `<prefix>/bin` (Windows). The `.plugin` extension is appended if missing. |
| 2 | resolve the agent name | — | `-n/--name` > the plugin file stem. **This name selects the INI section**, so it is fixed before anything is loaded. |
| 3 | fetch settings | — | From the local `mads.ini` or from the broker. May also deliver the plugin itself as an attachment. |
| 4 | `dlopen` / `LoadLibrary` | `register_pugg_plugin()` | Emitted by `MADS_REGISTER_PLUGINS(...)`. It installs one driver per class listed. Exceptions are swallowed and reported as a failed registration. |
| 5 | look the driver up by name | — | `--driver` > the `driver` setting > the plugin file stem. Failure prints the list of drivers the library actually registered. |
| 6 | protocol check | (`version`) | The plugin's `PLUGIN_PROTOCOL_VERSION` must be at least the host's minimum, otherwise the host exits. |
| 7 | consistency check | `kind()` | Compared to the driver name the plugin was loaded under; a mismatch is a warning, not a failure — but it means your INI section and your class disagree. |
| 8 | instantiate | **constructor** | Runs with **no settings available**. Do nothing here beyond zero-initialising members. |
| 9 | connect to the broker | — | Sockets are up before the plugin is configured. |
| 10 | configure | **`set_params(settings)`** | Called exactly once. All setup — opening devices, allocating buffers, reading configuration, preparing a `Datastore` — belongs here. |
| 11 | announce | **`info()`** | Called once; the returned map is printed as the agent's banner. Return values you want an operator to see (device path, resolved file names, mode). |
| 12 | announce blob format | **`blob_format()`** | **Sources only**, once. The string is used as the default `format` field of published blob frames. |
| 13 | optional `--delay` | — | A one-off sleep before the first iteration. |
| 14 | startup event | — | Published on the `agent_event` topic with the effective settings. |

Then the main loop starts. It ends on SIGINT, on a remote-control stop, or when
the plugin returns `return_type::critical`.

## Once, at shutdown

Shutdown event ➔ `disconnect()` ➔ the plugin object is destroyed ➔ drivers are
cleared ➔ the process may relaunch itself if a restart was requested remotely.
Release resources in the **destructor**; there is no `stop()` callback.

## Per tick — `mads source`

```
out.clear(); blob.clear();
rt = get_output(out, &blob);
   success/warning ─▶ publish (see return-types.md)
   retry           ─▶ nothing published
   error           ─▶ agent event, nothing published
   critical        ─▶ stop the runtime and throw
sleep for next_loop_duration, or for the configured period if it is zero
```

The tick is *unconditional*: a source is a clock, not a reactor. If your data
is not ready yet, return `retry` — do not wait inside `get_output()`.

## Per tick — `mads filter`

```
type = receive()                       (blocking up to receive_timeout, unless dont_block)
  ├─ topic == our own pub_topic ─▶ frame dropped with a warning (loop guard), tick ends
  ├─ topic == "control"         ─▶ handled by the host, tick ends
  ├─ a JSON frame               ─▶ load_data(in, topic)
  ├─ a blob frame               ─▶ load_data(metadata, topic, &bytes)
  └─ nothing received           ─▶ dont_block ? go straight to process() : end the tick
rt = load_data(...)
   success        ─▶ continue
   warning        ─▶ continue, and the warning is attached to the eventual output
   retry          ─▶ process() is NOT called, tick ends
   error          ─▶ process() is NOT called, agent event, tick ends
   critical       ─▶ agent event, runtime stopped
rt = process(out, &blob)
   success/warning ─▶ publish
   retry           ─▶ nothing published
   error/critical  ─▶ as above
sleep for next_loop_duration, or for the configured period if it is zero
```

Note the `dont_block` path: with `dont_block` set, `process()` is called on
every tick **even when no message arrived**, and `load_data()` is not. A filter
that aggregates over time (windowing, decimation, statistics) uses this; a
filter that maps one input to one output must not, or it will republish stale
state at the loop rate.

## Per tick — `mads sink`

```
type = receive()                       (blocking up to receive_timeout)
  nothing received ─▶ tick ends
  otherwise        ─▶ load_data(in, topic[, &bytes])
rt = load_data(...)
   success/retry  ─▶ nothing happens; the tick just ends
   warning        ─▶ logged and emitted as an agent event
   error          ─▶ logged, agent event, error counter incremented
   critical       ─▶ logged, agent event, runtime stopped
```

A sink never publishes and has **no `next_loop_duration`**: its pace is set
entirely by incoming traffic and `receive_timeout`.

## Per tick — `mads worker`

`mads worker` also runs **filter** plugins, but it is a different host: it
pulls work from a `mads dealer` over a PULL socket instead of subscribing to a
topic. Its loop is simpler and its error handling is **different** — see
`return-types.md`. It ignores `next_loop_duration`.

## Timing

- The loop period comes from `-p/--period` or the `period` setting. With
  neither, the agent free-runs.
- A method's `next_loop_duration` (sources and filters) sets the delay *before
  the next iteration*. **Zero means "use the configured period"**, not "run
  immediately". Set it to a non-zero value to pace a single iteration
  differently — e.g. backing off after a failed device read.
- The host may also pace itself from `time_step` / `time_step_us` /
  `high_res_loop` in the INI section; those are agent settings, not plugin ones.

## Threading

- Sources run remote control on a **separate thread**; filters and sinks handle
  it inside the loop.
- Everything the plugin sees is single-threaded: all callbacks run on the loop
  thread, in the order above. Members need no locking unless *you* start a
  thread — in which case you own the synchronisation, and you must join it in
  the destructor.
