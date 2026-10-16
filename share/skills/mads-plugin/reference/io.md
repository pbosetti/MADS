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

## Zero-copy blobs (protocol P9, opt-in)

When the producing agent runs with `shm = true`, large blobs stay in a
shared-memory ring owned by that agent and only a small descriptor travels over
ZeroMQ. Same-host consumers read the bytes in place; every consumer of the
topic must run on the producer's host, as the same OS user and in the same
container (segments are owner-only), or it drops every blob. Nothing changes for a
plugin that does not opt in: input blobs still arrive through `load_data()`
(the default `load_data_view()` makes one copy), output still goes through
`get_output()`/`process()`. `mads plugin --zero-copy` scaffolds a new plugin
with these overrides already in place.

Zero-copy **input** (filters and sinks): override `load_data_view()`.

```cpp
return_type load_data_view(json const &meta, std::string topic,
                           BlobView blob) override {
  // blob.data / blob.size: read-only, valid only during this call
  // (for a filter: through the following process()). Copy what you keep.
  _checksum = crc32(blob.data, blob.size);
  return return_type::success;
}
```

Zero-copy **output** (sources and filters): write straight into a ring slot.

```cpp
bool supports_shared_output() const override { return true; }

return_type get_output_shared(json &out, BlobBuffer &buf) override {
  size_t n = _frame.size();
  if (n > buf.capacity) {        // does not fit in a slot:
    buf.size = n;                // tell the host how much is needed,
    return return_type::retry;   // it falls back to get_output() this tick
  }
  std::memcpy(buf.data, _frame.data(), n);
  buf.size = n;
  out["size"] = n;
  return return_type::success;
}
```

- The ring is **lossy**: a slow consumer can see its slot overwritten. The host
  re-checks the slot after `load_data_view()` returns and drops (and counts) a
  torn read, so never act on data you could not finish reading.
- A filter's `process_shared()` mirrors `get_output_shared()`.
- Shared output is used only when the agent's settings section has
  `shm = true`; consumers on another host get an error, not the data.

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
