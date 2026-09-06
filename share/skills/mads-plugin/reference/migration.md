# Protocol versions and migration

## What the protocol version is

`PLUGIN_PROTOCOL_VERSION` in `common.hpp` numbers the plugin ABI/API contract.
It is incremented whenever the plugin interface changes. A host prints the
version of every plugin it loads and **refuses to load one older than its
minimum**:

```
Fatal error: unsupported plugin protocol version.
Recompile the plugin (see https://github.com/pbosetti/mads_plugin)
```

The version a project builds against is fixed by one line in its
`CMakeLists.txt`:

```cmake
FetchContent_Declare(plugin
  GIT_REPOSITORY https://github.com/pbosetti/mads_plugin.git
  GIT_TAG        v2.4-P8        # ← the P<N> suffix is the protocol version
  ...
)
```

That tag is how tooling detects a project's protocol: `GIT_TAG v*-P<N>`.

## Recognising an outdated plugin

| Signal | Meaning |
|---|---|
| `mads inspect_plugin <file>` exits with `2`, `protocol_current: no` | Built against an older protocol; rebuild or migrate. |
| The `GIT_TAG` in `CMakeLists.txt` has a lower `P<N>` than this MADS | Same, before building. |
| `INSTALL_SOURCE_DRIVER` / `INSTALL_FILTER_DRIVER` / `INSTALL_SINK_DRIVER` in the source | Pre-P8 registration macros. They still compile (they forward to `MADS_REGISTER_PLUGINS`, ignoring their type arguments) but should be replaced. |
| `create()` returning a raw pointer | Pre-P8. Drivers now return `std::unique_ptr`. |
| A `kind()` that selects a settings section | Pre-P8 behaviour. Since P8 the section is the agent name and `kind()` is only a consistency check. |
| `json_found` differs from `json_expected` in `inspect_plugin` | Not a protocol issue but the same class of problem: rebuild against the pinned `nlohmann/json`. |

## Migrating

```bash
mads plugin --update --dir <plugin-project>   # migrate in place
mads plugin --update --dir <project> --dry-run # show what would change
mads plugin --update --dir <project> --no-check # skip the post-migration build
mads plugin --update --dir <project> --from 6 --to 8   # force the range
```

The migration is data-driven (one JSON step per protocol bump) and does three
things: rewrites `CMakeLists.txt` pins, rewrites method signatures in `src/`
structurally, and then **compiles the result** to prove the rewrite is valid.
Originals are kept as `*.bak`. Anything the tool cannot do mechanically is
printed as a `MANUAL FOLLOW-UP` note — read those, they are not optional.

`--update` also refreshes this skill so the agent documentation in the project
matches the protocol the project now targets.

## When adding a new protocol version (MADS maintainers)

The pins live in `share/plugin_deps.json`; the per-step rewrites live in
`share/plugin_migrations/P<N>-P<N+1>.json`. Both must be updated together with
`PLUGIN_PROTOCOL_VERSION`, or a freshly scaffolded plugin and a migrated one
end up on different dependencies.
