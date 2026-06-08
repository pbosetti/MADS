# Release v2.1.1

This document summarizes what changed between `v2.1.0` and `v2.1.1`.

To **install this version**, have a look at [the Guide](https://mads-net.github.io/guides/install.html)

# Changes

These changes summarize what was added or updated since `v2.1.0`.

## New features

- Added `mads-package` subcommand for listing, inspecting, and installing optional MADS binary packages from GitHub releases. The tool reads a public package index, resolves compatible release assets for the current OS and architecture, caches metadata for up to six hours, and merges ZIP archives directly into the MADS installation prefix.
- The broker now starts even when another broker is already advertising in the same room, avoiding hard failures during restarts or overlapping deployments.
- Added JSON output mode for `mads --version` and `mads --rooms` to support programmatic consumption.

## Improvements

- Refactored the FSM main template (`fsm_main.tpl`) to use `AgentApp`, aligning it with the shared startup infrastructure introduced in v2.1.0.
- Enhanced command-line error handling across executables: parse errors and unrecognized options now produce detailed, actionable messages.
- Added accessors for client and server public keys to the CURVE authentication layer, making it easier to retrieve and inspect in-use key material.

## Bug fixes

- Fixed an error in setting up CURVE encryption directly from key strings (regression introduced in v2.1.0).
- Fixed the `agent_sub_topics` C API function to use `size_t` for the topic count parameter and adjusted its return type accordingly.
- Fixed settings retrieval in the Python wrapper (`mads_agent.py`) where the method behaved incorrectly under certain broker configurations.
- Fixed the Python wrapper for Windows compatibility.

## Documentation and packaging

- Added `mads-package` man page documenting all options, the package index format, caching behavior, and platform asset selection rules.
- Updated the FSM plugin template `CMakeLists.txt` to reflect the `AgentApp`-based startup changes.

## For developers

- The `mads-package` tool sources its package list from the public MADS package index at `MADS-NET/.github` and uses the GitHub Releases API to resolve assets, with a structured on-disk cache keyed by schema version.
- Package entries in the index now carry a `type` field alongside the repository URI, enabling the installer to apply type-specific handling in future releases.
