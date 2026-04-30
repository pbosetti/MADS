# Release v2.1.0

This document summarizes what changed between `v2.0.4` and `v2.1.0`.

To **install this version**, have a look at [the Guide](https://mads-net.github.io/guides/install.html)

# Changes

These changes summarize what was added or updated since `v2.0.4`.

## New features

- Added UDP service discovery for MADS brokers and agents. Brokers can advertise their settings endpoint in a named room, and agents can opt in with `--room` to discover the broker instead of hard-coding a settings URI.
- Added the `ServiceDiscovery` API and `service_discovery_demo` tool for advertising services, discovering rooms, validating sender addresses, and listing advertised providers on the local networks.
- Added `mads --rooms` to list advertised rooms, provider hostnames, settings URLs, and advertised MADS versions.
- Added broker-side service advertising, room reservation, configurable discovery intervals, hostname metadata, version metadata, and loopback preference for local agents.
- Added direct CURVE key setup functions to the C API, allowing clients to configure public and secret keys from strings in addition to key files.

## Improvements

- Refactored executable startup around `AgentApp`, sharing option parsing, standard exit handling, settings loading, service discovery, queue configuration, receive timeouts, events, and restart behavior across agent executables.
- Extended agent command-line options with consistent CURVE encryption flags, settings timeout support, queue-size handling, service-discovery room selection, and `--silent` support for plugin loaders.
- Improved plugin loader behavior with smart-pointer ownership, better option handling, cleaner status output, and support for silent processing mode.
- Improved broker interface handling with broadcast-capable interface discovery, interface-bound UDP sending, better Windows compatibility, and safer cleanup around network interface enumeration.
- Improved local discovery behavior by validating advertised IPs against packet senders and preferring `127.0.0.1` when the discovered service is running on the same host.

## Bug fixes

- Fixed malformed JSON handling and error reporting in agent, logger, and plugin-loader paths so parse failures are reported more clearly.
- Fixed `dont_block` with `queue_size=1` behavior.
- Fixed and hardened Windows networking support for service discovery.
- Fixed C API setters such as settings timeout and high watermark to return status codes and preserve diagnostic errors instead of throwing through the C boundary.
- Removed stale Doxygen input patterns and simplified generated documentation configuration.

## Documentation and packaging

- Updated man pages for broker and agent executables with the new service-discovery, CURVE, queue, silent, and settings options.
- Added a GitHub Actions workflow for publishing Doxygen documentation to GitHub Pages.
- Added a custom Doxygen stylesheet and logo configuration.
- Updated default template configuration and workspace metadata for the new service-discovery and plugin paths.

## For developers

- `AgentAppT` can now be used to build executables around `Agent` subclasses without duplicating common CLI and initialization code.
- The C API exposes broker settings discovery through `discover_broker_settings()`, making service discovery available to non-C++ integrations.
- Service advertisements now carry enough metadata for tooling to display provider hostname, advertised settings URL, encryption status, and version.
