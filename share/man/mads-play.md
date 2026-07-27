
# NAME

**mads-play** - Replays a bag file recorded by mads-record back onto a MADS network

# SYNOPSIS

**mads-play**
  **\-i, \-\-input** *path*
  [**\-\-restamp**]
  [**\-\-topics** *pattern*]...
  [**\-\-rate** *factor*]
  [**\-x, \-\-cross**]
  [**\-s, \-\-settings** *URI*]
  [**\-\-crypto**]
  [**\-v, \-\-version**]
  [**\-h, \-\-help**]

# DESCRIPTION

**mads-play** reads a bag file written by **mads-record** (see **src/bag.hpp** for the on-disk format)
and republishes every record exactly as it was captured -- topic and parts, byte-for-byte -- using the
raw wire path (**Agent::publish_raw_message()**), so JSON payloads are never re-serialized and blob
bytes are never re-copied through the JSON path. A subscriber on the network cannot tell a replayed
message from the original.

If the bag file is missing its trailing index (e.g. **mads-record** was killed before it could finalize
the file), **mads-play** falls back to a linear scan that recovers every complete record and prints a
warning naming how many records will be replayed.

# OPTIONS

**\-i**, **\-\-input** *path*
:  Bag file to replay. Required.

**\-\-restamp**
:  Best-effort rewrite of the **timestamp**/**timecode** fields to the current time before
   republishing, for records whose payload is the legacy, header-less **[topic][snappy(json)]** frame
   (the shape **Agent::publish()** emits whenever the payload ends up Snappy-compressed --
   unconditionally under **compression = "snappy"**, or above ~256 bytes under the default
   **compression = "auto"**). Every other field, and every other frame shape (an uncompressed small
   JSON payload carrying a self-describing header, a MsgPack frame, or a blob's meta+bytes parts), is
   republished byte-for-byte unchanged. This is a **mads-play**-only convenience built on top of
   **publish_raw_message()**; it does not change what that function does for any other caller.

**\-\-topics** *pattern*
:  MQTT-style topic filter (**+** matches one level, **#** matches this level and below); only records
   whose stored topic matches at least one given pattern are replayed. Repeatable. Default: replay
   every topic in the bag.

**\-\-rate** *factor*
:  Paces replay using the gaps between the recorded timestamps of the records actually being replayed
   (records skipped by **\-\-topics** don't contribute a gap), scaled by *factor* (**2.0** replays twice
   as fast as originally recorded, **0.5** half as fast). Must be **> 0**. Default: no pacing -- records
   are republished back-to-back as fast as possible.

**\-x**, **\-\-cross**
:  Cross-connect sockets (bind instead of connect on the publish side). Mainly useful for testing
   without a broker.

**\-n**, **\-\-name** *name*
:  Agent/section name (default: **play**). Only the endpoint/**pub_topic** placeholder from this
   section are used -- every record is published under its own recorded topic, not **pub_topic**.

**\-\-agent-id** *id*
:  Agent ID (no short flag: **\-i** is already **\-\-input** above). Has no effect on replayed frames
   themselves, which are republished byte-for-byte; kept for consistency with every other MADS agent
   executable.

**\-s**, **\-\-settings** *URI*
:  Settings file path or broker URI, same as every other MADS agent.

**\-\-crypto**, **\-\-keys_dir**, **\-\-key_broker**, **\-\-key_client**, **\-\-auth_verbose**
:  CURVE encryption options, same as every other MADS agent executable.

**\-v**, **\-\-version**
:  Show version information.

**\-h**, **\-\-help**
:  Show summary of options.

# EXAMPLES

Replay a bag exactly as recorded:

```
mads-play -i session.bag
```

Replay only a subset of topics, twice as fast as recorded, with fresh timestamps:

```
mads-play -i session.bag --topics 'sensors/#' --rate 2.0 --restamp
```

# BUGS

The upstream bug tracker can be found at https://github.com/pbosetti/MADS/issues.

# SEE ALSO

**mads**(1), **mads-record**(1), **mads-bag**(1), **mads-broker**(1)

# FILES

**mads.ini** (on the broker): must contain a **[play]**-style section (the exact name is set via
**\-\-name**), like any other MADS agent -- only its endpoint is actually used.

# AUTHOR

**mads-play** was written by Paolo Bosetti <paolo.bosetti@unitn.it>.

# LICENSE

https://www.apache.org/licenses/LICENSE-2.0
