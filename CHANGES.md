# Release 2.4.3

This is a bugfix release. Expect it to be fully compatible with previous `v2.4.x` releases.

## Fixes

- **OTA plugin delivery no longer crashes scaled agents.** A broker-served
  plugin was written to one fixed path per settings section,
  `<temp>/mads/<section>.plugin`, with a truncating write. That is fine for a
  single agent and fatal for several: `mads-director`/`mads-up` with `scale`
  greater than 1 start N instances of the same section on one host, and each
  one rewrote the very file its siblings had already `dlopen`'d. Overwriting a
  mapped file in place does not leave the running process with a stale but
  valid copy -- it changes the pages underneath it -- so a sibling died at its
  next page fault, intermittently and in a different process from the one that
  did the writing. The attachment is now cached under a digest of its own
  contents, `<temp>/mads/<section>/<digest>/<section>.<ext>`, and published by
  an atomic rename, so the bytes at a path in use never change. Instances of a
  scaled agent share one copy rather than fighting over it, with no `${ID}`
  templating or `--agent-id` required; a plugin that changes on the broker
  hashes differently, lands on a new path and is picked up at the next launch,
  with superseded copies swept once they are no longer selected. Nothing on the
  wire changed, and existing plugin binaries do not need recompiling.

- **Encryption no longer silently switches off the subscription table.** With
  `[broker] subscription_table = true` and `--crypto` together, the
  `subscriptions` topic was never published: the broker rejoined its own
  frontend over loopback TCP without CURVE credentials, so libzmq dropped it
  during the handshake -- no error anywhere, just a topic that never appeared.
  The table's publisher now joins the frontend over an `inproc` endpoint,
  which carries no handshake at all, so it behaves identically encrypted or
  not (and saves a pointless trip through the loopback interface either way).

- **`mads up` can gate on an encrypted broker.** `ready = "broker"` always
  probed in the clear, so on a fleet started with `--crypto` the readiness gate
  never opened: every dependent process waited out the full timeout behind a
  broker that was up and healthy, and the run died with a bare *"did not become
  ready in time"* that named nothing useful. **mads-up** now takes the same
  `--crypto`/`--keys_dir`/`--key_client`/`--key_broker` flags as every other
  `mads-*` executable, applying them to that probe (managed processes keep
  carrying their own flags in their `command`, unchanged). A broker probe that
  times out now names the encryption mismatch as a possible cause, and
  `--dry-run` marks which probes will run encrypted. Nothing in `director.toml`
  changes -- the plan format stays exactly what Director's GUI reads.

- **`mads doctor --crypto` no longer reports a healthy encrypted broker as
  dead.** The broker-reachability check probed with a plain socket regardless
  of `--crypto`, and a CURVE broker drops a plain peer during the handshake --
  so a perfectly working broker came back as *"did not respond"*, two lines
  above a *passing* CURVE handshake check against the very same endpoint. The
  probe now speaks CURVE when `--crypto` is given, `--graph-live` reads the
  subscription table the same way, and when the check does fail its hint now
  names the encryption mismatch as a possible cause in both directions.

# Release 2.4.2

This document summarizes what changed between `v2.4.1` and `v2.4.2`.

**Breaking for downstream C++ agents that touch ZMQ sockets directly; the wire
protocol is unchanged.** `MadsCore` is a shared library whose class layout
changed, so anything built against `v2.4.1` must be **recompiled**. Set the
release version accordingly before tagging.

## Dependencies

- **ZeroMQ C++ binding: `zmqpp` replaced by `cppzmq`.** MADS now talks to
  libzmq through [cppzmq](https://github.com/zeromq/cppzmq) (pinned to
  `v4.11.0`) instead of `zmqpp`, which has had no release since 2021 and was
  tracked here at a moving `master`. cppzmq is header-only and maintained by
  the ZeroMQ organisation itself.

  **The wire protocol does not change.** Frame layouts, topics and the
  CURVE/ZAP handshake are all libzmq-side, so a migrated agent and a `v2.4.1`
  broker interoperate in both directions (verified in all five
  broker/publisher/subscriber crossings), and `bag` files are unaffected.

  **Not affected, no action needed:** plugins (`.plugin` modules link only
  `pugg` + `nlohmann_json`; the plugin protocol stays at v8), the C API
  (`agent_c.h`), the Rust crate, and **every consumer of `Mads::AgentApp`** --
  its interface never named a ZMQ type.

  **Affected:** a custom agent deriving from `Mads::Agent` that touches the
  sockets directly. `Agent`'s protected members are now `zmq::context_t` /
  `zmq::socket_t` / `zmq::multipart_t`, `receive_raw()` takes a
  `zmq::multipart_t&`, and `agent.hpp` no longer pulls in `<zmqpp/zmqpp.hpp>`.
  For those, add one include:

  ```cpp
  #include <agent.hpp>
  #include <zmqpp_compat.hpp>   // deprecated shim: old zmqpp:: spellings keep working
  ```

  `zmqpp_compat.hpp` re-exposes `zmqpp::context`/`socket`/`message`/
  `socket_type`/`socket_option`/`curve` over cppzmq, deriving from the cppzmq
  types so references bind both ways. Every type is `[[deprecated]]` and names
  its replacement; define `MADS_NO_ZMQPP_COMPAT_WARNING` to silence the
  warnings while porting. **It will be removed in the release after next.**

  Note this is source compatibility only: `MadsCore` is a shared library whose
  class layout changed, so downstream code must be **recompiled** either way.

- **New `Mads::ZapAuth` (`zap_auth.hpp`).** cppzmq has no equivalent of
  `zmqpp::auth`, so MADS now owns its ZAP (RFC 27) handler. `Mads::CurveAuth`
  keeps the same method names and behaviour on top of it. Two improvements
  fall out: `setup_auth()` completes the ZAP bind before returning (libzmq lets
  connections through unauthenticated while no handler is bound), and teardown
  is deterministic rather than relying on zmqpp's process-lifetime actor state.
  `Mads::generate_keypair()` replaces `zmqpp::curve::generate_keypair()`.

## New features

Now that MADS talks to ZeroMQ directly (see "ZeroMQ C++ binding" above), a
number of things that were previously locked away become available. All of
the following are **off by default** — an existing `mads.ini` keeps behaving
exactly as before until you opt in. Details and examples are in
`man mads-broker` / `man mads-doctor`.

- **New tuning options for demanding networks.** A handful of new settings
  let you adapt MADS to networks that aren't a clean lab loopback:
  - `heartbeat_ivl` (and friends) make agents notice a dead or half-open
    connection — an unplugged cable, a firewall silently dropping a stale
    link — instead of assuming everything is fine forever.
  - `reconnect_ivl_max` spaces out reconnection attempts after a broker goes
    down, instead of every agent hammering it with a retry every 100ms and
    then all reconnecting in the same instant once it's back.
  - `immediate` stops an agent from silently piling up messages toward a
    broker it isn't actually connected to yet.
  - `max_msg_size` lets you set a hard cap on incoming message size, as a
    safety net against a misbehaving publisher. Note it disconnects the
    sender rather than just dropping the one oversized message, so pick a
    generous limit.
  - `tcp_keepalive`, `sndbuf`/`rcvbuf` give finer control over long-lived
    connections and high-rate data sources.
  - `[broker] io_threads` lets a busy broker use more than one network
    thread for higher sustained throughput.

- **Fetching settings from the broker no longer waits in line.** Before this
  change, if one agent was downloading a large plugin file from the broker,
  every other agent asking for its settings at the same time had to wait its
  turn. The broker can now field several settings requests at once
  (`[broker] settings_workers`, 2 by default), so a slow request no longer
  holds up everyone else.

- **Agents know when they're actually connected, instead of guessing.**
  Connecting to the broker used to mean "wait a fixed quarter of a second and
  hope." Agents now find out the moment the connection is actually live, so
  startup is both faster in the common case and no less safe in a slow one.

- **`mads top` now tells you whether the broker is still there.** An empty
  table used to be ambiguous: it looks exactly the same whether nobody is
  publishing or the broker has died. The header now carries a `link:`
  indicator taken from the connection itself rather than guessed from
  traffic, so you can tell those apart at a glance -- `link: up`,
  `link: DOWN for 12.4s`, or, if the broker refused your encryption key,
  `link: DOWN (broker rejected our key)`. A connection that keeps dropping
  and coming back -- a flaky cable or an overloaded broker, which at any
  single moment looks perfectly healthy -- shows as `link: up (2 drops)`.

  Custom agents can read the same information through the new
  `Agent::link_state()`, which reports whether the link is up, since when,
  why it went down, and how many times it has dropped and recovered. (This
  replaces `Agent::last_link_event()`, added earlier in this same unreleased
  cycle and never part of a release.)

- **`mads doctor --crypto` gives a real answer for CURVE problems.** Testing
  an encrypted setup used to report the same generic timeout whether the
  broker was down, misconfigured, or your key was simply rejected. It now
  performs a real handshake and tells you plainly when the broker has
  rejected your key, rather than leaving you to guess.

- **Optional live view of who is subscribed to what.** Turn on
  `[broker] subscription_table` and the broker publishes, once a second, a
  running count of subscribers per topic — handy for seeing at a glance
  whether the agents you expect are actually listening. Left off by default
  because watching every subscription does add some overhead.

- **Agents run fewer threads.** Every connected agent used to spend one
  background thread per socket it watched, on top of the one it may already
  use to receive. Those are now a single thread that watches everything at
  once. Measured on a real publish-and-subscribe agent: 6 threads before, 5
  after -- and 7 before, 5 after for one using last-value delivery. Nothing
  changes in what an agent does; it just costs less to run a lot of them on
  one machine.

- **A worker with nothing to do no longer ignores everything else.** A
  `mads-worker` waiting for jobs used to wait *only* for jobs: while its
  queue was empty it stopped reading anything else, so a fleet-wide
  shutdown or restart sent through the broker simply never reached it, and
  neither did the topics it had subscribed to. It now checks its job queue
  with a deadline and gets back to its other duties in between.

  **If you have written your own agent on top of `Mads::Worker`:** `pull()`
  can now return an empty object, meaning "no job this time", instead of
  waiting forever for one. Skip the rest of your loop body when it does.
  `pull(timeout)` lets you choose the wait yourself; `pull(0ms)` never
  blocks at all.

- **`mads doctor --graph-live`: compare your settings against reality.**
  `mads doctor --graph` has always drawn what `mads.ini` *declares*. Adding
  `--graph-live` overlays what the fleet is *actually doing*, read from the
  broker's live subscription table, so three situations that used to look
  identical on the graph can be told apart at a glance:
  - declared, and someone is really subscribed → `topic [live 2]`
  - declared, but nobody is listening → `topic [offline]`
  - **someone is listening to a topic nobody declared** → drawn as a separate
    orange node, with arrows from whichever publishers that listener is
    actually receiving from. This case was previously invisible: a
    settings-only graph had no way to show it at all.

  Two honest limits, both spelled out in `man mads-doctor`: the underlying
  data is anonymous (it can say *how many* subscribers a topic has, never
  *which* agents), and it says nothing about publishers, so a source is never
  reported as offline. If the broker isn't running with
  `subscription_table = true`, `--graph-live` fails with a clear error rather
  than quietly drawing a declared-only graph — which would otherwise look
  exactly like a successful live check. Plain `--graph` still works with no
  broker at all.

- **Bug fix: agents combining "last known value" delivery with remote
  control could occasionally misroute messages.** An agent configured for
  both LKV delivery (`queue_size = 1`) and remote control on its own thread
  had a subtle race: a shutdown/restart command could, in rare cases, be
  mistaken for ordinary data instead of acted on, or an ordinary message
  could be dropped instead of updating the latest value. Both jobs are now
  handled by a single, well-ordered thread, so this can no longer happen —
  and every agent doing both now runs with one fewer background thread than
  before, at no cost to correctness (verified with a fast producer/slow LKV
  consumer under repeated load).

- **`mads up` honours `director.toml`'s `base_instance_id`.** `mads-up` now
  supports the per-process `base_instance_id` key that `mads_director` v2.4.2
  added (the version this repo pins), with identical semantics: it offsets the
  value `${ID}` expands to in `command`, so a section with `scale = 3` and
  `base_instance_id = 10` starts three instances whose `${ID}` is 10, 11 and
  12 instead of 0, 1 and 2. It shifts `${ID}` only -- the instance names stay
  `name[1]`..`name[3]`, so `after` targets and the `[name]` output prefixes are
  unchanged. The key is optional and defaults to `0`, so every existing
  `director.toml` expands exactly as before; a negative value is rejected at
  load time. The same file therefore scales identically under the Director GUI
  and under `mads up` / `mads doctor --plan`.

## Fixes


- **Broker `p`/`r` keys now actually pause and resume.** The interactive
  broker's pause/resume keys had inverted effects. The cause is a bug in
  libzmq 4.3.5 itself (`src/proxy.cpp`: the `PAUSE` arm is missing its `0 ==`
  and compares six bytes against a five-byte command, so it always matches and
  selects `active`, while `RESUME` selects `paused`) -- not in zmqpp, which an
  older code comment blamed. The broker now sends the command that produces
  the intended effect, and `tests/test_broker_steering.cpp` pins the observed
  semantics so a future libzmq upgrade that fixes this upstream fails the
  build instead of silently re-inverting the keys.


---

# Release v2.4.1

This document summarizes what changed between `v2.4.0` and `v2.4.1`. This is a
patch release: `LIB_VERSION_CHECK` stays `2.4`, nothing in the wire protocol or any
existing public API signature changed, and everything below is purely additive.
Anything compiled against `v2.4.0` compiles and runs unmodified against `v2.4.1`.

# Changes

These changes summarize what was added or updated since `v2.4.0`.

## New features

- **`mads up` headless `director.toml` executor.** New `mads-up` command that
  runs the same `director.toml` process plan used by `mads_director`'s
  interactive GUI, unattended -- develop an agent set in the Director GUI,
  commit the resulting `director.toml`, then run it in CI, on an edge box, or
  under `systemd Type=simple` with `mads up`. It parses/validates/expands the
  file itself (no GLFW/GUI dependency), honouring Director's own `scale`
  expansion, `${PWD}`/`${ID}` templating and `after` dependency ordering (a
  forest, with cycles rejected), plus a new additive, GUI-ignorable `ready =
  "broker" | "port:<n>" | "log:<regex>" | "delay:<duration>"` key that gates
  starting a process's dependents on an actual readiness signal instead of a
  fixed sleep. Processes are spawned via the vendored `reproc` library in their
  own process group (POSIX: `setpgid`/`killpg`; Windows:
  `CREATE_NEW_PROCESS_GROUP`/ `CTRL_BREAK_EVENT`), so descendants are torn down
  too, not just the immediate child; `relaunch = true` processes restart with
  exponential backoff (100ms-30s, capped further by `--max-restarts`).
  `--until-exit <name>` runs until a named process exits and propagates its exit
  code (the CI primitive: bring up broker + pipeline, run a test agent, exit);
  `--timeout`, `--grace`, `--no-shell` and `--dry-run` (print the fully expanded
  plan without spawning anything) round out the flags. Foreground-only by
  design: no daemonization, no PID file, no `mads down`. See
  [share/man/mads-up.md](share/man/mads-up.md).
- **`mads doctor` health check.** New `mads-doctor` command, MADS's answer to
  `ros2 doctor`/`brew doctor`: a single command that checks the things most
  likely to trip up the first hour of a new deployment and reports each as
  `[PASS]`/`[WARN]`/`[FAIL]` with a one-line fix hint. It checks that the
  settings file is found and parses as valid TOML; that the broker's settings
  endpoint is reachable (reusing the same `Mads::probe_broker()` helper `mads
  up` uses); that every plugin declared via an `attachment` key (or given
  explicitly with `--plugin`) resolves and dry-run loads through the same
  `pugg::Kernel` path `mads-source`/`-filter`/`-sink` use, without ever calling
  `process()`/`get_output()`/ `load_data()`; that a loaded plugin's protocol
  matches the version pinned in `share/plugin_deps.json`; that CURVE key files
  (when `--crypto` is given) exist and decode as valid Z85 keys; and that the
  broker's configured frontend/backend/settings ports are not already bound
  locally, to catch an already-running broker early. `--plan <director.toml>`
  delegates to the same `src/director_config.hpp` parse/validate/expand pipeline
  `mads up --dry-run` uses and prints the same kind of expanded-plan report, so
  a whole deployment can be sanity-checked before it is launched for real.
  `--fix` scaffolds a missing settings file from the same template `mads ini`
  renders, and only ever creates a missing file -- it never deletes or
  overwrites one. `--graph [file.dot]` renders the settings file's declared
  pub/sub topology as Graphviz DOT text (to the given path, or stdout) -- one
  record-shaped node per agent section listing its `sub_topic` entries, one edge
  per matching `pub_topic`/`sub_topic` pair, nodes colored by inferred role
  (source/filter/sink), and a dashed contour on any agent with a dangling topic
  -- a `pub_topic` nobody subscribes to, or a `sub_topic` nothing ever publishes
  to, with the offending entry marked `[!]` in the node itself. Edges are
  computed with the new `Mads::subscription_match()` (see the wildcard entry
  below), i.e. exactly what the agent would receive on the wire -- literal
  `sub_topic` entries match by raw ZeroMQ byte prefix, wildcard ones by MQTT
  rules -- and *how* each edge came to exist is drawn into it: solid for an
  exact match, dashed for a literal prefix catch, dotted for a wildcard, with
  every non-exact edge naming the responsible pattern(s) under the published
  topic. Sections with no `pub_topic` key still publish under their own name,
  exactly as `Agent` itself defaults, so their edges are drawn too, in grey.
  Edges reaching a subscribe-all (`sub_topic = [""]`) subscriber through nothing
  but its `""` entry are collapsed into a count on that node's `(all)` line,
  since they match every publisher by definition and otherwise bury the wiring
  the graph is read for; `--graph-fanout` draws them anyway. No new dependency:
  `mads doctor` only ever writes DOT text, never shells out to `dot`. See
  [share/man/mads-doctor.md](share/man/mads-doctor.md).
- **`mads echo` / `mads top`.** Two new, purely read-only CLI subcommands for
  peeking at live traffic without writing an agent or touching `mads.ini` -- the
  MADS analogue of `ros2 topic echo`/`rostopic echo` and `htop`. `mads echo
  [topic ...]` subscribes as an ephemeral sink agent and pretty-prints each
  message (topic, timestamp, size, rang-colored JSON, or a one-line blob
  summary), with `--raw` (exact bytes, base64, for blobs), `--count N` (exit
  after N messages) and `--jsonl` (one compact JSON line per message, for piping
  into `jq`). `mads top [topic ...]` is a live, redrawn-in-place table of active
  topics (msg/s, bytes/s, last-seen age, last-payload sample), aggregated over a
  sliding window (`--window`, default 5s) and redrawn every `--sample-rate`
  seconds (default 1s); press `q` or Ctrl-C to quit. Both work with zero
  `mads.ini` setup by default (`--broker` points directly at the broker's
  subscribe endpoint) or via the normal `-s/--settings` section-based path other
  `mads-*` executables use, and both accept MQTT-style topic filters
  (`sensors/+/x`, `sensors/#`) via the wildcard matcher below --
  `Agent::set_sub_topic()` already applies it end to end, so neither command
  needed any filtering logic of its own. See
  [share/man/mads-echo.md](share/man/mads-echo.md),
  [share/man/mads-top.md](share/man/mads-top.md).
- **MQTT-style topic wildcards.** `sub_topic` entries can now use `+` (exactly
  one topic level) and `#` (this level and everything below it, including the
  level it replaces, e.g. `sensors/#` also matches the bare topic `sensors`;
  only legal as the final token) alongside plain literal topics. This is
  non-disruptive by construction: a `sub_topic` entry with no wildcard character
  subscribes exactly as before (identical ZMQ `SUBSCRIBE` frame, zero added
  overhead); only entries containing `+`/`#` take a two-stage path -- the
  broader literal prefix is subscribed at the ZMQ layer, then
  `Mads::topic_match()` filters each arriving message before it reaches
  `receive()`/callbacks, silently dropping non-matches. The matcher
  (`Mads::topic_match()`/`Mads::literal_prefix()`, `src/topic_match.hpp`) is a
  pure, dependency-free function, independently unit-tested. The two stages
  together -- byte prefix for literal entries, MQTT matching for wildcard ones
  -- are also exposed as a single `Mads::subscription_match()` predicate, which
  `Agent` and `mads doctor --graph` both build on, so what the graph draws and
  what the wire delivers cannot drift apart. See the "Settings model" section of
  [CONTEXT.md](CONTEXT.md).
- **`mads-record`/`mads-play`/`mads bag`: bag record & replay.** `mads-record`
  subscribes per `sub_topic` (MQTT-style filters) and writes every message to a
  bespoke binary bag file (`src/bag.hpp`) that stores the exact multi-part wire
  frame byte-for-byte, so JSON and binary blob messages both round-trip
  identically. A trailing index/footer gives O(1) `mads bag info` and fast
  seeking; if the writer is killed before it can write that footer, the reader
  falls back to a linear scan that recovers the complete valid prefix and
  reports it as truncated, rather than failing outright. Per-record CRC32 is on
  by default. `mads-play` reads a bag and republishes it via two new, purely
  additive `Agent` methods -- `receive_raw_message()`/`publish_raw_message()` --
  that expose the raw topic+parts frame with no JSON (de)serialization, so
  replay never re-parses JSON or re-copies blob bytes; `--topics` filters the
  replayed subset via the same `Mads::topic_match()` matcher, `--rate` paces
  replay using the recorded gaps between timestamps, and `--restamp` optionally
  rewrites the timestamp/timecode fields of plain compressed JSON frames to
  "now". `mads bag info`/`mads bag export --format jsonl` inspect a bag file
  directly (`--format mcap` is deferred as a possible future export target, not
  implemented here). See [share/man/mads-record.md](share/man/mads-record.md),
  [share/man/mads-play.md](share/man/mads-play.md),
  [share/man/mads-bag.md](share/man/mads-bag.md).

## Bug fixes

- **OTA plugin delivery (the `attachment` INI key) stopped working.** Fixed in
  `mads-source`/`-filter`/`-sink` and `mads-rsource`/`-rfilter`/`-rsink`: the
  broker-served attachment is only known after `Agent::init()` runs, but a
  recent change moved plugin loading to *before* `init()` so a plugin's own
  `kind()` could pick its settings section -- so the loaders' attachment check
  always saw an empty path and silently fell back to the compiled-in default
  plugin. Fixed by splitting `Agent::init()`'s settings-acquisition step out
  into a new, idempotent `Agent::fetch_settings()` (and an `AgentApp`-level
  wrapper of the same name): the loaders now resolve the settings section from
  CLI precedence alone (`-n`/`--name`, else the `--plugin` stem, else the
  compiled-in default), fetch settings under that name -- which also pulls
  down any served attachment -- and only then resolve which plugin file and
  which driver inside it to load. A loaded plugin's self-reported `kind()` is
  now a mismatch warning rather than a section selector, since the section has
  to be fixed before the plugin can be fetched at all.
  As part of the same fix, plugin file, driver and settings section are now
  three independent, symmetric choices instead of being conflated through the
  file name: a new `driver` INI key / `--driver` CLI flag (falling back to the
  plugin file's stem, as before) selects which driver to instantiate from a
  plugin file that registers more than one, and lets an OTA-delivered
  plugin's driver name differ from the settings section that fetched it --
  needed since the OTA temp file is always named after the section, not the
  driver. `mads doctor`'s plugin dry-run check now honours the same `driver`
  key when validating a declared `attachment`. See the "OTA Plugins" section
  in `man mads-source`/`mads-filter`/`mads-sink` for the full precedence
  rules and a worked multi-architecture example.
- **`Mads::Runtime::process_running()` was silently disconnected across DLL
  boundaries on Windows.** The process-wide run flag was a function-local
  `static` defined inline in `mads.hpp.in`; on Windows, an inline function-local
  static gets a separate instance in every DLL/EXE that includes the header (no
  automatic weak-symbol merging across PE images, the way ELF has). A
  caller-side `stop_process()` invoked from outside `MadsCore.dll` would then
  silently target a disconnected flag that `Agent::loop()` (compiled inside the
  DLL) never observed, so a process-wide stop never actually stopped anything on
  Windows. Fixed by defining it out-of-line, once, in the new `src/mads.cpp`,
  compiled into `MadsCore` itself. Also fixed as part of the same pass: a
  system-wide MADS install could leave a stale `MadsCore.dll` on `PATH` that the
  Windows loader picked up ahead of a freshly built one, failing test runs with
  `STATUS_ENTRYPOINT_NOT_FOUND` --- the test suite now prepends the freshly built
  DLL's directory to `PATH` for each child test process via a small wrapper
  script (`tests/win_test_wrapper.ps1`, hooked in through Catch2's
  `CROSSCOMPILING_EMULATOR`).

## Note

- **The MQTT bridge agent currently on branch `feat/P6` is intentionally not
  part of this release.** A working, tested implementation exists on the
  `feat/P6` branch (gated behind a new, off-by-default `MADS_ENABLE_MQTT` CMake
  option), but it is being held back and evaluated for extraction into its own
  repository -- an agent linking against the installed `libMadsCore` SDK, the
  same way `mads_director` already lives outside this monorepo -- rather than
  growing MADS core's vendored footprint for an optional dependency. See
  `NEW_FEATURES.md` for the full rationale.

---

# Release v2.4.0

This document summarizes what changed between `v2.3.1` and `v2.4.0`.

To **install this version**, have a look at [the Guide](https://mads-net.github.io/guides/install.html)

# Changes

These changes summarize what was added or updated since `v2.3.1`.

## New features

- **`mads-federate` agent.** New agent that relays selected topics between two
  independent MADS networks, each with its own broker. It owns two ordinary
  agent connections ("side A" and "side B"), each configured via a normal
  `[name]` section in that network's own broker settings; whatever a side
  subscribes to via its `sub_topic` is forwarded to the other network. Only JSON
  messages are relayed (no blobs), and the `control`/`agent_event` topics are
  never forwarded, so remote-control commands and lifecycle events stay local.
  Relayed messages are tagged with the relay's id in a `mads_relay_path` field
  to prevent A→B→A loops. See
  [share/man/mads-federate.md](share/man/mads-federate.md).
- **High-resolution loop pacing.** `Agent::loop()` now paces its internal timing
  in nanoseconds instead of milliseconds (existing millisecond-based code keeps
  compiling and behaving as before). Agents can set a `time_step_us` key in
  their settings section (wins over `time_step`) for microsecond-granularity
  periods, and opt into `enable_high_res_loop()` (or the
  `high_res_loop`/`spin_margin_us` settings keys) to busy-spin the tail of each
  interval instead of sleeping through it, trading CPU time for
  microsecond-accurate wake-up timing.
- **`mads plugin --update` migration command.** C++ plugins can now be migrated
  in place to the current plugin protocol version instead of being rewritten by
  hand. It detects the plugin's current protocol from its `CMakeLists.txt`,
  chains the migration steps recorded in `share/plugin_migrations` up to the
  current protocol, bumps the pinned `GIT_TAG`, and rewrites affected method
  signatures (located structurally, so reformatted/multi-line declarations
  migrate correctly). Each modified file is saved as `*.bak`, and a change
  report plus manual follow-up checklist is printed; unless `--no-check`, the
  migrated plugin is then compiled so the compiler flags anything the rewrite
  could not handle. `--dry-run`, `--from`, and `--to` are also available. Rust
  plugins are unaffected (they track the `mads-plugin` crate version in
  `Cargo.toml`). See [share/man/mads-plugin.md](share/man/mads-plugin.md).
- **`plugin_deps.json` dependency manifest.** Scaffolded C++ plugins now pin
  their dependency versions (plugin protocol, `mads_plugin`/`pugg` git tags,
  bundled `nlohmann::json` version) from a single `share/plugin_deps.json` file,
  which also supplies the default target protocol for `mads plugin --update`,
  instead of being hard-coded into the CMake template.
- **Run-state control from C and Python.** The C API gained `agent_stop()`,
  `agent_running()`, `mads_stop_process()`, and `mads_process_running()`, backed
  by a new public `Agent::running()` accessor (the exact condition
  `Agent::loop()` checks). The Python wrapper mirrors them as `Agent.stop()`,
  the `Agent.running` property, and module-level
  `stop_process()`/`process_running()`, so C and Python agents can drive and end
  receive loops cleanly instead of relying on signals.
- **Stoppable settings watcher.** `Mads::Watcher::watch()` can now be ended with
  `stop()` (or by a process-wide stop request) instead of looping forever; all
  platform waits are bounded so a stop is noticed within about a second. The
  broker now joins its settings-watcher thread on shutdown instead of leaking
  it.
- **`Mads::Runtime`.** Agent run state (the "keep going" flag consulted by
  `Agent::loop()`, the last-known-value drain thread, and the threaded remote
  control) is now owned by a `Mads::Runtime` object instead of a single
  process-global flag. Each `Agent` gets its own `Runtime` by default, so
  stopping or destroying one agent no longer stops the loops of other agents
  hosted in the same process, and a disconnected agent can reconnect and loop
  again. Agents that should stop together can share a `Runtime` via
  `Agent::set_runtime()`; `Mads::Runtime::stop_process()` (used by the
  SIGINT/SIGTERM handlers and the remote `shutdown`/`restart` commands) still
  stops every agent in the process. The previous `Mads::running` flag remains
  available as a `[[deprecated]]` alias of the process-wide state, so existing
  code keeps compiling and behaving as before, with a compile-time warning
  pointing at the replacement API. See [RUNTIME.md](RUNTIME.md).
- **Unit test suite and code coverage.** MADS gained an in-tree Catch2-based
  unit test suite (`tests/`, built with `-DMADS_BUILD_TESTS=ON`) covering the
  core library — settings loading, the broker request protocol, pub/sub and the
  wire codec over loopback ZeroMQ, the C ABI, CURVE key handling, and the plugin
  migration engine — with no external broker, database, or network access
  required. A `-DMADS_COVERAGE=ON` CMake option instruments the build for
  `gcovr`/lcov-style reports, and a new GitHub Actions workflow
  (`.github/workflows/coverage.yml`) runs the suite and uploads results to
  [codecov.io](https://codecov.io/gh/pbosetti/MADS) on every push.

## Improvements

- **Updated to `mads_plugin` v2.4-P8 (protocol P8) and `pugg` 1.2.0.** Plugin
  registration is simplified: the per-type
  `INSTALL_SOURCE_DRIVER`/`INSTALL_FILTER_DRIVER`/`INSTALL_SINK_DRIVER` macros
  are replaced by a single type-deducing `MADS_REGISTER_PLUGINS(klass, ...)`
  macro that can register several plugins from one library. Plugin child-class
  method signatures
  (`kind`/`load_data`/`process`/`get_output`/`set_params`/`info`) are unchanged
  from P7. The generated driver's `create()` now returns a `std::unique_ptr`
  rather than a raw pointer. A migration step (`P7-P8.json`) is provided for
  `mads plugin --update`.
- **More robust `GIT_TAG` rewriting during plugin migration.** The CMake
  `GIT_TAG` bump used by `mads plugin --update` no longer relies on a single
  fragile regex spanning the whole `FetchContent` block (which could match a tag
  mentioned in a comment, or mis-substitute a replacement value starting with a
  digit). It now locates the block header and then tokenizes it with a small
  CMake-aware scanner that honours `#` comments, quoted arguments, and nested
  parentheses, replacing exactly the `GIT_TAG` value token by position.
- **gv2fsm updated to v2.0.0.** The bundled finite-state-machine generator is
  updated to its latest release.

## Bug fixes

- **Startup events could crash short-lived agents.**
  `Agent::register_event(event_type::startup)` published from a detached thread
  that slept 500 ms while holding a raw pointer to the agent; destroying the
  agent within that window was a use-after-free (any events-enabled `AgentApp`
  that exits quickly). The thread is now owned by the agent, wakes early on
  shutdown/disconnect, and is joined while the sockets are still open. Sends on
  the publisher socket are also serialized with a mutex, closing a pre-existing
  data race between the delayed event thread and the owner thread (ZeroMQ
  sockets are not thread-safe).
- **`Agent::query_broker()` could hang forever on shutdown.** The REQ socket
  used to fetch settings from a broker had no linger timeout; if the broker was
  unreachable, a queued but undelivered request kept the ZeroMQ context alive
  and `Agent::shutdown()` blocked indefinitely in context termination. The
  socket now sets `linger = 0`.
- **C API CURVE key setters could crash instead of returning an error.**
  `agent_set_client_public_key`/`agent_set_client_secret_key`/`agent_set_server_public_key`
  guarded against being called before `agent_setup_crypto()` by checking the
  address of the internal `CurveAuth` pointer, which is never null; calling any
  of them too early dereferenced a null `CurveAuth` and crashed the process
  instead of returning `-1` as documented.

---

# Release 2.3.1

This document summarizes what changed between `v2.3.0` and `v2.3.1`.

This is a purely bugfix release, fully backward compatible. Fixed bugs:

* fixed `mads update` command on Linux (previously showing an unmanaged error message);
* fixed `mads package --install` command, which previously was failing with some packages like `mads-python`;
* fixed Rust plugin loader;
* Update to pugg 1.1.0 and mads_plugin v2.3-p7 (backward compatible with existing `.plugin` files);
* improved `mads inspect_plugin` command to detect `nlohmann::json` version mismatches between the plugin and the loader;
* Removed some harmless compilation warnings.

# Release v2.3.0

This document summarizes what changed between `v2.2.0` and `v2.3.0`.

To **install this version**, have a look at [the Guide](https://mads-net.github.io/guides/install.html)

> **License change:** this release switches the project license from CC-BY-SA 4.0 to the **Apache 2.0** license.

# Changes

These changes summarize what was added or updated since `v2.2.0`.

## New features

- **Rust plugin support.** MADS plugins can now be written in Rust — no C++ toolchain required. Three dedicated loaders (`mads-rsource`, `mads-rfilter`, `mads-rsink`) dynamically load Rust-compiled shared libraries via a stable C ABI (`mads_rust_plugin_t` vtable, exported as `mads_rust_plugin_register()`). Plugin authors implement one of the three Rust traits (`SourcePlugin`, `FilterPlugin`, `SinkPlugin`) from the new `mads-plugin` crate (installed at `share/rust/mads-plugin`) and export it with the corresponding `export_*_plugin!` macro. Panic safety across the FFI boundary is guaranteed via `std::panic::catch_unwind`. See [rust/README.md](rust/README.md) for the full trait reference, return codes, output types, and packaging guide.
- **`mads plugin --rust` scaffolding.** The `mads plugin` command accepts a new `--rust` flag that generates a ready-to-build Cargo project (`Cargo.toml`, `src/lib.rs`, `README.md`) instead of a CMake/C++ stub. The `--install-dir` option writes the local path to the `mads-plugin` crate into `Cargo.toml`. The `--datastore` flag is ignored when `--rust` is set.
- **`mads setup-python` command.** New `mads` sub-command that installs (or updates) the Python ctypes agent wrapper (`mads_agent.py`) into a target Python environment, removing the need to locate the file manually.
- **`mads package --json` option.** Both `mads package list` and `mads package info` accept `-j`/`--json` to emit machine-readable JSON instead of the formatted table, enabling scripting and tooling integration.
- **Mongo replay `unwrap_original` parameter.** The `mongo_replay` plugin gained an `unwrap_original` boolean parameter. When true, the outer envelope added during MongoDB logging is stripped and the original payload is re-published directly, simplifying replay-based testing.
- **MADS Director re-enabled.** The optional GUI process supervisor (`mads-director`) is back in the default build and updated to v2.2.0. It provides a graphical interface for starting, stopping, and monitoring agent processes.

## Improvements

- **Broker daemon-mode resilience.** When running in daemon mode, the broker now continuously retries the mDNS/DNS-SD advertising service after failures instead of giving up on the first error. This prevents silent loss of auto-discovery in environments where the network interface comes up after the broker starts.
- **Plugin loader settings re-application.** The settings override mechanism in `mads-source`, `mads-filter`, and `mads-sink` now re-applies core agent settings (e.g. `sub_topic`, `queue_size`) after user-supplied overrides so that built-in parameters are always honoured correctly, even when the plugin's settings section also provides them.
- **`mads update` improvements.** The self-update command received more robust error handling, better progress feedback, and improved version-detection logic for pre-release builds.
- **Package installation guard.** `mads package install` now refuses to overwrite files that are already present at the destination and reports the conflict clearly. A `--force` flag overrides this behaviour when an explicit overwrite is intended.
- **pugg updated to 1.0.4.** The bundled plugin kernel is updated; the change is backward-compatible with existing `.plugin` files.
- **gv2fsm version update.** The bundled finite-state-machine generator is updated to its latest release.
- **macOS: multiple CPack generators.** The macOS CI build now produces packages via multiple CPack generators (STGZ + productbuild) in a single pass.

## Bug fixes

- **Rust loader compile error on macOS/Linux.** `rust_plugin_loader.cpp` failed to build on Clang and GCC because `return_type` is defined inside the generated pugg plugin headers that the Rust loader intentionally does not include. Fixed by defining a self-contained `enum class return_type` from the `MADS_*` constants already present in `mads_rust_plugin.h`.

## License

- Project relicensed from CC-BY-SA 4.0 to **Apache License 2.0**. The full license text is in the `LICENSE` file.

---

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
