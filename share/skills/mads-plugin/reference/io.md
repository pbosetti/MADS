# Frames, topics and blobs

## What a published frame looks like

The plugin fills a `nlohmann::json` object; the agent stamps a few fields on
the way out, **only when the plugin has not already set them**:

| Field | Added by the agent |
|---|---|
| `agent_id` | The agent's id (`-i/--agent-id`, or the `agent_id` setting). |
| `hostname` | The host the agent runs on. |
| `timestamp` | `{"$date": "<ISO 8601>"}` — MongoDB extended JSON, so the logger can store it as a real date. |
| `timecode` | Frame number derived from `timecode_fps`. |
| `clock_offset_us`, `clock_ref` | Only when clock correction is enabled. |

So a plugin does **not** need to timestamp its own frames, and must not
overwrite `timestamp` unless it is replaying recorded data with its own time
base. Setting `agent_id` yourself overrides the agent's value.

Keep the payload a flat-ish JSON object. It is what every downstream filter,
the MongoDB logger and `mads feedback` will see.

## Topics

- The agent publishes on its `pub_topic` setting, which defaults to the agent
  name.
- A plugin can redirect a single frame by setting `out["topic"]`. The host reads
  it and uses it as the publish topic; an empty or absent value means "use the
  configured `pub_topic`".
- **The `topic` key is not stripped from the payload** — it goes out with the
  frame. Remove it yourself if downstream consumers should not see it.
- Subscriptions are ZMQ prefix matches. `sub_topic = [""]` subscribes to
  everything, including this agent's own output.
- A filter that receives a frame on the topic it publishes to has that frame
  **dropped with a warning**, to break the feedback loop. If your filter looks
  like it receives nothing, check that `sub_topic` and `pub_topic` differ.

## Receiving (filters and sinks)

```cpp
return_type load_data(json const &data, string topic = "",
                      vector<unsigned char> const *blob = nullptr) override;
```

- `data` is the decoded JSON frame — including the fields the sending agent
  stamped, so `data["agent_id"]`, `data["timestamp"]["$date"]` and friends are
  available.
- `topic` is the topic the frame arrived on. With multiple subscriptions this
  is how you tell sources apart; do not infer it from the payload.
- `blob` is non-null only for binary frames. In that case `data` is the frame's
  **metadata** object, and `blob` points at the bytes.
- Wire format (JSON or MessagePack) and compression are handled by the agent.
  The plugin always sees decoded JSON.

## Sending binary blobs

```cpp
return_type get_output(json &out, vector<unsigned char> *blob) override {
  out["width"] = w; out["height"] = h;   // metadata travels as JSON
  blob->assign(bytes.begin(), bytes.end());
  return return_type::success;
}
```

- A non-empty `blob` makes the host publish a two-part frame: your JSON as
  metadata, the bytes as payload.
- The `format` field describes the bytes. A **source** gets it from
  `blob_format()` (set `_blob_format` in the constructor or `set_params()`);
  a **filter** always gets `"raw"` unless it sets `out["format"]` itself.
- `blob` is cleared by the host before every call. Never keep a pointer to it.
- Sending both a blob and JSON-only data on alternate ticks is fine — the host
  decides per tick, based on whether `blob` is empty.

## Dummy / no-hardware mode

Design every hardware-facing plugin so it runs without the hardware. The
convention used by the generated templates is a parameter whose empty default
means "no device" (e.g. `address = ""`), in which case the plugin synthesises
plausible frames. That is what makes the plugin testable from its standalone
`main()`, and what lets a whole fleet run on a developer machine.

The base classes also carry a public `bool dummy` member, but **no host sets
it** — it is left over from earlier protocols. Do not rely on it. Read your own
flag from the settings instead (`dummy = true` in the agent's INI section, or
`-o dummy=true`), and document it in the README.
