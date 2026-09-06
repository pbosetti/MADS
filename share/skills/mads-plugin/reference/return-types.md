# `return_type`: what each value actually does

```cpp
enum class return_type { success = 0, retry, warning, error, critical };
```

The enum is shared by every plugin method, but **its effect is decided by the
host, not by the API**, and the hosts do not all agree. Pick the value from the
table for the host you are targeting.

Before returning `warning`, `error` or `critical`, always assign a
human-readable message to `_error`. The host reads it back through `error()`
and it is the only diagnostic an operator gets. It defaults to `"No error"`.

## `mads source` — `get_output(out, blob)`

| Value | Published on the data topic? | Effect |
|---|---|---|
| `success` | yes | If `out` is empty the host inserts `warning.get_output = "Plugin did not return any output"` and publishes *that* — an empty success is a visible defect, not a silent skip. If `blob` is non-empty the frame is published as blob + metadata, with `format` set from `blob_format()` unless you already set it. |
| `warning` | yes | `out.warning.get_output` is set to your `_error`, then handled exactly like `success`. |
| `retry` | **no** | Nothing is published, nothing is logged, no counter moves. The tick simply ends. This is the correct value for "no complete frame yet". |
| `error` | **no** | `{"error": {"get_output": "<your message>"}}` is emitted as an agent event on the `agent_event` topic (inside the event's `info` field), and the error counter is incremented. Downstream subscribers of your data topic see *nothing at all*. |
| `critical` | **no** | The runtime is stopped and a `std::runtime_error` is thrown out of the loop. The agent terminates. No agent event is emitted, so the message reaches the console only. |

## `mads filter` — `load_data()` then `process()`

`load_data()` decides whether `process()` runs at all:

| `load_data` returns | `process()` called? | Effect |
|---|---|---|
| `success` | yes | — |
| `warning` | yes | An agent event is emitted, **and** `warning.load_data` is merged into whatever `process()` eventually publishes. |
| `retry` | **no** | Tick ends silently. Use it to accumulate input across ticks (windowing, reassembly). |
| `error` | **no** | Agent event emitted, error counter incremented, tick ends. Nothing is published. |
| `critical` | no | Agent event emitted, runtime stopped. |

Then `process(out, blob)`:

| Value | Published? | Effect |
|---|---|---|
| `success` | yes | An empty `out` gets `warning.process = "Plugin did not return any output"` inserted, then is published. With a blob, `format` defaults to `"raw"` (filters do not consult `blob_format()`). |
| `warning` | yes | `out.warning.process` is set to `_error`, then as `success`. |
| `retry` | **no** | Nothing published. Correct when the filter consumed the input but has nothing to emit yet. |
| `error` | **no** | Agent event, error counter, tick ends. |
| `critical` | **no** | Agent event, runtime stopped. |

## `mads sink` — `load_data()`

A sink publishes nothing, so the return value only controls logging:

| Value | Effect |
|---|---|
| `success` | Nothing. |
| `retry` | Nothing — identical to `success` here. |
| `warning` | Logged to the console and emitted as an agent event. |
| `error` | Logged, agent event, error counter incremented. |
| `critical` | Logged, agent event, runtime stopped. |

## `mads worker` — a filter plugin on a different host

`mads worker` runs **filter** plugins against a `mads dealer` work queue. Its
error handling is deliberately simpler, and it differs from `mads filter` in
ways that will surprise you:

| Difference | `mads filter` | `mads worker` |
|---|---|---|
| Input | subscribed topic | `pull()` from the dealer |
| `retry` from either method | nothing published | **published as `{"error": ...}`** |
| `warning` from either method | published with a `warning` field | **published as `{"error": ...}`**, the output is discarded |
| `error` / `critical` | nothing on the data topic; agent event | published as `{"error": ...}`; the agent keeps running even on `critical` |
| Empty `out` on success | a warning field is inserted | published as-is |
| `out["topic"]` | selects the publish sub-topic | ignored |
| `next_loop_duration` | honoured | ignored |
| Blobs | supported | not used |

In short: under `mads worker`, **only `success` is not an error**. A filter
plugin that relies on `retry` for normal idling must not be deployed on
`mads worker` without adaptation.

## Choosing a value

- Waiting for more data, no complete frame yet → `retry`. Never `error`.
- Produced usable output, but something was off (a value clamped, a field
  missing, a fallback used) → `warning`, and put the reason in `_error`.
- This input or this tick is unusable, but the next one may be fine (a parse
  failure, a transient device read error) → `error`.
- Continuing is meaningless: the configured device does not exist, a mandatory
  parameter is missing, the output file cannot be opened → `critical`. Prefer
  detecting these in `set_params()` and failing loudly on the first tick.

## `retry` and the loop period

`retry` ends the tick without publishing. It does **not** spin: the host waits
for the configured loop period exactly as it would after a successful tick.

`retry` is the one value that **discards `next_loop_duration`**: it returns
from the tick early, before the host reads the pacing hint, so the configured
period always applies. `success`, `warning` and `error` all honour
`next_loop_duration`. To back off after a failed device read, set
`next_loop_duration` to the backoff and return `error` (or `warning`), not
`retry`.
