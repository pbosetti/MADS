
# NAME

**mads-doctor** - Environment/connectivity health check for a MADS deployment

# SYNOPSIS

**mads-doctor**
  [**\-s, \-\-settings** *path*]
  [**\-\-broker** *uri*]
  [**\-\-timeout** *ms*]
  [**\-\-plugin** *path*]...
  [**\-\-crypto**]
  [**\-\-keys_dir** *dir*]
  [**\-\-key_broker** *name*]
  [**\-\-key_client** *name*]
  [**\-\-fix**]
  [**\-v, \-\-version**]
  [**\-h, \-\-help**]

**mads-doctor** **\-\-plan** *director.toml*

**mads-doctor** **\-\-graph**\[=*file.dot*\] [**\-\-graph-fanout**] [**\-\-graph-live**]

# DESCRIPTION

**mads-doctor** is MADS's answer to `ros2 doctor`/`brew doctor`: a single command that checks the
things that most commonly go wrong in the first hour of using a pub-sub framework -- a wrong broker
URI, a stale settings file, a plugin protocol mismatch, a port already in use -- and reports each one
as **[PASS]**, **[WARN]**, or **[FAIL]** with a one-line fix hint. It never starts a broker or an
agent, never calls a plugin's `process()`/`get_output()`/`load_data()`, and never writes anything
except the one narrow case **\-\-fix** covers.

By default it checks, in order:

1. **Settings file found and parses.** The path given by **\-\-settings** (default `mads.ini` in the
   current directory -- unlike every other **mads-*** executable, whose `-s`/`--settings` defaults to
   a broker URI, since checking a *local file* is the point of this check) exists and is valid TOML.
   A `tcp://...` value is treated as a remote broker URI instead of a local file, and this check is
   reported **[PASS]** with a note pointing at the broker-reachable check below.
2. **Broker reachable.** Probes the broker's settings endpoint (from **\-\-broker**, else the
   `[broker]` section's `settings_address`, else `tcp://localhost:9092`) the same way `mads up`'s
   `ready = "broker"` does. With **\-\-crypto** the probe is CURVE-encrypted, and the endpoint is
   reported as `... (CURVE) is reachable`. The encryption mode has to match the broker's: a
   CURVE-secured broker drops a plain peer during the ZMTP handshake, and a plain broker drops an
   encrypted one, so either mismatch looks exactly like a broker that is down. When this check
   fails, its hint names that as a possible cause.
3. **Declared plugin(s) resolve and load.** Every `attachment` key found in the settings file (or
   every **\-\-plugin** path given explicitly, which then takes priority) is dry-run loaded through
   the same `pugg::Kernel` path **mads-source**/**mads-filter**/**mads-sink** use -- loaded, its
   `kind()` and protocol read, then unloaded. `process()`/`get_output()`/`load_data()` are never
   called.
4. **Plugin protocol matches the pinned `mads_plugin` version.** Compares each loaded plugin's
   protocol against `share/plugin_deps.json`'s `plugin_protocol` (the version `mads plugin --update`
   migrates plugins to).
5. **CURVE key files, if **\-\-crypto** is given, exist and are well-formed.** Checks
   `<key_client>.key`/`.pub` and `<key_broker>.pub` under **\-\-keys_dir** -- the same three files
   `Mads::CurveAuth::setup_curve_client()` reads -- exist and decode as valid Z85 CURVE keys.
6. **CURVE handshake actually succeeds, if **\-\-crypto** is given and check 5 passed.** Attempts a
   real CURVE handshake against the broker URI from check 2, with a `Mads::SocketMonitor` attached so
   a `ZMQ_EVENT_HANDSHAKE_FAILED_AUTH` rejection is reported as "the broker rejected this key" rather
   than the bare timeout a rejection and an unreachable broker used to look identical as.
7. **Local port-availability sanity check.** Probes the settings file's `[broker]`
   `frontend_address`/`backend_address`/`settings_address` ports on `127.0.0.1`; a port already
   answering is reported as a likely broker-already-running collision.

**mads-doctor** exits **0** only if every check reported **[PASS]** or **[WARN]**; any **[FAIL]**
makes it exit **1**.

## `--plan`

**\-\-plan** *director.toml* is a standalone mode: it parses, validates and expands the given
`director.toml` through the exact same `src/director_config.hpp` module `mads up --dry-run` uses (no
process spawning), and prints the same kind of expanded-plan report -- start order, templated
`command`, `scale` expansion, `after` dependencies, `ready` probes. It ignores every other flag except
**\-\-settings**-independent behaviour: the settings file, broker, plugins, and CURVE keys are not
touched. Use it to sanity-check a whole deployment before running `mads up` for real.

## `--graph`

**\-\-graph**\[=*file.dot*\] is another standalone, read-only mode: it parses the settings file (the
same **\-\-settings** path used by check 1, reusing the same TOML-loading logic) and emits a
**Graphviz DOT** description of the pub/sub topology it declares, to *file.dot* if given, or to
standard output otherwise. It never probes the broker, plugins, ports, or CURVE keys, and never
shells out to a `dot` binary -- rendering the DOT text to an image is left to the user (e.g.
`mads doctor --graph | dot -Tpng -o topology.png`).

Every non-`[agents]`/non-`[broker]` section becomes one record-shaped node, named after the section,
with its `sub_topic` entries listed underneath (one per line; `sub_topic = [""]` -- subscribe-all --
renders as a single `(all)` line; no subscriptions renders a bare name node). Nodes are colored by
inferred role: a source (declared `pub_topic` only) is `darkred`, a filter (both) is `darkgreen`, a
sink (`sub_topic` only) is `darkblue`; an agent with neither gets no color attribute.

One edge is drawn for every section A's `pub_topic` and every *other* section B's `sub_topic` entry
that matches it, using `Mads::subscription_match()` -- the same rule the running agent applies, which
is deliberately **not** plain string equality:

- A literal (wildcard-free) `sub_topic` entry is handed to ZeroMQ verbatim, and ZeroMQ matches by raw
  **byte prefix**: `sub_topic = ["sensors"]` really does receive `sensors/imu/raw`, and
  `sub_topic = [""]` really does receive everything.
- An entry containing `+`/`#` is matched by MQTT rules (`Mads::topic_match()`), after a broader
  `Mads::literal_prefix()` subscribe at the ZeroMQ layer.

Since checking those looser matches is the main reason to draw the graph at all, how each edge came
to exist is encoded in it: **solid** for an exact match, **dashed** for a literal prefix catch,
**dotted** for a wildcard match. Any non-exact edge also names the pattern(s) responsible under the
published topic (`sensors/imu/raw` over `via sensors/#`), so an unintended catch is visible at a
glance rather than inferred by hand.

Sections with no `pub_topic` key still publish -- under their own name, exactly as
`Agent`'s `cfg["pub_topic"].value_or(<agent name>)` default does -- so their edges are drawn too, in
**grey** to mark the topic as implied rather than declared. Such an implied topic never colors the
node (a sink stays a sink) and is never reported as dangling, since nobody listening to it is the
normal case.

A node gets a **dashed** contour if it has a dangling topic -- a declared `pub_topic` nothing
subscribes to, or a `sub_topic` entry nothing ever publishes to -- which is usually a
misconfiguration worth a second look. The offending entry is marked `[!]` in the node's own label, so
*which* pattern is dead is visible directly; a dangling `pub_topic`, which produces no edge to read
it off, is spelled out in the node as `pub <topic> [!]`. An agent's own `pub_topic` matching its own
`sub_topic` (a self-loop) satisfies both sides and is not flagged as dangling, matching real broker
behavior.

By default, edges into a subscribe-all subscriber that match *only* its `""` entry are collapsed into
a count on that node's `(all)` line (`(all) [21 publishers]`) instead of being drawn. A subscribe-all
subscriber matches every publisher by definition, so in a real deployment -- a logger, a monitor, a
recorder -- those edges are at once the most numerous and the least informative, and they bury the
wiring the graph is being read for. Nothing is lost: "receives everything" is exactly what the
`(all)` line says, and a DOT comment records how many arrows were collapsed. Pass **\-\-graph-fanout**
to draw them all instead.

## `--graph-live`

By default **\-\-graph** describes what the settings file *declares*. **\-\-graph-live** additionally
asks a running broker what is *actually* subscribed right now, and overlays the answer. It annotates,
never replaces: nodes and edges still come from the settings file.

Three situations, indistinguishable on a declared-only graph, become visible:

| Situation | Rendering |
|---|---|
| Declared, and someone is subscribed | `topic [live 2]` on the subscriber's entry |
| Declared, but nobody is subscribed | `topic [offline]` |
| Subscribed, but nobody declared it | a separate orange `note` node, with dotted arrows from whichever declared publishers that listener is actually receiving from |

The third is the one that is otherwise *invisible*: an agent subscribing to a topic no settings
section mentions leaves no trace at all on a declared-only graph.

This requires the broker to be running with `[broker] subscription_table = true`, and -- against an
encrypted broker -- **\-\-crypto** on the **mads-doctor** side too. If no table arrives,
**mads-doctor** exits **1** with an error rather than falling back to a declared-only graph -- a
silently degraded live graph is byte-identical to a plain one, so it would read as confirmation that
the fleet matches its settings when in fact nothing was measured. Plain **\-\-graph** is unaffected and
keeps working with no broker at all.

Two limits follow from what a subscription table can express, and are worth keeping in mind:

- **It is anonymous.** ZMQ subscription frames carry no peer identity, so a count says *how many*
  sockets are subscribed to a prefix, never *which* agents. If two sections declare the same
  `sub_topic` and only one is running, both are shown live.
- **It says nothing about publishers.** A pure source leaves no trace in a subscription table, so it
  is never marked offline -- only "nothing is listening to it", which the declared graph already
  shows with `[!]`.

Wildcard entries are looked up under the literal prefix they actually subscribe at the ZMQ layer
(`sensors/+/raw` is subscribed as `sensors/`), matching what **mads**(1) agents do at runtime. MADS's
own internal subscriptions (`control`, and the `subscriptions` topic this query itself joins) are
never reported as undeclared.

## `--fix`

**\-\-fix** attempts the one check with an unambiguous, non-destructive auto-fix: if the settings file
is missing, it is scaffolded from the same template `mads ini` renders. **\-\-fix** only ever
*creates* a missing file -- it never deletes or overwrites an existing one, however broken.

# OPTIONS

**\-s**, **\-\-settings** *path*
:  Path to the settings file to check. Default `mads.ini` in the current directory. A `tcp://...`
   value is treated as a remote broker URI instead.

**\-\-broker** *uri*
:  Broker settings endpoint to probe for check 2. Default: derived from the settings file's
   `[broker]` section, else `tcp://localhost:9092`.

**\-\-timeout** *ms*
:  Timeout in milliseconds for the broker-reachable and port-availability probes. Default `1000`.

**\-\-plugin** *path*
:  Dry-run load this plugin file (repeatable). When given, replaces the automatic scan of the
   settings file's `attachment` keys. Resolved like **mads-source**/**-filter**/**-sink**'s own
   `plugin` argument: relative to the current directory first, falling back to the installed-plugin
   location.

**\-\-crypto**
:  Run the CURVE key-file check (check 5) and the handshake check (check 6), *and* speak CURVE in
   every probe that talks to the broker -- checks 2 and 6, and **\-\-graph-live**'s subscription-table
   read. Off by default, since CURVE is opt-in; pass it whenever the broker runs with `--crypto`,
   and leave it off when the broker does not.

**\-\-keys_dir** *dir*
:  Directory to look for CURVE key files in. Default: `<exec_dir>/../etc`, same as every other
   **mads-*** executable's `--keys_dir`.

**\-\-key_broker** *name*
:  Base name (without `.pub`) of the broker/server CURVE public key file. Default `broker`.

**\-\-key_client** *name*
:  Base name (without `.key`/`.pub`) of the client CURVE key files. Default `client`.

**\-\-plan** *director.toml*
:  Validate a `director.toml` deployment plan (like `mads up --dry-run`) and exit; see above.

**\-\-graph**\[=*file.dot*\]
:  Emit a Graphviz DOT topology graph of the settings file's declared pub/sub topics to *file.dot*, or
   standard output if omitted, and exit; see above.

**\-\-graph-fanout**
:  With **\-\-graph**: draw one edge per publisher into every subscribe-all (`sub_topic = [""]`)
   subscriber, instead of collapsing them into a count on the subscriber's `(all)` line. Ignored
   without **\-\-graph**.

**\-\-graph-live**
:  With **\-\-graph**: overlay live subscriber counts read from the broker's subscription table onto
   the declared graph; see above. Requires `[broker] subscription_table = true`, and waits at least
   2500 ms (the broker republishes the table about once a second) regardless of a lower
   **\-\-timeout**. Against an encrypted broker it also needs **\-\-crypto** (and **\-\-keys_dir** if the
   keys are not in the default location), since it subscribes to the broker's backend for real.
   Ignored without **\-\-graph**.

**\-\-fix**
:  Attempt safe, non-destructive auto-fixes; see above.

**\-v**, **\-\-version**
:  Show version information.

**\-h**, **\-\-help**
:  Show summary of options.

# EXIT STATUS

**0** if every check reported **[PASS]** or **[WARN]** (or, in **\-\-plan** mode, if the plan loaded
and expanded cleanly). **1** if any check reported **[FAIL]**, or **\-\-plan**'s file failed to
load/validate.

# EXAMPLES

Check the settings file in the current directory, plus broker reachability and port availability:

```
mads doctor
```

Check a specific settings file and CURVE keys, scaffolding the file if it is missing:

```
mads doctor -s /etc/mads/mads.ini --crypto --fix
```

Sanity-check a whole deployment plan before running it for real:

```
mads doctor --plan deploy/director.toml
```

Print the declared pub/sub topology as DOT text, and render it to an image with Graphviz:

```
mads doctor --graph | dot -Tpng -o topology.png
```

Write the topology graph straight to a file instead:

```
mads doctor --graph=topology.dot
```

# BUGS

The upstream bug tracker can be found at https://github.com/pbosetti/MADS/issues.

# SEE ALSO

**mads**(1), **mads-up**(1), **mads-broker**(1), **mads-source**(1), **mads-filter**(1),
**mads-sink**(1)

# AUTHOR

**mads-doctor** was written by Paolo Bosetti <paolo.bosetti@unitn.it>.

# LICENSE

https://www.apache.org/licenses/LICENSE-2.0
