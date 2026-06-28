{% include "rust_header.tpl" %}
use mads_plugin::prelude::*;

/// Plugin state — add fields for configuration and accumulated data here.
#[derive(Default)]
pub struct {{class_name}} {
    // example field:
    // count: u64,
}

impl SinkPlugin for {{class_name}} {
    /// Called once at startup with the full agent settings as a JSON object.
    fn set_params(&mut self, params: serde_json::Value) {
        let _ = params;
    }

    /// Key-value pairs printed at agent startup.
    fn info(&self) -> std::collections::HashMap<String, String> {
        Default::default()
    }

    /// Consume an incoming frame.  Sinks do not produce output.
    ///
    /// Return values:
    ///   Return::success()    – frame handled
    ///   Return::retry()      – ignore this frame silently
    ///   Return::warning(msg) – log the warning and continue
    ///   Return::error(msg)   – log the error and continue
    ///   Return::critical(msg)– log the error and stop the agent
    fn load_data(&mut self, input: serde_json::Value, topic: &str,
                 _blob: Option<&[u8]>) -> Return {
        // consume the frame, e.g. write to a file or database
        let _ = (input, topic);
        Return::success()
    }

    /// Inter-iteration delay in ms.  Usually 0 for event-driven sinks.
    fn next_loop_ms(&self) -> i64 { 0 }
}

export_sink_plugin!("{{name}}", {{class_name}});
