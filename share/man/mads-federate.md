
# NAME

**mads-federate** - Relays selected topics between two independent MADS networks

# SYNOPSIS

**mads-federate**
  **\-a, \-\-settings-a** *URI*
  **\-b, \-\-settings-b** *URI*
  [**\-\-name-a** *name*]
  [**\-\-name-b** *name*]
  [**\-\-id** *relay-id*]
  [**\-\-poll-interval-us** *microseconds*]
  [**\-v, \-\-version**]
  [**\-h, \-\-help**]

# DESCRIPTION

**mads-federate** is a relay agent that connects to **two** independent MADS networks (each with its
own broker) and forwards selected topics between them, in both directions. It is the only way to move
messages across two brokers: the broker itself is a payload-opaque ZeroMQ proxy and has no notion of
peering with another broker.

Internally, **mads-federate** owns two ordinary MADS agent connections ("side A" and "side B"), one per
network, each configured exactly like any other agent — through a **[name]** section in *that network's
own* broker settings. Which topics are relayed in each direction is controlled entirely by the normal
**sub_topic** setting of each side's section: whatever side A subscribes to (from network A) is forwarded
to network B, and vice versa. There is no separate topic-filtering option specific to **mads-federate**.

Only JSON messages are relayed in this version (binary/blob payloads are not forwarded). The **control**
and **agent_event** topics are never relayed, regardless of **sub_topic**, so that remote-control commands
and agent lifecycle events stay local to their own network.

## Loop prevention

Every relayed message is tagged with this relay's own id (see **\-\-id**) in a **mads_relay_path** JSON
array field before being published on the other side. A message that already carries this relay's id in
that field is not forwarded again, which prevents the immediate A -> B -> A ping-pong for a single relay.
Chains of several **mads-federate** instances degrade gracefully (a message accumulates one id per hop),
but a mesh of relays with overlapping topic sets can still loop; keep topic sets for each direction
disjoint (or few relays) to avoid this.

The original publisher's **agent_id**/**hostname** fields are preserved across the relay (Agent::publish
only stamps them when absent), so consumers on the other network still see who actually published the
message, not the relay.

# OPTIONS

**\-a**, **\-\-settings-a** *URI*
:  Settings URI (or local .ini path) for network A's broker. Required.

**\-b**, **\-\-settings-b** *URI*
:  Settings URI (or local .ini path) for network B's broker. Required.

**\-\-name-a** *name*
:  Agent/section name used on network A: **mads-federate** fetches its own configuration (**sub_topic**,
   **pub_topic**, ...) from the **[**name**]** section of network A's broker settings. Defaults to
   **federate_a**. Network A's own ini must contain a matching section, exactly as for any other agent.

**\-\-name-b** *name*
:  Same as **\-\-name-a**, for network B. Defaults to **federate_b**.

**\-\-id** *relay-id*
:  Identifier stamped into relayed messages for loop-prevention (see above). Defaults to
   *name-a*-*name-b*. If you run more than one **mads-federate** instance between the same pair of
   networks, give each a distinct **\-\-id**.

**\-\-poll-interval-us** *microseconds*
:  How long to sleep between polls when neither side has a pending message (default 1000, i.e. 1 ms).
   Lower values reduce forwarding latency at the cost of CPU usage when idle.

**\-v**, **\-\-version**
:  Show version information.

**\-h**, **\-\-help**
:  Show summary of options.

# EXAMPLES

Relay topics between two networks, using each broker's own settings service:

```
mads-federate --settings-a tcp://broker-a.local:9092 \
              --settings-b tcp://broker-b.local:9092
```

On broker A's **mads.ini**, add a section selecting what to send to network B:

```ini
[federate_a]
sub_topic = ["sensors", "alarms"]
```

On broker B's **mads.ini**, add the matching section selecting what to send to network A:

```ini
[federate_b]
sub_topic = ["commands"]
```

# BUGS

The upstream bug tracker can be found at https://github.com/pbosetti/MADS/issues.

# SEE ALSO

**mads**(1), **mads-broker**(1), **mads-bridge**(1)

# FILES

**mads.ini** (on each broker): must contain a **[federate_a]**/**[federate_b]**-style section (the exact
name is set via **\-\-name-a**/**\-\-name-b**) with a **sub_topic** list, like any other MADS agent.

# AUTHOR

**mads-federate** was written by Paolo Bosetti <paolo.bosetti@unitn.it>.

# LICENSE

https://www.apache.org/licenses/LICENSE-2.0
