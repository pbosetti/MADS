# Rust plugins

Scaffold with `mads plugin -r -t <source|filter|sink> <name>`. A Rust plugin is
a `cdylib` exposing a small C ABI (`mads_rust_plugin_register`), loaded by
dedicated hosts. Everything in `lifecycle.md` and `return-types.md` applies
unchanged — the hosts share the same loop code — only the surface differs.

**Rust plugin hosts exist on Linux and macOS only**, not on Windows.

## Build and run

```sh
cargo build --release            # produces target/release/lib<name>.so | .dylib
cp target/release/lib<name>.so "$(mads -p)/lib/"
mads rsource lib<name>.so        # or: mads rfilter / mads rsink
```

There is no `.plugin` extension and no `mads inspect_plugin` support: the file
keeps Cargo's `lib<name>.so` naming.

## The traits

```rust
use mads_plugin::prelude::*;

pub trait SourcePlugin: Default + 'static {
    fn set_params(&mut self, params: serde_json::Value);
    fn info(&self) -> HashMap<String, String> { Default::default() }
    fn blob_format(&self) -> &str { "" }
    fn get_output(&mut self) -> (Return, Output);
    fn next_loop_ms(&self) -> i64 { 100 }
}

pub trait FilterPlugin: Default + 'static {
    fn set_params(&mut self, params: serde_json::Value);
    fn info(&self) -> HashMap<String, String> { Default::default() }
    fn load_data(&mut self, input: serde_json::Value, topic: &str,
                 blob: Option<&[u8]>) -> Return;
    fn process(&mut self) -> (Return, Output);
    fn next_loop_ms(&self) -> i64 { 0 }
}

pub trait SinkPlugin: Default + 'static {
    fn set_params(&mut self, params: serde_json::Value);
    fn info(&self) -> HashMap<String, String> { Default::default() }
    fn load_data(&mut self, input: serde_json::Value, topic: &str,
                 blob: Option<&[u8]>) -> Return;
    fn next_loop_ms(&self) -> i64 { 0 }
}
```

Register exactly one struct per library:

```rust
export_source_plugin!("<name>", MyPlugin);   // or export_filter_plugin! / export_sink_plugin!
```

## Differences from C++

| | C++ | Rust |
|---|---|---|
| Construction | `create()` calls your constructor | `Default::default()` — the struct must derive or implement `Default` |
| Error message | assign to `_error`, return a bare enum | the message is part of the value: `Return::error("...")` |
| Output | write into an `out` reference | return `Output::json(v)`, `Output::blob(meta, bytes)` or `Output::empty()` |
| Pacing | `next_loop_duration` member | `next_loop_ms()` method |
| `kind()` | a method that must match the driver name | the name passed to the export macro |
| Several drivers per file | supported | one plugin per library |
| Registration failure | driver not found | ABI-version or kind mismatch, reported at load |

`Return` values map one-to-one onto `return_type`: `Return::success()`,
`retry()`, `warning(msg)`, `error(msg)`, `critical(msg)` — with the same
effects as documented in `return-types.md`.

`Output::empty()` is how a Rust plugin publishes nothing while still reporting
success; in C++ the equivalent is leaving `out` untouched, which the host turns
into a warning frame. Prefer `Return::retry()` when the intent is "nothing to
say this tick".

## Gotchas

- **`next_loop_ms()` returning a negative value means "use the configured
  period"**, exactly like `0`. It is not a distinct free-run mode; free running
  is what you get when no `period` is configured. (The generated template
  comment says otherwise.)
- The exported name is compared against the **agent name** (the INI section, i.e.
  the `.so` stem or `-n`). A mismatch is a startup warning and usually means
  the settings section you edited is not the one being read.
- The library must be a `cdylib`; the generated `Cargo.toml` already sets that.
- Panics must not cross the C ABI boundary. Catch or avoid them, and return
  `Return::error(...)` instead.
