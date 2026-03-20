# Release v2.0.2

This is a **minor update**, which means that it is supposed to be compatible with v2.0.x versions. If not, please raise an issue ;) 

To **install this version**, have a look at [the Guide](https://mads-net.github.io/guides/install.html)

# Changes

These changes summarize what was added or updated in `v2.0.2` since `v2.0.1`.

## New features

- Added the new `mads-fsm` utility to generate FSM-based agents from Graphviz/FSM descriptions.
- Extended the C agent wrapper with version and default-settings accessors, richer API documentation, and broader test coverage.

## Bug fixes

- Improved agent shutdown and disconnect behavior by coordinating thread termination, unblocking pending receives, and closing ZeroMQ sockets with zero linger to avoid hangs.
- Fixed Windows shutdown and broker-related stability issues, including hangs caused by `ZMQ_LINGER`, pipe deadlocks, process-group handling, and timing problems in broker tests.
- Fixed Linux attachment handling when writing broker-provided settings to temporary files.
- Added connection-state validation in the C wrapper so initialization, connect, publish, receive, event registration, and disconnect fail cleanly when used in the wrong state.
- Fixed agent publication metadata handling so `agent_id` and `hostname` are populated automatically when missing.
- Fixed `set_conflate()` so it honors the requested value instead of always enabling conflation.
- Improved thread safety around received-message state by guarding status, last message, and last blob access.
- Fixed library-path configuration across Windows and non-Windows builds.

## For developers

- Added a standalone smoke test suite covering installed executables, plugins, generated plugin stubs, C and C++ linkage, runtime messaging, and the Python wrapper.
- Installed and exported a proper CMake package configuration (`MadsConfig.cmake` and version file), making `find_package(Mads REQUIRED)` work in downstream projects.
- Updated FetchContent usage for `nlohmann/json` to use release tarballs with preserved extraction timestamps.
- Added `gv2fsm` as a vendored dependency and linked it into the new `mads-fsm` executable.
- Improved plugin and replay target linking for static MongoDB client libraries.
- Added smoke-test support for configurable build configurations, especially for multi-config generators such as Visual Studio on Windows.
- Updated ignore rules for generated build directories and local helper files used during development.
