{% include "rust_header.tpl" %}
use mads_plugin::prelude::*;

/// Plugin state — add fields for configuration and per-frame data here.
#[derive(Default)]
pub struct {{class_name}} {
    // example field:
    // scale: f64,
}

impl FilterPlugin for {{class_name}} {
    /// Called once at startup with the full agent settings as a JSON object.
    /// Use this to read parameters from the INI file or -o key=value options.
    fn set_params(&mut self, params: serde_json::Value) {
        // example:
        // self.scale = params["scale"].as_f64().unwrap_or(1.0);
        let _ = params;
    }

    /// Key-value pairs printed at agent startup (shown alongside plugin name).
    fn info(&self) -> std::collections::HashMap<String, String> {
        // example:
        // [("scale".into(), self.scale.to_string())].into()
        Default::default()
    }

    /// Receive an incoming frame before processing.
    ///
    /// Return values:
    ///   Return::success()    – proceed to process()
    ///   Return::retry()      – skip this frame silently
    ///   Return::warning(msg) – proceed, but attach a warning field to output
    ///   Return::error(msg)   – log the error, skip process()
    ///   Return::critical(msg)– log the error and stop the agent
    fn load_data(&mut self, input: serde_json::Value, _topic: &str,
                 _blob: Option<&[u8]>) -> Return {
        // store or validate input here
        let _ = input;
        Return::success()
    }

    /// Produce an output frame from the data loaded by load_data().
    ///
    /// Output variants:
    ///   Output::json(value)       – publish a JSON frame
    ///   Output::blob(meta, bytes) – publish JSON metadata + binary blob
    ///   Output::empty()           – suppress publication this iteration
    fn process(&mut self) -> (Return, Output) {
        let out = serde_json::json!({});
        (Return::success(), Output::json(out))
    }

    /// Inter-iteration delay in ms.  0 = as fast as possible, -1 = free-run.
    fn next_loop_ms(&self) -> i64 { 0 }
}

// Register this struct as a filter plugin with the given name.
// The name must match the stem of the .so file when loaded by mads-rfilter.
export_filter_plugin!("{{name}}", {{class_name}});
