
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
   `ready = "broker"` does.
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
6. **Local port-availability sanity check.** Probes the settings file's `[broker]`
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
:  Also run the CURVE key-file check (check 5). Off by default, since CURVE is opt-in.

**\-\-keys_dir** *dir*
:  Directory to look for CURVE key files in. Default: `<exec_dir>/../etc`, same as every other
   **mads-*** executable's `--keys_dir`.

**\-\-key_broker** *name*
:  Base name (without `.pub`) of the broker/server CURVE public key file. Default `broker`.

**\-\-key_client** *name*
:  Base name (without `.key`/`.pub`) of the client CURVE key files. Default `client`.

**\-\-plan** *director.toml*
:  Validate a `director.toml` deployment plan (like `mads up --dry-run`) and exit; see above.

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

# BUGS

The upstream bug tracker can be found at https://github.com/pbosetti/MADS/issues.

# SEE ALSO

**mads**(1), **mads-up**(1), **mads-broker**(1), **mads-source**(1), **mads-filter**(1),
**mads-sink**(1)

# AUTHOR

**mads-doctor** was written by Paolo Bosetti <paolo.bosetti@unitn.it>.

# LICENSE

https://www.apache.org/licenses/LICENSE-2.0
