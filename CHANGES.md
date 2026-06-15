# Release v2.2.0

This document summarizes what changed between `v2.1.1` and `v2.2.0`.

To **install this version**, have a look at [the Guide](https://mads-net.github.io/guides/install.html)

> **Compatibility note:** the agent/broker settings handshake is gated on
> `MAJOR.MINOR` (`LIB_VERSION_CHECK`). Because this is a minor bump, `v2.2.x`
> agents will not exchange settings with a `v2.1.x` broker (or vice versa);
> upgrade the broker and agents together. The published *data* wire format is
> unchanged by default (header-less, snappy-compressed JSON), so the new MsgPack
> encoding is fully opt-in.

# Changes

These changes summarize what was added or updated since `v2.1.1`.

## New features

- **Opt-in MessagePack wire format.** Agents can now publish payloads encoded as MessagePack instead of JSON via `set_wire_format(WireFormat::MsgPack)`. Messages carry a small self-describing frame header (`format`, `compression`, `schema_version`); receivers transparently decode both the new headered frames and the legacy header-less JSON frames, so the choice only affects what an agent publishes. The broker remains payload-opaque and needs no changes.
- **Configurable from `mads.ini`.** The wire format and compression policy can be selected per agent via the `wire_format` (`"json"` | `"msgpack"`) and `compression` (`"auto"` | `"snappy"` | `"none"`) keys in the agent's settings section, with no code changes.
- **C and Python bindings** for the new controls: `agent_set_wire_format`/`agent_wire_format` and `agent_set_compression`/`agent_compression` in the C API, and the corresponding `set_wire_format`/`wire_format`/`set_compression`/`compression` methods (plus `WireFormat` and `Compression` enums) in the Python wrapper.
- **Compression policy.** Added `set_compression(Compression::None|Snappy|Auto)`/`compression()`. `Auto` (the new default) compresses only payloads at or above a size threshold, so small control/status frames are no longer needlessly snappy-framed while large frames keep their previous compression. The chosen codec is recorded in the frame header.
- **Explicit delivery semantics.** Added `set_delivery(Delivery::Queued|LastKnownValue)` / `delivery()` to control subscriber buffering directly, decoupled from the queue size.
- **Zero-copy blob access.** Added `last_blob_view()`, returning non-owning views (`string_view`/`span`) into the last received blob to avoid the full copy performed by `last_blob()`.
- **Dropped-message counter.** Added `dropped_messages()`, exposing how many malformed or undecodable frames have been skipped.
- Added `install_signal_handlers()` for explicit, idempotent SIGINT/SIGTERM setup.

## Improvements

- Malformed or undecodable incoming frames (bad part counts, non-snappy payloads, failed MessagePack decode) are now dropped and counted instead of throwing, so a single bad message can no longer terminate an agent's main loop.
- `snappy::Uncompress` results are now checked on every receive path (including the threaded remote-control drain), preventing storage of garbage payloads.
- The loop watchdog is now cooperative: it owns a joinable handle, force-exits only after a bounded grace period, and uses `_Exit` to avoid hanging static destructors. An orderly `shutdown()` stops it cleanly.
- SIGINT/SIGTERM handlers are installed once per process, so repeated `loop()` calls or multiple agents in one process no longer clobber handler state.
- Threaded remote control now exclusively owns the subscriber socket and guards against concurrent `receive()` calls.
- Settings and broker timecode are fetched over a single shared REQ socket at startup, and CURVE client setup was factored into one helper, removing duplicated connection cycles.
- The settings JSON projection is cached at initialization instead of re-serialising TOML on every event.
- Non-startup/shutdown events are published synchronously, avoiding a thread (and a full settings copy) per event.
- `publish()` no longer overwrites caller-provided `hostname`/`timestamp` fields; it only stamps fields that are absent.

## Bug fixes

- `set_high_watermark(0)` now means "unlimited" (ZeroMQ semantics) instead of silently switching to Last-Known-Value mode. The `queue_size == 1 ⇒ LKV` convenience mapping is retained.
- Fixed a ctypes argument-type mismatch in the Python wrapper (`mads_agent.py`) where `agent_sub_topics` was called with a `c_int` count instead of `c_size_t`, matching the C API's `size_t *` parameter.
- Blob reception now performs a single copy of the payload bytes instead of two.

## Deprecations

- `set_conflate()` / `conflate()` are deprecated; use `set_delivery(Delivery::LastKnownValue)` / `delivery()` instead.

## For developers

- Introduced a self-describing on-the-wire frame header (magic `MADS`, header version, format, compression, flags, and `schema_version`) as the foundation for evolving the payload format without broker changes. See `REFACTOR.md` and `MSGPACK.md` for the design rationale.
- Named constants (`DEFAULT_RECEIVE_TIMEOUT_MS`, `DEFAULT_SETTINGS_TIMEOUT_MS`, `STARTUP_SHUTDOWN_DELAY_MS`) replace previously scattered magic numbers, and the documented receive-timeout default now matches the implementation (500 ms).
- Documented and asserted the broker's payload-opaque invariant in `broker.cpp`.
