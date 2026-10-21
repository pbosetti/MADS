
# NAME

**mads-broker** - The broker for the MADS network

# SYNOPSIS

**mads-broker** [**\-s, \-\-settings** *arg*] [**\-r, \-\-room** [*room*]] [**\-d, \-\-daemon**] [**\-\-crypto[=arg]**] [**\-\-keys_dir[=arg]**] [**\-v, \-\-version**] [**\-h, \-\-help**]

# DESCRIPTION

**mads-broker** is the broker for the MADS network. It is a server that listens for incoming connections from sources and sinks and routes messages between them. It also provides a centralized INI file for configurating each agent/client.

The broker will watch for changes to the INI file, automatically reload it, and provide updated settings to newly connecting agents. Agents already running will only update the settings upon relaunch.

Note that if you change broker's settings in the INI file, then you want to also relaunch the broker itself.

# OPTIONS

**\-s**, **\-\-settings** *arg*
:  specify the settings file to be used. If not specified, the default settings file is used (usually /usr/local/etc/mads.ini).

**\-r**, **\-\-room** [*room*]
:  Broadcast the broker settings port in the named service-discovery room. If *room* is omitted, the default room is **mads**. Agents use the same option to explore a room and discover the broker settings port. This discovery path is opt-in for agents: if they are not launched with **\-\-room**, the usual settings lookup via **\-\-settings** or the compiled default settings URI is used.

**\-d**, **\-\-daemon**
:  run the broker as a daemon. This suppress the output to the console upon launch and the interactive behavior.

**\-\-crypto[=arg]**
: enable CURVE cryptography. The optional argument sets the name of the keys to be used for encryption (default to `broker`).

**\-\-keys_dir[=arg]**
: the path of the directory where to search for key files. Default to the `etc` directory under the mads prefix directory (`mads -p`).

**\-v**, **\-\-version**
: show version information.

**\-h**, **\-\-help**
:  show summary of options.

# SETTINGS

These INI keys tune the underlying libzmq sockets and are all optional: an
unedited settings file behaves exactly as before they existed.

**io_threads** (`[broker]`, integer, default `1`)
:  Number of libzmq I/O threads for the broker context. The broker forwards
   *all* fleet traffic through this pool, so upstream's rule of thumb of
   roughly one I/O thread per gigabit of sustained throughput applies. A
   value below 1 is clamped to 1 with a warning rather than refused.

**tcp_keepalive**, **tcp_keepalive_idle**, **tcp_keepalive_cnt**, **tcp_keepalive_intvl** (`[agents]`, integer)
:  Enable and tune TCP keepalive probing on a socket, useful for detecting a
   half-open connection (an unplugged cable, a NAT/firewall timeout) on a
   long-lived plant-network link. Unset by default, which leaves the OS/libzmq
   default untouched. May be set fleet-wide under `[agents]` and overridden in
   an individual agent's section; the broker applies the fleet-wide value to
   its own sockets too.

**sndbuf**, **rcvbuf** (`[agents]`, integer, bytes)
:  Kernel socket buffer size, useful for tuning high-rate sources. Unset by
   default, which leaves the OS/libzmq default untouched. Same
   `[agents]`-then-per-agent-override resolution as the keepalive settings
   above. Applies to tcp and, unless overridden by the two keys below, to
   ipc connections too.

**ipc_sndbuf**, **ipc_rcvbuf** (`[agents]`, integer, bytes, default 1048576)
:  Kernel socket buffer size of **ipc://** connections only: the broker's ipc
   endpoints and the agents that connect to them; tcp endpoints of the same
   sockets are unaffected. The OS default for Unix-domain sockets is small (8
   KiB on macOS), which limits a broker hop to about 1.4 GB/s and makes ipc
   slower than loopback tcp for messages of a few hundred KiB and up; 1 MiB
   lifts the limit (about 30 GB/s on the same machine). 0 keeps the OS
   default. Resolved like **sndbuf**; the broker reads `[broker]`, then
   `[agents]`. Needs the libzmq patch MADS builds with (stock libzmq 4.3.5
   applies socket buffer sizes to tcp only).

   On Linux the kernel silently caps every buffer request at
   `net.core.wmem_max` (send) and `net.core.rmem_max` (receive): about 208
   KiB on a stock install, which is also the AF_UNIX default, so there the
   1 MiB default has no effect until the limits are raised, e.g.

       sudo sysctl -w net.core.wmem_max=4194304 net.core.rmem_max=4194304

   (make it permanent with a file in `/etc/sysctl.d/`). macOS caps at
   `kern.ipc.maxsockbuf`, 8 MiB by default, so 1 MiB applies as is.

**heartbeat_ivl**, **heartbeat_ttl**, **heartbeat_timeout** (`[agents]`, integer, milliseconds)
:  Makes libzmq exchange ZMTP `PING`/`PONG` commands, the only
   transport-level liveness MADS has -- without it, a half-open connection
   (an unplugged cable, a NAT/firewall timeout) looks alive indefinitely.
   Heartbeats stay off unless `heartbeat_ivl` is set. `heartbeat_ttl` is
   internally in units of 100ms (libzmq caps it at 6553.5s); pass it here in
   milliseconds like the other two, it is converted for you. Same
   `[agents]`-then-per-agent-override resolution as the keepalive settings.

**reconnect_ivl**, **reconnect_ivl_max** (`[agents]`, integer, milliseconds, defaults `100`/`0`)
:  The default is a flat 100ms retry forever: a fleet hammering a downed
   broker continuously, then thundering-herding on recovery. Setting
   `reconnect_ivl_max` enables exponential backoff up to that ceiling; 30000
   (30s) is a reasonable value.

**immediate** (`[agents]`, boolean, default `false`)
:  Refuses to queue messages toward a peer that is not connected yet, instead
   of buffering them into a pipe that may never drain.

**max_msg_size** (`[agents]`, integer, bytes, default unlimited)
:  Rejects oversized frames at the transport instead of allocating for them.
   **Disconnects the offending peer** rather than dropping just the one
   oversized frame -- for a PUB/SUB agent that means the peer immediately
   reconnects and retries, so this is a defensive limit against a
   misconfigured or hostile publisher, not a per-message filter.

**subscription_table** (`[broker]`, boolean, default `false`)
:  When enabled, the broker publishes a live topic -> subscriber-count table
   on the `subscriptions` topic every second (an ordinary MADS message, e.g.
   readable with `mads-echo subscriptions`), derived from the XPUB backend's
   own subscribe/unsubscribe notifications. **Off by default because it has a
   real cost while running**: it requires wiring a capture socket into the
   broker's proxy, which receives a copy of *every* message the broker
   forwards, not just subscription frames. An unmigrated agent that happens
   to subscribe to `subscriptions` simply ignores the unrecognised topic; no
   settings-contract change is involved. Works the same with **\-\-crypto**: the
   table's publisher rejoins the frontend over an internal `inproc` endpoint,
   which carries no ZMTP handshake, so it needs no CURVE credentials of its
   own. Subscribers still do -- read the topic exactly as any other encrypted
   agent would, e.g. `mads-echo --crypto subscriptions`.

**settings_workers** (`[broker]`, integer, default `2`)
:  Number of worker threads answering settings requests behind a ROUTER
   endpoint (previously a single lockstep REP). The handler reads plugin
   attachments off disk inside the loop, so on a single REP, one agent
   fetching a large attachment blocked every other agent's settings request
   behind it; a worker pool removes that head-of-line blocking. REQ<->ROUTER
   is a standard pairing, so this is invisible on the wire: an unmigrated
   agent's plain REQ settings request works unchanged. A value below 1 is
   clamped to 1 with a warning rather than refused.

**max_open_files** (`[broker]`, integer, unset by default)
:  Open file descriptors the broker process may use, which is what bounds
   fleet size. Every connected agent holds **two** of the broker's descriptors
   for as long as it stays connected -- its publisher on the XSUB frontend and
   its subscriber on the XPUB backend -- plus a third while it fetches its
   settings. With the usual soft limit of 1024 and the broker's own ~30
   descriptors of overhead, that caps a fleet at roughly **490 agents**, and
   libzmq refuses everything past it *almost silently*: its listener treats
   `EMFILE` as a non-fatal `accept()` error. Set this and the broker applies
   the limit at startup, before binding anything, and reports the resulting
   ceiling and the agent count it implies.

   Unset leaves the limit exactly as inherited and only reports it, warning
   when it is low enough to be worth raising. `0` asks for as many descriptors
   as the process is permitted. **Cannot exceed the hard limit** -- a larger
   value is clamped with a warning, because raising the hard limit needs
   `LimitNOFILE=` in the systemd unit or a privileged `ulimit -Hn`, not a
   settings key. A negative value is ignored with a warning rather than
   refused, like `io_threads`.

   A value *below* the current soft limit **lowers** it. That needs no
   privileges -- the soft limit moves freely anywhere at or below the hard one
   -- and is useful both to cap a broker sharing a machine with other services
   and, chiefly, to rehearse what descriptor exhaustion looks like: with
   `max_open_files = 128` the broker starts with room for about 48 agents, so
   the refusal messages can be seen without root or a real fleet. Lowering is
   always reported as a warning, since it is much the rarer intent and a typo
   (`128` for `1280`) is otherwise noticed only when the fleet stops growing.
   Descriptors already open are unaffected; only further allocations fail.

   Has no effect on Windows, which has no per-process descriptor limit for
   sockets, and is reported as not applicable there. On macOS the raise is
   additionally capped by `kern.maxfilesperproc`, and on Linux by
   `fs.nr_open`. `mads doctor` reports the limit in force and whether it
   leaves room for the fleet.

**ipc** (`[broker]`, boolean, default `true`)
:  Also bind the frontend, backend and settings sockets on `ipc://`
   endpoints (Unix-domain sockets), next to their `tcp://` ones. Agents on
   the broker's host then connect through them instead of the loopback tcp
   stack; see LOCAL TRANSPORT below. Ignored, with a warning, when the
   libzmq build has no ipc transport: the broker then serves tcp only.

**ipc_dir** (`[broker]`, string, default the runtime directory)
:  Directory for the socket files. The default is the per-user runtime
   directory: `$XDG_RUNTIME_DIR/mads`, else `/tmp/mads-<uid>`
   (`%LOCALAPPDATA%\mads\run` on Windows), created owner-only (mode 0700). A
   socket path must fit in 100 bytes (the `sun_path` limit), so keep it
   short.

**frontend_ipc**, **backend_ipc**, **settings_ipc** (`[broker]`, string)
:  Explicit `ipc://` endpoints for one socket, overriding the path derived
   from `ipc_dir`. Needed for a socket whose tcp address has no numeric port.
   A value that is not an `ipc://` endpoint is ignored with a warning.

# LOCAL TRANSPORT

With `ipc = true` (the default) the broker binds each socket a second time,
at `<ipc_dir>/broker-<port>-<role>.ipc`, where *port* is that socket's tcp
port and *role* is `front`, `back` or `settings`: several brokers on one host
therefore never collide. The startup banner and the **N** key list the ipc
endpoints next to the tcp ones. Service discovery (**\-\-room**) still
advertises tcp only.

An ipc problem never affects tcp service: if an endpoint cannot be bound it
is simply not offered. Before binding, the broker probes the path: a stale
file left by a crashed broker is replaced, but a socket another live process
still accepts connections on, or a path that is not a socket, is left alone
and that endpoint is skipped with a warning. On an orderly exit (**Q**,
**X**, SIGINT/SIGTERM in daemon mode) the broker removes its socket files,
but only if they are still the ones it created.

The broker answers a `transport` command on the settings socket with a
one-frame JSON object: its host token (hostname and machine id), the ipc
endpoint of each role (empty when not offered), and whether CURVE is on.
Agents send it at startup and, with `transport = "auto"` (the default),
connect over ipc when the broker runs on their host and the socket files are
visible to them; see **mads-source**(1). A broker that predates the command
answers with its bare version, which agents read as "tcp only".

The `ip_whitelist` does not apply to ipc peers, since they have no IP
address: the owner-only runtime directory is their access control. CURVE,
when enabled with **\-\-crypto**, applies to the ipc endpoints exactly as to
the tcp ones.

On Windows, ipc uses AF_UNIX sockets and needs Windows 10 or later and a
libzmq built against the Windows 10 SDK (`afunix.h`). Without it the broker
serves tcp only, and agents fall back silently. `mads doctor` reports what
the broker advertises and what an agent on this host would use.

# REMOTE EDITING

With `remote_edit = true` in `[broker]` the broker lets an editor (the *Open
broker settings* panel of **mads ini**, see **mads-ini**(1)) read and replace
the settings file it was started with. Nothing changes while the key is off, which
is the default. The service can reach exactly one file, the broker's own
settings file: requests carry no paths. A saved file is validated as TOML,
written atomically and picked up by the broker's file watcher like any other
change (the usual "Reloading settings" message appears), so newly connecting
agents get the new settings at once. A request is refused when the file
changed since the editor loaded it, and the current content is returned
instead.

**remote_edit** (`[broker]`, boolean, default `false`)
:  Turns the service on.

**edit_address** (`[broker]`, string, default `"tcp://*:9094"`)
:  The tcp endpoint. Its port also names the ipc endpoint,
   `<ipc_dir>/broker-<port>-edit.ipc`, so several brokers on one host do not
   collide.

**editors_dir** (`[broker]`, string, default `<keys_dir>/editors`)
:  Directory holding the public keys (`*.pub`, Z85 key on the first line) of
   the editors allowed over tcp. A relative path is resolved like
   **\-\-keys_dir**, against the working directory.

**edit_ipc** (`[broker]`, string)
:  An explicit `ipc://` endpoint, instead of the derived one.

**edit_backups** (`[broker]`, integer, default `10`)
:  How many timestamped backups (`<file>.<YYYYmmdd-HHMMSS>.bak`) are kept next
   to the settings file; every save first copies the previous content.

**edit_password_file** (`[broker]`, string, default `<settings file>.edit_password`)
:  The editing password verifier, created with `mads ini --set-password`. See
   below.

## Security model

Over the **ipc** endpoint (offered when the broker's `ipc` setting is on and
libzmq has ipc) the service is plaintext: like the bus's ipc endpoints, the
owner-only runtime directory holding the socket file is the access control, so
only processes of the broker's OS user can use it. Over **tcp** the service
runs only with **\-\-crypto**, as a CURVE server with the broker's own keypair, and
only clients whose public key is in *editors_dir* are admitted. This list is
separate from the bus's: the keys of agents in the keys directory give no
editing right, and an editor key gives no access to the bus (the exception: with a password set and
no editor keys, the bus keys are admitted, see below). Plaintext tcp is
only accepted in password mode (see "Editing password" below). The broker fails closed:

* without **\-\-crypto** and without a password a warning is printed and only the ipc endpoint is served;
* with no valid key in *editors_dir* and no password, there is no tcp listener;
* if an endpoint cannot be bound (port in use, a live broker owning the ipc
  socket) a warning is printed and the broker carries on without the service.

The editor keys are read at startup: restart the broker after adding or
removing one. When the tcp listener is active its port is announced
in service discovery under the name `edit`.

## Editing password

A password can be set with `mads ini --set-password`. The broker never stores
or receives it: only a salted Argon2id/SCRAM-style verifier is kept, in the
file `<settings file>.edit_password` (or *edit_password_file*), created
owner-only (mode 0600). The file is read at startup, so restart the broker to
change or remove the password; a file readable by group or others is reported
with a warning, and a malformed one with an error, in which case it is treated
as if there were no password (the tcp listener is never weakened). The ipc
endpoint never asks for the password. After 5 consecutive failed attempts the
service refuses every password request for 30 seconds (lockout); failures
and lockouts are logged.

The tcp listener depends on **\-\-crypto** and on the password:

| | no password | password |
|---|---|---|
| no **\-\-crypto** | no tcp listener (ipc only) | plaintext tcp with password |
| **\-\-crypto** | CURVE, editor keys only; none, no tcp listener | CURVE and password; editor keys, or the bus keys when *editors_dir* has none |

Without **\-\-crypto** the password is never sent and requests cannot be
altered in transit, but the settings text itself travels **in clear**: this
"development mode" is for trusted networks only, and the broker prints a
warning at startup. Service discovery announces only the `edit` port; an
editor learns whether a password is needed from the service itself.

## Creating an editor key pair

An editor key pair is an ordinary CURVE pair (see CRYPTO.md): on the editor's
machine generate it with `mads --keypair=NAME`, which writes `NAME.key`
(private, stays there) and `NAME.pub`. Copy `NAME.pub` into the broker's
*editors_dir*, and give the editor `NAME.key`, `NAME.pub` and the broker's
`broker.pub`. Do not put the editor's `.pub` in the broker's keys directory
itself, unless that editor should also join the bus.

## Logging

Every save, and every refused or failed request, is logged on the broker's
console with a timestamp, the editor (its public key, or `ipc`), the outcome
and the size.

# CLOCK OFFSET

The broker answers a `clock` command on the settings socket (the same
REQ/REP endpoint `-s`/`--settings` uses), an NTP-style four-timestamp
exchange that lets an agent measure how far its own clock is from the
broker host's -- see `Mads::Agent::measure_clock_offset()`. This is purely
additive and stateless on the broker side: no configuration is needed here,
and an older broker that predates this command is detected gracefully by
the agent (it falls into the ordinary "unexpected command" reply and the
agent degrades to an unmeasured offset rather than erroring).

The related agent-side settings (`clock_source`, `clock_sync_responder`,
`clock_interval_ms`, `clock_announce_ms`, `clock_correction`) live under
`[agents]` in the settings file the broker serves; see the comments in the
shipped `mads.ini` and `mads-top`(1)'s `--probe` flag for how to see the
measured offsets across a fleet.

# BUGS

The upstream bug tracker can be found at https://github.com/pbosetti/MADS/issues.

# SEE ALSO

**mads**(1), **mads-logger**(1), **mads-source**(1), **mads-sink**(1), **mads-filter**(1), **mads-doctor**(1)

# FILES

**/usr/local/etc/mads.ini**: MADS configuration file.

# AUTHOR

**mads-broker** was written by Paolo Bosetti <paolo.bosetti@unitn.it>.

# LICENSE

http://www.apache.org/licenses/LICENSE-2.0
