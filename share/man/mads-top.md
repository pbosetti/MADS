
# NAME

**mads-top** - Live `htop`-style table of active topics on a MADS network

# SYNOPSIS

**mads-top**
  [*topic* ...]
  [**\-b, \-\-broker** *URI*]
  [**\-\-sample-rate** *seconds*]
  [**\-\-window** *seconds*]
  [**\-\-probe**]
  [**\-s, \-\-settings** *arg*]
  [**\-n, \-\-name** *name*]
  [**\-i, \-\-agent-id** *id*]
  [**\-S, \-\-save-settings** *arg*]
  [**\-\-crypto**]
  [**\-\-keys_dir[=path]**]
  [**\-\-key_broker[=name]**]
  [**\-\-key_client[=name]**]
  [**\-\-auth_verbose**]
  [**\-r, \-\-room[=room]**]
  [**\-v, \-\-version**]
  [**\-h, \-\-help**]

# DESCRIPTION

**mads-top** is a live, redrawn-in-place table of the topics currently active on a MADS network -- the
MADS analogue of **htop**. It connects as an ephemeral, read-only sink **Agent** (same zero-config-by-
-default / **\-\-settings**-for-consistency duality as **mads-echo**; see **mads-echo**(1)), subscribes to
everything by default (or to the given MQTT-style topic filters), and shows, per topic: messages/second,
bytes/second (each averaged over a trailing sliding window, **\-\-window**), how long ago the topic was
last seen, and a short preview of its last payload. The table redraws every **\-\-sample-rate** seconds.
Being read-only, it cannot desync a running system.

The header line also carries a **link:** indicator, taken from the transport itself rather than inferred
from traffic. This distinguishes the two reasons a table can go quiet -- nobody is publishing, or the
broker is gone -- which otherwise look identical:

**link: up**
:  The connection to the broker is established (the ZMTP handshake completed).

**link: up (2 drops)**
:  Established now, but the connection has been lost and re-established 2 times since **mads-top**
   started. Shown in yellow: at any single instant a link that keeps flapping looks exactly like one
   that never dropped, and a flapping link is worth knowing about.

**link: DOWN for 12.4s**
:  The broker has gone away (or was never reachable). ZeroMQ keeps retrying underneath, and the
   indicator returns to **up** by itself once it succeeds. If the broker refused the CURVE key, this
   reads **DOWN ... (broker rejected our key)** instead of leaving you to guess.

**link: ?**
:  Nothing observed yet -- still connecting.

Below the topic table, a **Host clock skew** section lists, per hostname
seen in traffic, the minimum of (this machine's receive time - the
message's own `timestamp`) observed in the trailing **\-\-window** -- no
extra protocol, since every published message already carries
`timestamp`/`hostname`. Labelled *skew*, not *offset*: it is a tight upper
bound on offset plus one-way network delay, not a clean measurement, and
disappears once no traffic from that host is seen within the window. See
**\-\-probe** below for a real measurement.

Press **q** (or Ctrl-C) to quit.

# OPTIONS

*topic* ...
:  Zero or more MQTT-style topic filters to subscribe to (see **mads-echo**(1) for the wildcard grammar).
   Overrides any **sub_topic** loaded via **\-\-settings**. Defaults to subscribing to every topic.

**\-b**, **\-\-broker** *URI*
:  Subscribe endpoint to connect to directly, bypassing the settings/broker-query path. Defaults to
   **tcp://localhost:9091**.

**\-\-sample-rate** *seconds*
:  How often the table is redrawn, in seconds. Default **1.0**.

**\-\-window** *seconds*
:  Trailing time window used to average messages/second and bytes/second per topic. Default **5.0**. A
   larger window smooths bursty traffic; a smaller one reacts faster to changes.

**\-\-probe**
:  Replace the topic-activity table with an active clock-offset probe: every **\-\-sample-rate**
   seconds, broadcast a clock-sync ping (**Agent::broadcast_clock_probe()**) and show every
   responder's currently adopted offset, delay and source, grouped by clock domain -- the agents
   sharing one clock domain (the same machine, or containers on one kernel) should always show the
   same offset and the same **ref**; a mismatch that does not resolve itself is worth investigating.
   Unlike the default read-only mode, **\-\-probe** needs a connected publisher, so it is the one case
   where **mads-top** sends anything onto the bus. See "CLOCK OFFSET" below.

**\-s**, **\-\-settings** *URI*
:  Path to the settings file (ini format), or a broker settings URI (**tcp://host:port**). See
   **mads-echo**(1)'s "Two ways to reach a broker" for how this interacts with the zero-config default.

**\-n**, **\-\-name** *name*
:  Agent/section name to use with **\-\-settings** (default **top**).

**\-i**, **\-\-agent-id** *id*
:  Agent ID to add to outgoing JSON frames. Unused in the default read-only mode (**mads-top** never
   publishes there) but accepted for consistency with other **mads-\*** executables; under **\-\-probe**
   it also names this agent's own probes on the bus.

**\-S**, **\-\-save-settings** *filename.ini*
:  Save the settings (loaded via **\-\-settings**) to the given file (ini format), then exit.

**\-\-crypto**
:  Enable CURVE encryption for broker communication.

**\-\-keys_dir[=path]**
:  Directory where CURVE key files are stored.

**\-\-key_broker[=name]**
:  Name of the broker key file, without the **.key** extension. Defaults to **broker**.

**\-\-key_client[=name]**
:  Name of the client key file, without the **.key** extension. Defaults to **client**.

**\-\-auth_verbose**
:  Enable verbose authentication messages.

**\-r**, **\-\-room[=room]**
:  Discover the broker's settings endpoint by service-discovery room name, instead of **\-\-settings**.

**\-v**, **\-\-version**
:  Show version information.

**\-h**, **\-\-help**
:  Show summary of options.

# EXAMPLES

Watch everything flowing through a broker on localhost, redrawing twice a second:

```
mads-top --sample-rate 0.5
```

Watch only sensor traffic on a remote broker, with no **mads.ini** at all, averaged over a 10 second
window:

```
mads-top --broker tcp://plant-broker.local:9091 --window 10 'sensors/#'
```

Check whether every agent in the fleet agrees on its clock offset:

```
mads-top --probe
```

# CLOCK OFFSET

**\-\-probe** is the interactive front end to the same clock-offset measurement described in
**mads-broker**(1)'s "CLOCK OFFSET" section and the **[agents] clock_\*** keys documented in
**mads.ini**. Each redraw shows this agent's own adopted offset first, then every responder heard
within the probe window grouped by clock domain: **OFFSET** (this responder's currently adopted
offset, i.e. what it would apply if **clock_correction** were on), the **source** that produced it
(**broker** or **peer**), hop count, and the round-trip **delay** of this particular probe (expect an
order of magnitude more than a **clock_source = "broker"** exchange, since a bus round trip crosses
the broker's proxy twice). A responder that never measured its own offset shows **?** rather than a
number.

# SEE ALSO

**mads**(1), **mads-broker**(1), **mads-echo**(1)

# FILES

**mads.ini**: only consulted when **\-s/\-\-settings** is given; must then contain a **[top]**-style
section (the exact name is set via **\-n/\-\-name**) if a non-default **sub_topic** is wanted.

# AUTHOR

**mads-top** was written by Paolo Bosetti <paolo.bosetti@unitn.it>.

# LICENSE

https://www.apache.org/licenses/LICENSE-2.0
