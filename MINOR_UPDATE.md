# Preparing a minor update (e.g. `v2.3.x` → `v2.4.0`)

Checklist of the things that are easy to miss, with the reasoning behind each so
it stays useful when the details change. Ordered roughly as you would work
through them. Patch releases (`v2.4.0` → `v2.4.1`) only need §1, §6 and §7.

---

## 1. The version number itself

**Do not edit any version by hand.** `src/mads.hpp` is generated from
[`src/mads.hpp.in`](src/mads.hpp.in) by `configure_file()` and is not tracked in
git; the values come from `git describe --tags --dirty --exclude=*-g*` in
[`CMakeLists.txt`](CMakeLists.txt) (~L30-64). Bumping the version *is* creating
the annotated tag `vX.Y.Z`.

- CI releases on `tags: ['v*.*.*']` ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)).
- The checkout needs `fetch-depth: 0`, otherwise `git describe` has no tags.
- Building a tree with no tags (or no `.git`) silently falls back to `0.0.1`
  with a CMake warning — check for it if a package comes out mis-versioned.
- A commit *past* a tag becomes `vX.Y.Z-p<n>-g<hash>` and is treated as a
  pre-release; only exact tags are official.

## 2. `LIB_VERSION_CHECK`: a minor bump is a compatibility break

This is the one thing that makes a minor release different from a patch.

`CMakeLists.txt` L64 sets `LIB_VERSION_CHECK` to `MAJOR.MINOR`, and
`Mads::check_version()` compares everything before the last dot. So:

- `v2.3.0`, `v2.3.1`, `v2.3.99` are all mutually compatible;
- **`v2.4.0` rejects every `v2.3.x` consumer**, and vice versa.

Everything that calls `check_version()` — plugins, external agents, downstream
SDK users — has to be rebuilt and re-released against the new minor. Plan that
before tagging, not after. If you did *not* intend a compatibility break, ship a
patch release instead.

## 3. The plugin dependency set

Five places pin plugin-related versions. They must agree, or a freshly
scaffolded plugin and a migrated one end up on different dependencies:

| What | Where |
| --- | --- |
| `mads_plugin` tag used by MADS itself | [`CMakeLists.txt`](CMakeLists.txt) `FetchContent_Populate(plugin ...)` (~L428) |
| Pins for **newly scaffolded** plugins | [`share/plugin_deps.json`](share/plugin_deps.json) |
| Fallbacks when that manifest is missing | [`src/main/make_plugin.cpp`](src/main/make_plugin.cpp) (~L219-221) |
| Pins applied when **migrating** a plugin | [`share/plugin_migrations/*.json`](share/plugin_migrations/) (`cmake.plugin_git_tag`, `cmake.pugg_git_tag`, json version inside `replacements`) |
| json version MADS core itself uses | [`vendors/CMakeLists.txt`](vendors/CMakeLists.txt) |

Note the `mads_plugin` tag embeds the MADS minor version (`v2.3-P6`, `v2.3-P7`,
`v2.4-P8`), so a minor bump usually means a new plugin tag even when the
protocol number does not change.

> **Trap, hit for real in the 2.4.0 cycle.** A migration `replacements` rule
> whose regex targets a form an *earlier* step already rewrote is silently dead:
> `apply_regex()` treats "zero matches" as a no-op, not an error. `P7-P8.json`
> bumped json only via a regex matching the legacy `GIT_TAG v3.11.3` block —
> but `P6-P7.json` had already rewritten that block to the tarball URL form, so
> the bump never ran and a migrated plugin stayed on 3.11.3 while a scaffolded
> one moved to 3.12.0.
>
> **Always test the full chain from the oldest supported protocol**, never just
> the single new step, and assert the *final* versions.

### If the plugin protocol changes (P8 → P9)

1. Add `share/plugin_migrations/P8-P9.json`.
2. Bump `plugin_protocol` **and** `plugin_git_tag` in `share/plugin_deps.json`.
3. Bump the fallbacks in `make_plugin.cpp`.
4. Bump the `mads_plugin` `GIT_TAG` in `CMakeLists.txt`.
5. Update the end-to-end assertions in
   [`tests/test_plugin_migrate.cpp`](tests/test_plugin_migrate.cpp) to the new
   final tag and dependency versions.
6. Update `share/man/mads-plugin.md` if the protocol table is mentioned there.

**Do not "modernise" [`tests/fixtures/plugin_p6/`](tests/fixtures/plugin_p6/).**
It is the migration test's *input*, deliberately frozen at P6 — updating it
makes the end-to-end test vacuous. Its `CMakeLists.txt` must also keep exactly
**one** protocol-suffixed `GIT_TAG` token, because `detect_protocol()` is a
plain, non-comment-aware regex scan (a second such token even inside a `#`
comment would be picked up first).

## 4. Vendored dependency upgrades

Bump in [`vendors/CMakeLists.txt`](vendors/CMakeLists.txt); the mongo drivers
are pinned via `MADS_MONGO_C_DRIVER_VERSION` / `MADS_MONGO_CXX_DRIVER_VERSION`.

- **Verify in a fresh build directory.** A stale `_deps/<dep>-build` tree keeps
  the *previous* version's generated headers (`version.hpp`, `export.hpp`) on
  the include path, where they can shadow the new ones. An incremental build can
  look green while compiling against a mix of both.
- **mongo-cxx-driver specifically:** keep `BUILD_VERSION` pinned. Left at its
  default `0.0.0` it reads `${CMAKE_BINARY_DIR}/VERSION_CURRENT` — a file it
  writes into *our* build tree — so an upgraded driver keeps reporting the old
  version forever; failing that it shells out to a Python script.
- Watch for API renames across major versions (bsoncxx 4.0 renamed
  `bsoncxx::type::k_utf8` → `k_string`).
- After bumping, confirm the configure log prints the version you expect, and
  spot-check a generated `version.hpp`.

## 5. Public header hygiene

`file(GLOB LIB_HEADERS ${SOURCE_DIR}/*.hpp)` in `CMakeLists.txt` is
**non-recursive** and installs *everything* directly under `src/`:

- a new header in `src/` is published to the SDK automatically — intended or not;
- internal headers belong in `src/detail/`, which the glob does not reach.

Installed headers must not leak vendored types (bsoncxx/mongocxx), or SDK users
need the whole driver header tree on their include path and `sizeof()` of our
classes becomes driver-ABI dependent. `Logger` and `MongoFetch` use a pimpl for
exactly this. To verify: compile a TU that includes the public headers with
every vendor `-I`/`-isystem` path stripped.

## 6. Build matrix that must stay green

| Configuration | Why |
| --- | --- |
| default | the shipped build |
| `-DMADS_ENABLE_MONGOCXX=OFF` | must build *and* pass; `Logger` degrades to file-only, queryable at runtime via `Logger::has_mongo_support()` |
| `-DMADS_BUILD_TESTS=ON` | full Catch2 suite, no broker/DB/network needed |
| `-DMADS_COVERAGE=ON` | the coverage job |

Test suites are picked up by a glob — dropping `tests/test_*.cpp` in needs no
CMake edit.

**Coverage:** after restructuring an instrumented source, stale `.gcda` files
flood the build with `cannot merge previous GCDA file: corrupt arc tag` (one
line per mismatched arc — tens of thousands). MadsCore clears them in a
POST_BUILD step; `cmake --build build --target coverage-reset` does it manually.
Stale counters also quietly pollute the report.

## 7. Documentation

- [`CHANGES.md`](CHANGES.md) — rewrite for the new release. Existing convention:
  `# Release vX.Y.Z`, then "what changed between `<prev>` and `<new>`", grouped
  into New features / Changes / Fixes.
- [`share/man/*.md`](share/man/) — any CLI option added or renamed.
- [`README.md`](README.md), [`COMPILE.md`](COMPILE.md),
  [`CONTEXT.md`](CONTEXT.md), [`RUNTIME.md`](RUNTIME.md) — as needed.
- If a minor bump breaks `check_version()` (§2), say so prominently in
  `CHANGES.md`; it is the change most likely to bite users.

## 8. When touching `src/main/*.cpp`

Agent executables are migrating to `AgentAppFor<T>` (see
[`src/agent_app.hpp`](src/agent_app.hpp), and `image.cpp`/`logger.cpp`/
`dealer.cpp` as models). Three things bite:

- `add_common_options()` also declares `-r/--room` (service discovery),
  `-s/-S/--settings-timeout`, `--crypto`, `-v`, `-h`. Check short-flag
  collisions against the agent's own options — `add_agent_identity_options()`
  claims `-n` for `--name`, which clashes with e.g. the logger's `--no-mongo`.
  You can declare `-n,name`/`-i,agent-id` yourself with better help text;
  `AgentApp` looks them up by long name, so it still applies them.
- **Subclasses that override `connect()` with a different default delay**
  (`Worker` and `Dealer` use `0ms`, `Agent` uses `250ms`) must be passed the
  delay explicitly — `AgentApp::connect()` has its own `250ms` default and would
  silently add a settle delay.
- `enable_events()` fires `register_event(startup)` *inside* `connect()`. If an
  agent deliberately registers startup later (e.g. `worker.cpp`, only after its
  plugin loaded successfully), keep the explicit calls instead.

## 9. Before tagging

- [ ] Fresh-tree build + full suite green in every configuration of §6.
- [ ] `cmake --install` into a scratch prefix; confirm the header set is what you
      intend and nothing internal leaked.
- [ ] Scaffold a plugin (`mads plugin -t filter demo`) **and** migrate an old one
      (`mads plugin --update`); confirm both end on identical dependency pins.
- [ ] `CHANGES.md` reflects the actual diff.
- [ ] Tag `vX.Y.Z`, push the tag, watch the CI release job.
