# Release v2.0.3

This is a **minor update**, which means that it is supposed to be compatible with v2.0.x versions. If not, please raise an issue ;) 

To **install this version**, have a look at [the Guide](https://mads-net.github.io/guides/install.html)

# Changes

These changes summarize what was added or updated in `v2.0.3` since `v2.0.2`. They also include intermediate patches released after `v2.0.2`.

## New features

- Added runtime publish/subscribe topic overrides to the C and Python agent wrappers, while extending the C++ API with explicit pub/sub endpoint and topic accessors.
- Added support for initializing an agent with `settings_uri = "none"`, which creates a minimal default configuration without loading a settings file.
- Exposed numeric version macros in `mads.hpp`, making compile-time version checks easier for downstream code.

## Bug fixes

- Fixed agent reconnect handling so repeated `connect()` calls cleanly disconnect previous endpoints before reconnecting.
- Fixed replay streaming from MongoDB views by replacing per-record re-queries with cursor-based iteration, correctly resetting replay state on reconnect/disconnect, and making repeat mode restart reliably.
- Fixed `mongo_replay` view reuse handling so `reuse_view` now calls the correct fetch path instead of rebuilding the replay view.
- Improved plugin-loader diagnostics by preserving warning/error payloads in structured JSON events and reporting when a plugin returns no output.
- Improved loop error reporting by logging caught `std::exception` messages instead of stopping silently.

## For developers

- Enabled `CMAKE_EXPORT_COMPILE_COMMANDS` by default to generate `compile_commands.json` for editors and tooling.
- Added and documented an Android-oriented build path for `MadsCore`, including platform-specific source selection and an explicit unsupported-path implementation for `HttpsClient`.
- Pinned the bundled `mads-director` dependency to `v2.0.3`, enabled its example configuration, and adjusted vendored ZeroMQ linkage for Android builds.
- Updated generated plugin templates to inherit base-class constructors by default, avoiding a common source of custom plugin boilerplate errors.
