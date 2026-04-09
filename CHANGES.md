# Release v2.0.3-6-g722b6ce

This document summarizes what changed between `v2.0.3` and `v2.0.4`.

To **install this version**, have a look at [the Guide](https://mads-net.github.io/guides/install.html)

# Changes

These changes summarize what was added or updated since `v2.0.3`.

## New features

- Added the new `inspect_plugin` command-line tool to probe a plugin file, report its detected driver types, and check whether its protocol version matches the current runtime.
- Extended `broker` with interactive cycling through available network endpoints, making it easier to inspect the settings URL exposed on each interface at runtime.

## Improvements

- Improved `broker` console output with clearer timestamps, better daemon-mode logging, and more explicit reload and attachment diagnostics.
- Reworked broker network-interface discovery to enumerate available IPv4 addresses and derive settings URLs from them instead of requiring manual NIC selection.
- Updated the default `mads.ini` attachment path to point to the installed plugin location under `../lib/publish.plugin`.
- Added conditional terminal cursor rollback support in `goback.hpp`, allowing status output to remain readable when cursor rewinding should be disabled.

## Bug fixes

- Fixed Docker-based builds by adding `pkg-config` to the image dependencies and copying the `cmake` directory into the container build context.
- Fixed vendor configuration ordering for `inja` so builds do not conflict when `gv2fsm` pulls the same dependency through `FetchContent`.
- Updated plugin build properties so the `replay` plugin can carry an explicit runtime path to `libMadsCore` where needed on newer macOS builds.

## For developers

- `inspect_plugin` supports both human-readable and JSON output, which makes it suitable for manual debugging as well as CI or packaging checks. This also allows inspecting plugins from latest versions of the MADSCode Visual Studio plugin.
- The broker changes simplify testing on systems with multiple interfaces by exposing the full set of candidate settings URLs directly from the runtime.
