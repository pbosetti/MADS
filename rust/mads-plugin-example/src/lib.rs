//! Example MADS Rust plugins.
//!
//! Only one plugin type can be active per shared library (only one
//! `mads_rust_plugin_register` symbol may be exported).  Comment/uncomment
//! the relevant section and change `crate-type` in Cargo.toml if you want
//! to experiment with different kinds.
//!
//! Build with:
//!   cargo build --release
//!
//! The resulting .so is at target/release/libmads_plugin_example.so
//! Rename/symlink it to <name>.plugin in your MADS lib directory.
//! Load with:
//!   mads-rfilter libmads_plugin_example.so

use mads_plugin::prelude::*;

// ── Filter example ────────────────────────────────────────────────────────────
// Receives every JSON frame, scales numeric values by `scale`, forwards it.

#[derive(Default)]
pub struct ScaleFilter {
    scale: f64,
    last:  Option<serde_json::Value>,
}

impl FilterPlugin for ScaleFilter {
    fn set_params(&mut self, params: serde_json::Value) {
        self.scale = params["scale"].as_f64().unwrap_or(1.0);
    }

    fn info(&self) -> std::collections::HashMap<String, String> {
        [("scale".into(), self.scale.to_string())].into()
    }

    fn load_data(&mut self, mut input: serde_json::Value, _topic: &str,
                 _blob: Option<&[u8]>) -> Return {
        // Scale every top-level numeric value.
        if let Some(obj) = input.as_object_mut() {
            for v in obj.values_mut() {
                if let Some(n) = v.as_f64() {
                    *v = serde_json::json!(n * self.scale);
                }
            }
        }
        self.last = Some(input);
        Return::success()
    }

    fn process(&mut self) -> (Return, Output) {
        match self.last.take() {
            Some(v) => (Return::success(), Output::json(v)),
            None    => (Return::retry(),   Output::empty()),
        }
    }

    fn next_loop_ms(&self) -> i64 { 0 }
}

export_filter_plugin!("scale_filter", ScaleFilter);

// ── Source example (uncomment to use instead of the filter above) ─────────────
//
// #[derive(Default)]
// pub struct CounterSource {
//     count:  u64,
//     period: u64,
// }
//
// impl SourcePlugin for CounterSource {
//     fn set_params(&mut self, params: serde_json::Value) {
//         self.period = params["period"].as_u64().unwrap_or(100);
//     }
//     fn get_output(&mut self) -> (Return, Output) {
//         self.count += 1;
//         let out = serde_json::json!({ "count": self.count });
//         (Return::success(), Output::json(out))
//     }
//     fn next_loop_ms(&self) -> i64 { self.period as i64 }
// }
//
// export_source_plugin!("counter_source", CounterSource);

// ── Sink example (uncomment to use instead of the filter above) ───────────────
//
// #[derive(Default)]
// pub struct PrintSink;
//
// impl SinkPlugin for PrintSink {
//     fn set_params(&mut self, _params: serde_json::Value) {}
//     fn load_data(&mut self, input: serde_json::Value, topic: &str,
//                  _blob: Option<&[u8]>) -> Return {
//         println!("[{topic}] {input}");
//         Return::success()
//     }
// }
//
// export_sink_plugin!("print_sink", PrintSink);
