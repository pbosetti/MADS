
# NAME

**mads-up** - Headless, scriptable executor for a `director.toml` process plan

# SYNOPSIS

**mads-up**
  [**\-f, \-\-file** *path*]
  [**\-\-until-exit** *name*]
  [**\-\-timeout** *duration*]
  [**\-\-grace** *duration*]
  [**\-\-max-restarts** *n*]
  [**\-\-no-shell**]
  [**\-\-dry-run**]
  [**\-q, \-\-quiet**]
  [**\-v, \-\-version**]
  [**\-h, \-\-help**]

# DESCRIPTION

**mads-up** is a headless executor for `director.toml`, the process-plan format already used by
**mads_director**'s interactive GUI (https://github.com/mads-net/mads_director). It is not a new
compose format: develop and tune an agent set interactively in the Director GUI, commit the resulting
`director.toml`, then run that exact same file unattended in CI, on an edge box, or under `systemd`
(`Type=simple`) with **mads-up**.

**mads-up** parses and validates `director.toml` itself (no dependency on the Director GUI or its GLFW
build requirement), expands `scale` into numbered instances and `${PWD}`/`${ID}` templating in `command`,
and orders startup so every process named in another process's `after` key starts (and, if it declares a
`ready` probe, becomes ready) before its dependents. Startup order and validation is available without
spawning anything via **\-\-dry-run**.

Processes are started via a shell by default (matching Director's own behaviour: `$SHELL -lc "<command>"`
on POSIX, `cmd.exe /S /C "<command>"` on Windows); **\-\-no-shell** tokenizes `command` and execs it
directly instead. Each process's stdout/stderr is captured and multiplexed to this process's own
stdout/stderr, prefixed with `[name]`, unless **\-\-quiet** is given.

**mads-up** is foreground-only by design: there is no daemonization, no PID file, and no `mads down`.
**SIGINT**/**SIGTERM** tear down every managed process (in reverse start order) and this process then
exits -- exactly what `systemd Type=simple`, Docker, and CI runners expect.

## The ready key

Beyond the schema Director already defines (`command`, `after`, `workdir`, `enabled`, `scale`,
`relaunch`, `tty`), **mads-up** adds one new, optional, per-process key:

```toml
ready = "broker" | "broker:<uri>" | "port:<n>" | "log:<regex>" | "delay:<duration>"
```

It gates starting a process's dependents on an actual readiness signal instead of a fixed sleep:

- **`broker`** / **`broker:<uri>`** -- probes a MADS broker's settings endpoint (default
  `tcp://localhost:9092` if no URI is given) until it answers.
- **`port:<n>`** -- probes local TCP port *n* until something accepts a connection.
- **`log:<regex>`** -- waits until a line matching *regex* appears on the process's stdout or stderr.
- **`delay:<duration>`** -- waits a fixed duration (e.g. `500ms`, `2s`, `1.5s`, `3m`; a bare number means
  seconds), unconditionally.

`ready` is additive and silently ignored by Director's own parser, so a `director.toml` carrying it still
loads fine in the GUI.

## Teardown and relaunch

On stop (SIGINT/SIGTERM, **\-\-until-exit**'s target exiting, **\-\-timeout** elapsing, or a
non-`relaunch` process dying with a non-zero exit code), every running process is sent SIGTERM (POSIX) or
`CTRL_BREAK_EVENT` (Windows) in reverse start order, given **\-\-grace** to exit on its own, then SIGKILL
(POSIX) / forcefully terminated (Windows) if it hasn't. Processes are started in their own process group
so descendants (things the managed process itself forks) are torn down too, not just the immediate child.

A process with `relaunch = true` is restarted automatically on a non-zero exit, with exponential backoff
starting at 100ms and capped at 30s (reset once a restarted process has stayed up for a while); pass
**\-\-max-restarts** to give up after a fixed number of attempts instead of retrying forever.

# OPTIONS

**\-f**, **\-\-file** *path*
:  Path to the `director.toml` to run. Defaults to `director.toml` in the current directory.

**\-\-until-exit** *name*
:  Run until the named process exits, then tear everything else down and exit with that process's own
   exit code. The typical CI shape: bring up a broker and a pipeline, run a test agent, exit.

**\-\-timeout** *duration*
:  Hard cap on the whole run (startup and steady state), e.g. `30s`, `5m`. If it elapses, **mads-up**
   tears everything down and exits non-zero.

**\-\-grace** *duration*
:  How long to wait after SIGTERM before escalating to SIGKILL during teardown. Default `5s`.

**\-\-max-restarts** *n*
:  Give up relaunching a `relaunch = true` process after *n* attempts (it stays stopped; the rest of the
   plan keeps running). Default: unlimited (the exponential backoff already bounds CPU usage).

**\-\-no-shell**
:  Tokenize each process's `command` and exec it directly instead of shelling out. Use this when a
   command must not be shell-interpreted (no globbing, no `$VAR` expansion, no `;`/`&&` chaining).

**\-\-dry-run**
:  Print the fully expanded plan (templates resolved, `scale` expanded, start order, `ready` probes) and
   exit without spawning anything.

**\-q**, **\-\-quiet**
:  Suppress the multiplexed `[name]` stdout/stderr passthrough from managed processes.

**\-v**, **\-\-version**
:  Show version information.

**\-h**, **\-\-help**
:  Show summary of options.

# EXIT STATUS

**0** on a clean stop (SIGINT/SIGTERM, or **\-\-until-exit**'s target exiting with code 0). Otherwise the
exit code is non-zero: the propagated exit code of the **\-\-until-exit** target when it is itself
non-zero, the exit code of a non-`relaunch` process that died unexpectedly, or **1** for a `director.toml`
that failed to load/validate, a process that failed to start, a `ready` probe that never succeeded, or
**\-\-timeout** elapsing.

# EXAMPLES

Bring up everything described in `director.toml` and run until Ctrl-C:

```
mads-up
```

CI shape: bring up the plan, run until the `smoke-test` process exits, propagate its exit code:

```
mads-up --until-exit smoke-test --timeout 2m
```

Sanity-check a plan without starting anything:

```
mads-up --dry-run -f deploy/director.toml
```

# BUGS

The upstream bug tracker can be found at https://github.com/pbosetti/MADS/issues.

# SEE ALSO

**mads**(1)

# FILES

**director.toml**: the process plan, in the same format used by `mads_director`'s GUI (see
https://github.com/mads-net/mads_director), plus the additive `ready` key described above.

# AUTHOR

**mads-up** was written by Paolo Bosetti <paolo.bosetti@unitn.it>.

# LICENSE

https://www.apache.org/licenses/LICENSE-2.0
