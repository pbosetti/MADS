# Run state and agent lifecycle

## Context

Since MADS v2.4.0, the "keep going" state that drives `Mads::Agent::loop()` is
owned by a dedicated object, `Mads::Runtime`, instead of a single
process-global flag. This page explains the model, how to control agent
lifecycles with it, and how the deprecated `Mads::running` flag maps onto it.

Historically, all loops in a process watched one global `Mads::running`
atomic, and `Agent::shutdown()`/`Agent::disconnect()` cleared it. That made
instance-level teardown mutate process-level state, with surprising effects
when a process hosted more than one agent: destroying one agent silently
ended every other agent's `loop()`, froze their last-known-value drain
threads on stale data, could arm another agent's watchdog into a forced
process exit, and a disconnected agent could never loop again.

## The model

There are three levels of run state, from narrowest to widest:

1. **Agent** — `Agent::shutdown()` and `Agent::disconnect()` raise a stop
   request that affects *only that agent*. `Agent::connect()` clears it, so
   an agent can be disconnected, reconfigured, reconnected, and looped again.
2. **Runtime (group)** — every agent references a `Mads::Runtime`; by default
   each agent owns its own. Calling `runtime->stop()` ends the loops of every
   agent attached to that Runtime. Attach several agents to one Runtime with
   `Agent::set_runtime()` (before connecting) to stop them as a group.
3. **Process** — `Mads::Runtime::stop_process()` makes *every* Runtime's
   `running()` report false. This is what the SIGINT/SIGTERM handlers
   installed by `Agent::loop()` use, and what the remote-control `shutdown`
   and `restart` commands request. A process-wide stop cannot be undone by
   `Runtime::reset()`.

`Agent::loop()`, the last-known-value drain thread, and the threaded remote
control all keep going only while the agent's Runtime is running *and* no
per-agent stop was requested.

## Typical usage

A single-agent program needs no explicit Runtime management at all:

```cpp
Mads::Agent agent("my_agent", settings_uri);
agent.init();
agent.connect();
agent.loop([&]() {
  // ... publish/receive ...
  return std::chrono::milliseconds(100);
});
agent.shutdown(); // stops this agent only
```

To stop a loop from inside the lambda, stop the agent's own Runtime:

```cpp
agent.loop([&]() -> std::chrono::nanoseconds {
  if (done()) agent.runtime()->stop();
  return std::chrono::nanoseconds(0);
});
```

Two independent agents in one process no longer interfere: each has its own
Runtime, so destroying or disconnecting one leaves the other's loop, drain
thread, and watchdog untouched. If they *should* stop together, group them:

```cpp
Mads::Agent source("source", uri), sink("sink", uri);
sink.set_runtime(source.runtime()); // one shared group
// ...
source.runtime()->stop();           // stops both
```

For an orderly full-process shutdown from your own code (equivalent to
Ctrl-C):

```cpp
Mads::Runtime::stop_process();
```

An application main loop can poll `agent.running()` — the exact condition
`Agent::loop()` checks between iterations (Runtime state plus the per-agent
stop raised by `shutdown()`/`disconnect()`).

## C and Python agents

The C API exposes the same model: `agent_stop()` stops one agent,
`agent_running()` drives a receive loop (`while (agent_running(a)) ...`),
and `mads_stop_process()`/`mads_process_running()` handle the process level.
The Python wrapper mirrors these as `Agent.stop()`, the `Agent.running`
property, and module-level `stop_process()`/`process_running()`.

`Mads::Watcher` participates too: `watch()` now returns after `stop()` is
called or when a process-wide stop is requested, so watcher threads can be
joined cleanly instead of leaked.

## Migrating from `Mads::running`

`Mads::running` still exists as a **deprecated alias of the process-wide run
flag**, so existing code compiles and behaves as before — writing `false`
stops every agent in the process, and reading it reports the process-wide
state. Using it raises a compile-time deprecation warning. Replace it as
follows:

| Legacy | Replacement |
|---|---|
| `Mads::running = false;` (stop everything) | `Mads::Runtime::stop_process();` |
| `Mads::running = false;` (stop one agent's loop) | `agent.runtime()->stop();` |
| `while (Mads::running) { ... }` (app main loop) | `while (Mads::Runtime::process_running()) { ... }` |
| `while (Mads::running) { ... }` (per-agent thread) | `while (agent.runtime()->running()) { ... }` |

One behavioral difference is intentional: agent teardown no longer clears the
process-wide flag. Code that relied on "destroying an agent ends my own
`while (Mads::running)` loop" must now stop the process (or a shared
Runtime) explicitly.
