# {{name}} — working notes for AI coding agents

This is a **MADS {{type_lower}} plugin**{% if rust %} written in Rust{% else %} written in C++17{% endif %}.

Framework context — the plugin lifecycle, the order in which the host agent
calls each method, what every return value does, how settings reach the plugin,
topics, blobs, testing, deployment and protocol migration — is in the
`mads-plugin` skill under [`.claude/skills/mads-plugin/`](.claude/skills/mads-plugin/SKILL.md).
**Read `SKILL.md` before changing plugin code**, and follow its `reference/`
files rather than inferring host behaviour from the template comments.

Regenerate that skill for a newer MADS with `mads plugin --update`.

## This plugin

<Describe what this plugin does: what it reads, what it publishes, and why.>

- Behavior: **{{type_lower}}** — host agent `mads {% if rust %}r{% endif %}{{type_lower}}`
- Class: `{{class_name}}`{% if rust %} in [`src/lib.rs`](src/lib.rs){% else %} in [`src/{{source_file}}`](src/{{source_file}}){% endif %}
- Driver name: `{{name}}`{% if rust %} — the name passed to the export macro; it must match the `.so` stem the loader is given{% else %} — must stay equal to the CMake target name, so that `kind()` and the file stem agree{% endif %}
- INI section: `[{{name}}]` (or whatever `-n` selects at run time)

## Parameters

<List every INI key the plugin reads, its type, default and unit. Keep this
table and the README in sync — it is the plugin's public interface.>

| Key | Type | Default | Meaning |
|---|---|---|---|
|  |  |  |  |

## Output frames

<Describe the JSON the plugin publishes (or consumes, for a sink): the fields,
their units, and the topic they go to.>

## Conventions for this project

{% if rust %}- Rust 2021, built with Cargo; format with `rustfmt`, lint with `clippy`.
- The crate is a `cdylib`; exactly one plugin struct is exported per library.
- Do not add third-party crates without asking. `serde_json` is already
  available through `mads_plugin::prelude`.
- Keep unit tests in a `#[cfg(test)]` module in `src/lib.rs`, exercising the
  plugin the way the host does: `set_params()`, then `load_data()`/`process()`
  or `get_output()`. Assert on both the returned `Return` and the payload.
- A panic must never cross the plugin boundary: return `Return::error(...)`.
{% else %}- C++17, built with CMake; LLVM formatting, two-space indent.
- `CamelCase` for classes and namespaces, `snake_case` for methods and
  variables, `_leading_underscore` for private members, declared last.
- Do not add third-party dependencies without asking. `nlohmann/json` is
  already available, and the plugin base classes also provide a `SerialPort`
  helper in `serialport.hpp`.
- All plugin logic must be reachable from the test `main()` at the bottom of
  the source file, and that test must be deterministic and self-checking:
  assert on both the returned status and the payload, and exit non-zero on
  failure.
- An exception must never escape a plugin method: catch it, set `_error` and
  return `return_type::error`.
{% endif %}- Never block, sleep or busy-wait inside a plugin method.

## Build and test

```sh
{% if rust %}cargo build --release
mads r{{type_lower}} target/release/lib{{name}}.so{% else %}cmake -Bbuild -DCMAKE_INSTALL_PREFIX="$(mads -p)"
cmake --build build -j4
./build/{{name}}                       # the standalone test driver
mads inspect_plugin build/{{name}}.plugin
mads {{type_lower}} build/{{name}}.plugin{% endif %}
```
