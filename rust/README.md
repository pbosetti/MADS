# MADS Rust Plugins {#rust_plugins}

Write MADS plugins in safe Rust.  The plugin compiles to a standard shared
library (`.so` / `.dylib`) that is loaded at runtime by dedicated MADS
executables: **`mads-rsource`**, **`mads-rfilter`**, and **`mads-rsink`**.

No C++ toolchain is needed to write or build a plugin.

---

## Prerequisites

| Requirement | Version |
|---|---|
| Rust toolchain | 1.70 + (stable) |
| MADS installation | v2 + (for `mads-rfilter` etc.) |

Install Rust via [rustup](https://rustup.rs) if needed.

---

## Quick start

### 1. Create a new plugin crate

```sh
cargo new --lib my_filter
cd my_filter
```

Edit `Cargo.toml`:

```toml
[lib]
crate-type = ["cdylib"]   # required — produces a .so

[dependencies]
mads-plugin = "0.1"       # once published to crates.io
```

Until the crate is on crates.io, use a local path.  The crate source is
installed alongside MADS under `share/rust/mads-plugin/`:

```toml
# via the MADS system installation
mads-plugin = { path = "/usr/local/share/rust/mads-plugin" }

# or directly from the repository
mads-plugin = { git = "https://github.com/pbosetti/MADS", subdirectory = "rust/mads-plugin" }
```

### 2. Write the plugin (`src/lib.rs`)

```rust
use mads_plugin::prelude::*;

#[derive(Default)]
pub struct MyFilter {
    scale: f64,
}

impl FilterPlugin for MyFilter {
    fn set_params(&mut self, params: serde_json::Value) {
        self.scale = params["scale"].as_f64().unwrap_or(1.0);
    }

    fn load_data(&mut self, mut input: serde_json::Value, _topic: &str,
                 _blob: Option<&[u8]>) -> Return {
        // scale every top-level number
        if let Some(obj) = input.as_object_mut() {
            for v in obj.values_mut() {
                if let Some(n) = v.as_f64() {
                    *v = serde_json::json!(n * self.scale);
                }
            }
        }
        Return::success()
    }

    fn process(&mut self) -> (Return, Output) {
        // return the (already modified) stored frame
        (Return::success(), Output::empty())   // see note below
    }
}

export_filter_plugin!("my_filter", MyFilter);
```

> **Tip:** for a filter, the simplest pattern is to mutate `input` inside
> `load_data` and store it, then emit it from `process`.  `Output::empty()`
> suppresses publication; return `Output::json(value)` to publish.

### 3. Build

```sh
cargo build --release
# → target/release/libmy_filter.so
```

### 4. Run

Copy (or symlink) the `.so` into the MADS lib directory, or pass the full
path on the command line:

```sh
mads-rfilter target/release/libmy_filter.so -o scale=2.5
```

The loader prints the plugin name, kind, and ABI version on startup and then
behaves identically to `mads-filter`.

---

## Plugin kinds

Each kind maps to one MADS loader executable and one trait.

| Kind | Loader | Trait | Produces output? |
|---|---|---|---|
| Source | `mads-rsource` | `SourcePlugin` | Yes (`get_output`) |
| Filter | `mads-rfilter` | `FilterPlugin` | Yes (`process`) |
| Sink | `mads-rsink` | `SinkPlugin` | No |

Only **one** kind can be exported per shared library (one `mads_rust_plugin_register` symbol).

---

## Trait reference

### `FilterPlugin`

```rust
pub trait FilterPlugin: Default + 'static {
    fn set_params(&mut self, params: serde_json::Value);
    fn info(&self) -> HashMap<String, String>;          // optional; shown on startup
    fn load_data(&mut self, input: serde_json::Value,
                 topic: &str, blob: Option<&[u8]>) -> Return;
    fn process(&mut self) -> (Return, Output);
    fn next_loop_ms(&self) -> i64;                      // optional; default 0
}
```

### `SourcePlugin`

```rust
pub trait SourcePlugin: Default + 'static {
    fn set_params(&mut self, params: serde_json::Value);
    fn info(&self) -> HashMap<String, String>;
    fn blob_format(&self) -> &str;                      // optional; MIME hint
    fn get_output(&mut self) -> (Return, Output);
    fn next_loop_ms(&self) -> i64;                      // default 100 ms
}
```

### `SinkPlugin`

```rust
pub trait SinkPlugin: Default + 'static {
    fn set_params(&mut self, params: serde_json::Value);
    fn info(&self) -> HashMap<String, String>;
    fn load_data(&mut self, input: serde_json::Value,
                 topic: &str, blob: Option<&[u8]>) -> Return;
    fn next_loop_ms(&self) -> i64;                      // default 0
}
```

---

## Return codes

| Constructor | Meaning |
|---|---|
| `Return::success()` | Frame processed; publish output if any |
| `Return::retry()` | Skip this iteration silently |
| `Return::warning(msg)` | Publish with a warning field |
| `Return::error(msg)` | Log error, increment error counter, continue |
| `Return::critical(msg)` | Log error and stop the agent |

---

## Output variants

| Constructor | Publishes |
|---|---|
| `Output::json(value)` | JSON frame |
| `Output::blob(meta, bytes)` | JSON metadata + binary blob |
| `Output::empty()` | Nothing (suppresses publication) |

---

## Loader options

The Rust loaders accept the same flags as their C++ counterparts:

```
mads-rfilter [OPTIONS] <plugin.so>

  -n, --name <NAME>       Agent name (default: library stem)
  -i, --agent-id <ID>     Agent ID field added to every outgoing frame
  -p, --period <MS>       Sampling period in ms (source and filter)
  -b, --dont-block        Non-blocking receive (filter and sink)
  -d, --delay <MS>        Initial delay before the first iteration
  -o, --option KEY=VALUE  Override a settings key (repeatable)
      --silent            Suppress the per-message status line
```

Settings from the MADS INI file are also passed to `set_params` as a JSON
object, so plugins can read any key from there without additional CLI flags.

---

## Example plugin

See [`mads-plugin-example/src/lib.rs`](mads-plugin-example/src/lib.rs) for a
working `ScaleFilter` (filter) and commented-out `CounterSource` / `PrintSink`
examples.

Build and inspect the exported symbol:

```sh
cd rust
cargo build --release
nm -D target/release/libmads_plugin_example.so | grep mads_rust
# → T mads_rust_plugin_register
```

---

## ABI notes (for advanced use)

The C header [`src/main/mads_rust_plugin.h`](../src/main/mads_rust_plugin.h)
defines `mads_rust_plugin_t` — a flat struct of function pointers.  The single
exported symbol `mads_rust_plugin_register()` returns a pointer to a static
instance of this struct.  Per-instance state lives in the opaque `void *`
returned by `create()`.

Output buffers (`output_json`, `output_blob`, `last_error`) are owned by the
plugin instance and remain valid until the next call.  The loader never frees
them.

The `export_*_plugin!` macro generates all of this automatically, including
`catch_unwind` guards so that a panic in plugin code becomes a `critical` return
rather than undefined behaviour across the FFI boundary.
