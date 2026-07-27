
# NAME

**mads-record** - Subscribes to MADS topics and records every message to a bag file

# SYNOPSIS

**mads-record**
  **\-o, \-\-output** *path*
  [**\-\-no-crc**]
  [**\-\-count** *n*]
  [**\-x, \-\-cross**]
  [**\-s, \-\-settings** *URI*]
  [**\-\-crypto**]
  [**\-v, \-\-version**]
  [**\-h, \-\-help**]

# DESCRIPTION

**mads-record** is an ordinary MADS agent that subscribes per its **sub_topic** setting (MQTT-style
wildcard filters supported, e.g. **sensors/#** or **sensors/+/x** -- see **CONTEXT.md**'s "Settings
model" section) and writes every message it receives to a bag file: a bespoke binary format that
stores the exact multi-part wire frame (timestamp + topic + parts) byte-for-byte, so JSON payloads
are never re-parsed/re-serialized and binary blobs are copied exactly once. Both JSON and blob
messages round-trip through a bag byte-identical.

The bag format is documented in full in **src/bag.hpp**. It carries a trailing index and footer for
O(1) **mads bag info** and fast seeking; if **mads-record** is killed before it can write that footer
(e.g. **SIGKILL**, a crash, a power loss), the file is still a valid, readable bag -- **mads-play**/
**mads bag** fall back to a linear scan that recovers every complete record written so far.

The administrative **control** and **agent_event** topics are never recorded, even under a catch-all
**sub_topic** (e.g. **sub_topic = [""]**), mirroring **mads-federate**'s own rationale for excluding
them from automatic relaying: capturing (and later replaying, via **mads-play**) a remote-control
command such as **shutdown** would be a footgun, not a feature.

**mads-record** stops on **SIGINT**/**SIGTERM** (or after **\-\-count** messages, if given), finalizing
the bag file (writing its index/footer) before exiting.

# OPTIONS

**\-o**, **\-\-output** *path*
:  Bag file to write. Required. Overwritten if it already exists.

**\-\-no-crc**
:  Disable the per-record CRC32 checksum (enabled by default). Slightly reduces file size and write
   cost; disables corruption detection on read.

**\-\-count** *n*
:  Stop automatically after recording *n* messages (default: 0, unlimited -- runs until stopped).

**\-x**, **\-\-cross**
:  Cross-connect sockets (bind instead of connect on the subscribe side). Mainly useful for testing
   without a broker.

**\-n**, **\-\-name** *name*
:  Agent/section name. **mads-record** reads its **sub_topic** from the settings section named after
   this (default: **record**), so several recorders with different filters can run from the same
   **mads.ini**.

**\-i**, **\-\-agent-id** *id*
:  Agent ID (has no effect on recorded frames themselves -- **mads-record** never publishes; kept for
   consistency with every other MADS agent executable).

**\-s**, **\-\-settings** *URI*
:  Settings file path or broker URI. Same as every other MADS agent; **mads-record** reads its
   **sub_topic** from the **[record]** section (or the name given via **\-\-name**).

**\-\-crypto**, **\-\-keys_dir**, **\-\-key_broker**, **\-\-key_client**, **\-\-auth_verbose**
:  CURVE encryption options, same as every other MADS agent executable.

**\-v**, **\-\-version**
:  Show version information.

**\-h**, **\-\-help**
:  Show summary of options.

# EXAMPLES

Record every message on the default settings/broker into a bag file:

```
mads-record -o session.bag
```

On the broker's **mads.ini**, select what **mads-record** subscribes to:

```ini
[record]
sub_topic = ["sensors/#"]
```

Record at most 1000 messages and stop automatically:

```
mads-record -o sample.bag --count 1000
```

Inspect or replay the result with **mads bag info** / **mads-play**:

```
mads bag info -f session.bag
mads-play -i session.bag
```

# BUGS

The upstream bug tracker can be found at https://github.com/pbosetti/MADS/issues.

# SEE ALSO

**mads**(1), **mads-play**(1), **mads-bag**(1), **mads-broker**(1), **mads-federate**(1)

# FILES

**mads.ini** (on the broker): must contain a **[record]**-style section (the exact name is set via
**\-\-name**) with a **sub_topic** list, like any other MADS agent.

# AUTHOR

**mads-record** was written by Paolo Bosetti <paolo.bosetti@unitn.it>.

# LICENSE

https://www.apache.org/licenses/LICENSE-2.0
