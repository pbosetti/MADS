{% include "rust_header.tpl" %}
use mads_plugin::prelude::*;

/// Plugin state — add fields for configuration and internal counters here.
#[derive(Default)]
pub struct {{class_name}} {
    // example fields:
    // count: u64,
    // period_ms: i64,
}

impl SourcePlugin for {{class_name}} {
    /// Called once at startup with the full agent settings as a JSON object.
    fn set_params(&mut self, params: serde_json::Value) {
        // example:
        // self.period_ms = params["period"].as_i64().unwrap_or(100);
        let _ = params;
    }

    /// Key-value pairs printed at agent startup.
    fn info(&self) -> std::collections::HashMap<String, String> {
        Default::default()
    }

    /// MIME/format hint for binary blob output (e.g. "image/png").
    /// Return an empty string when this source produces JSON-only frames.
    fn blob_format(&self) -> &str { "" }

    /// Produce one output frame per call.
    ///
    /// Return values:
    ///   Return::success()    – publish the Output
    ///   Return::retry()      – skip this tick silently
    ///   Return::warning(msg) – publish with a warning field attached
    ///   Return::error(msg)   – log the error, do not publish
    ///   Return::critical(msg)– log the error and stop the agent
    ///
    /// Output variants:
    ///   Output::json(value)       – publish a JSON frame
    ///   Output::blob(meta, bytes) – publish JSON metadata + binary blob
    ///   Output::empty()           – suppress publication this tick
    fn get_output(&mut self) -> (Return, Output) {
        // example:
        // self.count += 1;
        // let out = serde_json::json!({ "count": self.count });
        let out = serde_json::json!({});
        (Return::success(), Output::json(out))
    }

    /// Inter-iteration delay in ms.  Controls the publication rate.
    fn next_loop_ms(&self) -> i64 { 100 }
}

export_source_plugin!("{{name}}", {{class_name}});
