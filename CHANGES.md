# Release v3.0.0

This major release adds a local transport (ipc sockets and shared memory) for
agents that run on the same host as the broker, a graphical node editor for
`mads.ini` that can also edit the settings file of a running broker, and
version 9 of the plugin protocol (P9). It is not interoperable with 2.5.x
components: see *Compatibility*.

## Compatibility

Read this before upgrading from 2.5.x.

- **Upgrade the broker and all agents together.** The settings handshake
  compares `MAJOR.MINOR` (`Mads::check_version()`, `LIB_VERSION_CHECK`), so a
  3.0 broker refuses 2.5 agents and the other way round. Agents that link
  MadsCore must be recompiled: `Agent` gained members.
- **Plugins built for P8 or older are refused.** The plugin protocol moves
  from P8 to P9 and the loaders accept exactly the host's protocol: an older
  `.plugin` stops with an error asking to rebuild it, and a plugin built for a
  *newer* protocol is now refused too, instead of crashing on a shifted
  vtable (`mads-source`, `-filter`, `-sink`, `mads-worker`, `mads doctor`,
  `mads inspect_plugin`). **Plugin authors:** run `mads plugin --update DIR`
  and rebuild. The P8 -> P9 step only moves the `mads_plugin` pin to
  `v2.6-P9` and defaults the build type to Release before `project()` in
  `CMakeLists.txt`; no source change is needed, the zero-copy hooks are
  opt-in (see *For developers*).
- **Agents on the broker's host now use ipc by default.** `transport = "auto"`
  connects over `ipc://` sockets when the broker runs on the same host and the
  socket files are visible, and over tcp otherwise. Set `transport = "tcp"`
  (in `[agents]`, in one agent's section, or with `--transport tcp`) to keep
  tcp everywhere. The broker binds the ipc sockets in addition to tcp
  (`[broker] ipc = false` turns this off); tcp clients are unaffected.
  `ip_whitelist` does not apply to ipc peers: access to them is controlled by
  the owner-only runtime directory (CURVE still applies if enabled).
- **Wire format:** unchanged unless an agent sets `shm = true` (default
  `false`). Shared-memory messages carry a new header flag and are sent as
  `[topic][header blob|shm][meta][empty]`, so receivers that predate the flag
  see an empty blob with the producer's metadata. Consumers must run on the
  producer's host, as the same OS user and in the same container.
- **A bare `mads ini` now opens the editor** on `<prefix>/etc/mads.ini` (or a
  new document from the template if that file does not exist) instead of
  printing the template. Use `mads ini --print` for the old output. Builds
  without the GUI keep printing the template.
- **mads-chat settings moved** to `mads-chat.json` in a per-user folder
  (`~/Library/Application Support/MADS` on macOS, `$XDG_CONFIG_HOME/mads` or
  `~/.config/mads` on Linux, `%APPDATA%\MADS` on Windows; `MADS_CONFIG_DIR`
  overrides it). An existing `./mads_chat_settings.json` is copied there on
  first start (never deleted) and can then be removed.
- Remote editing of the broker's settings is new and off by default: nothing
  to migrate.

## New features

### Local transport and shared memory

- **ipc transport for agents on the broker's host.** The broker binds its
  XSUB, XPUB and settings sockets a second time on `ipc://` endpoints (socket
  files `broker-<port>-front|back|settings.ipc` under `ipc_dir`, default
  `$XDG_RUNTIME_DIR/mads` or `/tmp/mads-<uid>`, `%LOCALAPPDATA%\mads\run` on
  Windows; owner-only). Agents learn about them through the new `transport`
  settings command. Keys: `[broker] ipc`, `ipc_dir`, `frontend_ipc`,
  `backend_ipc`, `settings_ipc`; `[agents]` or per-agent `transport`
  (`auto`, `tcp`, `ipc`; `ipc` fails at startup if it cannot be honoured) and
  `--transport` on every agent. A settings URL of the form `ipc://...` is
  accepted. A live socket is never taken over, stale ones left by a crashed
  broker are replaced, and the broker removes its socket files on exit. The
  startup banner and the **N** key list the ipc endpoints and the agent info
  block shows the transport in use. See **mads-broker**(1), LOCAL TRANSPORT.
- **1 MiB kernel buffers on ipc sockets** (`ipc_sndbuf`, `ipc_rcvbuf`; 0 = OS
  default). The AF_UNIX default (8 KiB on macOS) made ipc slower than
  loopback tcp for messages of 256 KiB and up. On Linux requests are capped by
  `net.core.wmem_max`/`rmem_max`. Needs the bundled libzmq patch (see
  *For developers*).
- **Shared-memory blobs.** An agent with `shm = true` (also `-o shm=true` on
  the plugin loaders; `shm_slots`, default 8, and `shm_slot_size`, default
  16 MiB) publishes blobs through a lossy shared-memory ring and sends only a
  reference; blobs larger than a slot go inline with a one-time warning.
  Consumers read the bytes in place or through one copy; a slot overwritten
  while being read is dropped and counted, never delivered torn. It is
  enabled only if the broker is on this host, and all consumers must run on
  the producer's host as the same OS user and in the same container. Windows
  and POSIX are supported. See **mads-source**(1), SHARED MEMORY, and the
  `[agents]` comments in `mads.ini`.
- **`mads record` reports shared-memory payloads it could not recover**
  (overwritten or unreadable) at exit, hinting to raise the producer's
  `shm_slots`; `mads play` republishes recorded shm blobs as real bytes.
- **`mads doctor` checks the local transport** (check 8): ipc support in
  libzmq, ownership and mode of the runtime directory, the ipc endpoints the
  broker advertises and whether their socket files are visible, and which
  transport an agent here would select (including per-agent overrides). Check 9
  lists the POSIX shared-memory registry and flags orphaned segments of dead
  producers, which `--fix` removes. The open-file limit check is now number
  10. See **mads-doctor**(1).

### The mads.ini editor

- **`mads-ini`, a node editor for `mads.ini`** (also started by `mads ini`).
  One node per agent section, colored by role, one link per topic, computed
  with the agents' own matching rules (exact, prefix, MQTT wildcards).
  Drag from an output to an input to subscribe; Ctrl-click (Cmd-click on
  macOS) or Delete detaches a link. A palette creates built-in agents, plugin
  loaders, custom agents, `federate` (one node per network side) and
  `perf_assess`; the inspector edits every schema key with typed widgets,
  per-key comments and enable/comment-out; a Problems list shows lint
  results. Nodes of custom and Python agents take their role (source, filter,
  sink) from the active `pub_topic`/`sub_topic` keys, or from the inspector's
  Role combo. Comments and layout are preserved (positions and roles go in a
  hidden `#@` line), saves are atomic with a `.bak` copy and conflict
  detection, the file is reloaded when changed on disk, with undo/redo. The
  editor uses the same font as mads-chat and mads-director. See
  **mads-ini**(1).
- **Headless editing.** `mads ini --check [FILE]` lints a file (exit 0 clean,
  1 warnings, 2 errors); `--get SECTION.KEY` prints a raw value; repeatable
  `--set SECTION.KEY=VALUE` with `--comment TEXT` changes keys keeping all
  comments (a plain word is stored as a string). All of them refuse a missing
  settings file up front. `mads ini --print` prints the template.

### Remote editing of the broker's settings

- **Edit a running broker's `mads.ini` from another machine.** Opt-in with
  `[broker] remote_edit = true` (plus `edit_address`, default
  `tcp://*:9094`, `editors_dir`, `edit_ipc`, `edit_backups`, default 10). On
  the broker's host the editor connects over an owner-only ipc endpoint with
  no key or password. Over tcp the service needs `--crypto` and admits only
  the public keys in `editors_dir` (default `<keys_dir>/editors`, created by
  the broker if missing). The *Open broker settings* panel of the editor lists
  the brokers found by `--room` discovery (the edit port is advertised as
  `edit`), the local broker (enabled only when its socket is live), or a
  manual `host[:port]`, `tcp://` or `ipc://` address. Saves are validated as
  TOML (4 MiB cap), conflict-checked, written atomically, backed up and logged
  on the broker console; the broker reloads them. See **mads-broker**(1),
  REMOTE EDITING, **mads-ini**(1) and CRYPTO.md.
- **Optional password.** `mads ini --set-password [FILE]` stores a verifier in
  `<settings>.edit_password` (or `[broker] edit_password_file`);
  `--remove-password [FILE]` deletes it. The password is a second factor with
  CURVE; without `--crypto` it is the only check and the contents then travel
  in clear (development only). The password is never sent or stored: the
  exchange is a challenge-response with single-use nonces, replies are signed,
  and repeated failures lock password logins for 30 s. The editor can
  remember a derived secret for one broker ("Remember on this computer").

### Command line

- **`mads --keypair` writes the secret key owner-only** (mode 0600) and
  explains where the editor's `.pub` goes on the broker host.
- **`mads-top` and `mads-echo` work without a `[top]`/`[echo]` section**
  (typically with a broker found with `-r/--room`, or with `-s`): they
  subscribe to every topic.
- **`mads plugin --zero-copy`** adds the P9 shared-memory hooks (with the
  contract in comments) to a generated C++ plugin. See **mads-plugin**(1).

### Releases

- **Beta pre-releases and `mads beta`.** Tags of the form `vX.Y.Z-betaN` are
  published as GitHub pre-releases (never "latest"). `mads beta` lists the
  beta releases of the next version (newer than the latest stable one), newest
  first, with publication date and the download links for this platform.
  `mads update` is unchanged.
- **The Android package is a plain SDK archive**,
  `Mads-<version>-Android-<abi>.tar.gz` with `lib/`, `include/` and the zmq
  license at its root (it used to be a `.deb` with a bogus architecture).

## Fixes

- **The broker reloaded its settings file only until the first "atomic"
  save.** inotify (Linux) and kqueue (macOS) follow the inode, and editors
  (and `mads ini`) replace it by renaming. The file watcher now re-arms on the
  path (also used by `mads top`), ignoring late events of the replaced watch.
- **The broker's daemon mode with `--crypto` no longer hangs on SIGTERM**: the
  ZAP handler is released before the context is closed.
- **A changed `attachment` path is used on settings reload** without
  restarting the broker.
- **The broker's N key erases only the settings line** again. It
  moved the cursor a fixed 5 lines up, which broke once the broker printed two
  settings lines (tcp and ipc) or log lines from other threads. The bottom of
  the screen is now a fixed block, and log lines go through one locked helper.
- **`mads --keypair` no longer leaves the secret key world-readable** under the
  default umask.
- **`Agent::last_topic()` is the topic of any received message.** It used to
  follow JSON messages only, so the filter anti-loop guard and the blob/sink
  paths compared against a stale topic for blob inputs (and never saw
  `control` messages).
- **The `-o shm_slots`/`shm_slot_size` options of the plugin loaders are
  validated** like the settings keys; a non-integer value gives a clean error
  instead of an uncaught exception. A plugin that fails to load now reports the
  reason (e.g. a protocol mismatch).
- **Hardening of the new ipc/shm paths:** `ip_whitelist` is applied to every
  peer that is not provably an ipc peer (link-local IPv6 such as `fe80::1%en0`
  is no longer skipped); an explicit `ipc_dir` gets the same ownership check and
  mode 0700 as the default (a directory owned by someone else disables ipc with
  a warning); published shm payloads stay valid across idle loop passes; ipc
  sockets are detected on Windows (AF_UNIX reparse points).
- **The editor reports parse errors of the current text**, not of the text as
  loaded; Save As of a remote document no longer proposes the endpoint as file
  name.

## For developers

- **Plugin protocol P9** (`mads_plugin` tag `v2.6-P9`). New virtual methods with
  copying defaults, so P8 sources compile unchanged: `Filter::load_data_view()`
  and `Sink::load_data_view(data, topic, BlobView)` (read-only view of the
  producer's bytes, valid only during the call and, for filters, the following
  `process()`), `Source::supports_shared_output()`/`get_output_shared(out,
  buf)` and `Filter::supports_shared_output()`/`process_shared(out, buf)`
  (write at most `buf.capacity` bytes into `buf.data`, set `buf.size`; on
  overflow set the required size and return `return_type::retry`, the host
  falls back to `get_output()`/`process()` in the same iteration). Shared output
  is used only with `shm = true`. Templates, the `mads-plugin` skill
  and `mads plugin --update` (migration `share/plugin_migrations/P8-P9.json`)
  follow P9. Loaders check the driver version before `create()`.
- **Agent API** (additive): `Agent::set_transport_mode()`, `transport_mode()`,
  `transport()`; `enable_shared_output(slots, slot_size)`, `acquire_slot()`,
  `publish_slot()`, `abandon_slot()` (nothing written: the slot's previous
  payload stays valid), `discard_slot()` (bytes may be written);
  `accept_shared_views(true)` to receive `message_type::shared` and
  `last_shared()`; `shm_failed()`, `shm_torn()`; `Agent::add_default_section()`.
  `AgentApp` gains `--transport`. `receive_raw_message()` materializes shm
  references as blob frames. Shared pieces: `Mads::detail::choose_transport()`
  (used by agents and doctor), `Endpoint` parsing (`tcp`, `ipc`, `inproc`),
  `runtime_dir()`, `host_token`, `config_dir()`, and in `src/shm`
  (`Mads::shm`, not installed) `SharedSegment`, `SharedRing`, `SegmentCache`,
  `scan_registry()`, `sweep_orphans()`.
- **Auth:** `ZapAuth::configure_domain_curve(domain, keys)` gives a ZAP domain
  its own CURVE key set, never checked against the bus allowlist;
  `CurveAuth::load_server_keypair()` and `client_public_keys()`;
  `classify_zap_address()`.
- **`mads_ini_core`** (`src/ini`), a new GUI-free static library: the
  comment-preserving document model (`ini_document`), the key schema
  (`ini_schema.toml`, embedded at build time; node types, section-name aliases,
  linter), `Mads::Ini::Editor`, the `mads ini` command line (`ini_cli`) and the
  remote-edit service and client (`remote_edit`: `EditService`, `serve()`,
  `RemoteSource`). The pure topology module exposes `agent_topic_info()`,
  `topology_edges()` and `dangling_topics()`; `mads doctor --graph` output is
  unchanged.
- **GUI:** `MadsGui::draw_status_footer` takes an optional `line2_color`
  argument (e.g. `kErrorColor`); mads-chat's room discovery moved to
  `src/gui/common/room_discovery.*` and is shared with mads-ini. Per-user
  preferences live in `config_dir()` (`mads-ini.json` is owner-only).
- **Build:** imnodes is a new FetchContent dependency, pinned to commit
  `2d0c280b07b2f62b120034811cc6e41251673961`. `-DMADS_PLUGIN_SOURCE_DIR=<dir>`
  builds against a local `mads_plugin` checkout instead of fetching it. libzmq
  (4.3.5) is patched at fetch time (`vendors/patches`, idempotent) so that
  `ZMQ_SNDBUF`/`ZMQ_RCVBUF` apply to ipc sockets too, until upstream has it.
  Library-only builds (`MADS_BUILD_APPS=OFF`) no longer install app data.
  `.gitignore` keeps the editor's `*.bak` backups and `*.edit_password`
  verifiers out of git.
- **Beta tagging and Debian versions.** Tag `vX.Y.Z-betaN` (N >= 1, e.g.
  `v3.1.0-beta1`): CI builds it and creates a pre-release. Version numbers stay
  `X.Y.Z`, `GIT_TAG` is the beta tag, and commits past it become
  `vX.Y.Z-betaN-p<n>-g<hash>`. The Debian version is `X.Y.Z` for a release,
  `X.Y.Z~betaN` for a beta (sorts before the final release),
  `X.Y.Z+p<n>.g<hash>` past a release and `X.Y.Z~betaN+p<n>.g<hash>` past a
  beta. Package file names still use `GIT_TAG`. Tag parsing lives in
  `src/detail/release_tag.hpp`; the release process is in `MINOR_UPDATE.md`.
- **Tests:** new suites for ipc transport (ports 44600-44699), shared memory,
  the ini document, schema, editor and CLI, remote editing, ZAP domains and
  addresses, config dir, release tags, and topology edges. The two attachment
  tests of `test_agent_broker` no longer share ports (now 42212 and 42213),
  which made `ctest -j` fail at random.
- **Docs:** new man page **mads-ini**(1); ipc, shared-memory and doctor
  sections in the broker, agent, doctor and plugin pages; a remote-editing
  section in CRYPTO.md.

Changes up to v2.5.x: see CHANGES_v2.md.
