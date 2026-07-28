
# NAME

**mads-echo** - Print live MADS messages to the console

# SYNOPSIS

**mads-echo**
  [*topic* ...]
  [**\-b, \-\-broker** *URI*]
  [**\-\-raw**]
  [**\-\-count** *N*]
  [**\-\-jsonl**]
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

**mads-echo** is a zero-config way to peek at live traffic on a MADS network without writing an agent or
touching **mads.ini** -- the MADS analogue of **ros2 topic echo** / **rostopic echo** / **mosquitto_sub**.
It connects as an ephemeral, read-only sink **Agent** and pretty-prints every message it receives: topic,
timestamp, size, and either indented (colorized) JSON or a one-line blob summary. Being read-only, it
cannot desync a running system.

Each positional *topic* argument is an MQTT-style subscription filter (see **CONTEXT.md**'s "Settings
model" section): a plain string subscribes exactly as-is, while **+** (exactly one topic level) and **#**
(this level and everything below it, only legal as the last token) act as wildcards, e.g.
**sensors/+/x** or **sensors/#**. With no *topic* argument, **mads-echo** subscribes to everything.

## Two ways to reach a broker

**mads-echo** supports two independent, mutually compatible ways to connect:

- **Zero-config (default).** No **mads.ini** section is required. **\-\-broker** points directly at the
  broker's backend (subscribe) endpoint; *topic* arguments are given directly on the command line. This
  is the common case for quickly checking what is being published on a known broker.
- **\-s, \-\-settings**, for consistency with every other **mads-\*** executable. **mads-echo** then
  behaves exactly like **mads-logger** etc.: it queries the broker (or reads a local **.ini** file) for a
  **[echo]** section (or **[**name**]** with **\-n/\-\-name**), taking its **sub_topic** from there unless
  overridden by positional *topic* arguments on the command line.

**\-\-broker** can be combined with either mode: it always overrides whatever subscribe endpoint the
other mode resolved to, which is the most direct way to point at a specific broker with zero setup.

# OPTIONS

*topic* ...
:  Zero or more MQTT-style topic filters to subscribe to. Overrides any **sub_topic** loaded via
   **\-\-settings**. Defaults to subscribing to every topic.

**\-b**, **\-\-broker** *URI*
:  Subscribe endpoint to connect to directly, bypassing the settings/broker-query path (e.g.
   **tcp://broker.local:9091**). Defaults to **tcp://localhost:9091**.

**\-\-raw**
:  For blob messages, print the exact bytes (base64-encoded) instead of the default one-line
   **&lt;blob N bytes&gt;** summary.

**\-\-count** *N*
:  Exit automatically after printing *N* messages.

**\-\-jsonl**
:  Emit one compact JSON line per message (**topic**, **timestamp**, **size**, **type**, **payload**)
   instead of the multi-line pretty form, and disable coloring -- intended for piping into **jq** or
   similar tools.

**\-s**, **\-\-settings** *URI*
:  Path to the settings file (ini format), or a broker settings URI (**tcp://host:port**). See "Two ways
   to reach a broker" above.

**\-n**, **\-\-name** *name*
:  Agent/section name to use with **\-\-settings** (default **echo**).

**\-i**, **\-\-agent-id** *id*
:  Agent ID to add to outgoing JSON frames. Unused by **mads-echo** itself (it never publishes) but
   accepted for consistency with other **mads-\*** executables.

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

Print everything flowing through a broker on localhost:

```
mads-echo
```

Print only accelerometer readings from any sensor, on a remote broker, with no **mads.ini** at all:

```
mads-echo --broker tcp://plant-broker.local:9091 'sensors/+/acc'
```

Pipe the last 50 messages on a topic through **jq**:

```
mads-echo --jsonl --count 50 'sensors/#' | jq '.payload'
```

Using the normal settings path, exactly like **mads-logger**:

```
mads-echo --settings tcp://broker.local:9092
```

with a matching section in that broker's **mads.ini**:

```ini
[echo]
sub_topic = ["sensors/#"]
```

# SEE ALSO

**mads**(1), **mads-broker**(1), **mads-top**(1), **mads-logger**(1)

# FILES

**mads.ini**: only consulted when **\-s/\-\-settings** is given; must then contain an **[echo]**-style
section (the exact name is set via **\-n/\-\-name**) if a non-default **sub_topic** is wanted.

# AUTHOR

**mads-echo** was written by Paolo Bosetti <paolo.bosetti@unitn.it>.

# LICENSE

https://www.apache.org/licenses/LICENSE-2.0
