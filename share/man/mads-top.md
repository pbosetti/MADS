
# NAME

**mads-top** - Live `htop`-style table of active topics on a MADS network

# SYNOPSIS

**mads-top**
  [*topic* ...]
  [**\-b, \-\-broker** *URI*]
  [**\-\-sample-rate** *seconds*]
  [**\-\-window** *seconds*]
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

**\-s**, **\-\-settings** *URI*
:  Path to the settings file (ini format), or a broker settings URI (**tcp://host:port**). See
   **mads-echo**(1)'s "Two ways to reach a broker" for how this interacts with the zero-config default.

**\-n**, **\-\-name** *name*
:  Agent/section name to use with **\-\-settings** (default **top**).

**\-i**, **\-\-agent-id** *id*
:  Agent ID to add to outgoing JSON frames. Unused by **mads-top** itself (it never publishes) but
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

Watch everything flowing through a broker on localhost, redrawing twice a second:

```
mads-top --sample-rate 0.5
```

Watch only sensor traffic on a remote broker, with no **mads.ini** at all, averaged over a 10 second
window:

```
mads-top --broker tcp://plant-broker.local:9091 --window 10 'sensors/#'
```

# SEE ALSO

**mads**(1), **mads-broker**(1), **mads-echo**(1)

# FILES

**mads.ini**: only consulted when **\-s/\-\-settings** is given; must then contain a **[top]**-style
section (the exact name is set via **\-n/\-\-name**) if a non-default **sub_topic** is wanted.

# AUTHOR

**mads-top** was written by Paolo Bosetti <paolo.bosetti@unitn.it>.

# LICENSE

https://www.apache.org/licenses/LICENSE-2.0
