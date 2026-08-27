
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
   above.

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

# BUGS

The upstream bug tracker can be found at https://github.com/pbosetti/MADS/issues.

# SEE ALSO

**mads**(1), **mads-logger**(1), **mads-source**(1), **mads-sink**(1), **mads-filter**(1)

# FILES

**/usr/local/etc/mads.ini**: MADS configuration file.

# AUTHOR

**mads-broker** was written by Paolo Bosetti <paolo.bosetti@unitn.it>.

# LICENSE

http://www.apache.org/licenses/LICENSE-2.0
