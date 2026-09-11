//! # mads-plugin
//!
//! Safe Rust API for writing MADS plugins.
//!
//! ## Quick start (filter plugin)
//!
//! ```rust,ignore
//! use mads_plugin::prelude::*;
//!
//! #[derive(Default)]
//! struct MyFilter {
//!     scale: f64,
//! }
//!
//! impl FilterPlugin for MyFilter {
//!     fn set_params(&mut self, params: serde_json::Value) {
//!         self.scale = params["scale"].as_f64().unwrap_or(1.0);
//!     }
//!
//!     fn load_data(&mut self, input: serde_json::Value, _topic: &str,
//!                  _blob: Option<&[u8]>) -> Return {
//!         // store input for process()
//!         Return::success()
//!     }
//!
//!     fn process(&mut self) -> (Return, Output) {
//!         (Return::success(), Output::json(serde_json::json!({"value": 42.0 * self.scale})))
//!     }
//! }
//!
//! export_filter_plugin!("my_filter", MyFilter);
//! ```

pub use serde_json;

// ── Return code ───────────────────────────────────────────────────────────────

/// Maps to `mads_return_t` in the C ABI.
#[derive(Debug, Clone)]
pub struct Return {
    pub code: i32,
    pub message: Option<String>,
}

impl Return {
    pub fn success()            -> Self { Self { code: 0, message: None } }
    pub fn retry()              -> Self { Self { code: 1, message: None } }
    pub fn warning(msg: impl Into<String>) -> Self { Self { code: 2, message: Some(msg.into()) } }
    pub fn error(msg: impl Into<String>)   -> Self { Self { code: 3, message: Some(msg.into()) } }
    pub fn critical(msg: impl Into<String>) -> Self { Self { code: 4, message: Some(msg.into()) } }
}

// ── Output ────────────────────────────────────────────────────────────────────

/// Holds the output produced by `process()` / `get_output()`.
pub enum Output {
    /// JSON-only frame.
    Json(serde_json::Value),
    /// JSON metadata + raw binary blob.
    Blob { meta: serde_json::Value, data: Vec<u8> },
    /// No output this iteration.
    Empty,
}

impl Output {
    pub fn json(v: serde_json::Value) -> Self { Self::Json(v) }
    pub fn blob(meta: serde_json::Value, data: Vec<u8>) -> Self { Self::Blob { meta, data } }
    pub fn empty() -> Self { Self::Empty }
}

// ── Plugin traits ─────────────────────────────────────────────────────────────

pub trait FilterPlugin: Default + 'static {
    fn set_params(&mut self, params: serde_json::Value);
    fn info(&self) -> std::collections::HashMap<String, String> {
        Default::default()
    }
    fn load_data(&mut self, input: serde_json::Value, topic: &str,
                 blob: Option<&[u8]>) -> Return;
    fn process(&mut self) -> (Return, Output);
    /// Inter-iteration delay in ms. 0 = as fast as possible, -1 = free-run.
    fn next_loop_ms(&self) -> i64 { 0 }
}

pub trait SourcePlugin: Default + 'static {
    fn set_params(&mut self, params: serde_json::Value);
    fn info(&self) -> std::collections::HashMap<String, String> {
        Default::default()
    }
    /// MIME/format string for binary blob output (e.g. "image/png").
    fn blob_format(&self) -> &str { "" }
    fn get_output(&mut self) -> (Return, Output);
    fn next_loop_ms(&self) -> i64 { 100 }
}

pub trait SinkPlugin: Default + 'static {
    fn set_params(&mut self, params: serde_json::Value);
    fn info(&self) -> std::collections::HashMap<String, String> {
        Default::default()
    }
    fn load_data(&mut self, input: serde_json::Value, topic: &str,
                 blob: Option<&[u8]>) -> Return;
    fn next_loop_ms(&self) -> i64 { 0 }
}

// ── Internal state wrapper ────────────────────────────────────────────────────
// Exported for use inside the macros; not part of the public API.

#[doc(hidden)]
pub mod __private {
    use super::*;
    use std::ffi::{CString, c_char};

    /// Per-instance heap state wrapping the user's plugin struct.
    pub struct PluginState<P> {
        pub inner:      P,
        pub last_json:  Option<CString>,   // serialised output JSON
        pub last_blob:  Option<Vec<u8>>,   // binary output
        pub last_error: Option<CString>,
        pub last_info:  Option<CString>,
        /// NUL-terminated copy of the &str returned by `blob_format()`.
        /// A Rust &str is *not* NUL-terminated and an empty one has a dangling
        /// pointer, so the loader can never be handed `str::as_ptr()` directly.
        pub last_format: Option<CString>,
    }

    impl<P: Default> PluginState<P> {
        pub fn new() -> Self {
            Self {
                inner:      P::default(),
                last_json:  None,
                last_blob:  None,
                last_error: None,
                last_info:  None,
                last_format: None,
            }
        }
    }

    impl<P> PluginState<P> {
        pub fn set_output(&mut self, output: Output) {
            match output {
                Output::Json(v) => {
                    self.last_json = CString::new(v.to_string()).ok();
                    self.last_blob = None;
                }
                Output::Blob { meta, data } => {
                    self.last_json = CString::new(meta.to_string()).ok();
                    self.last_blob = Some(data);
                }
                Output::Empty => {
                    self.last_json = None;
                    self.last_blob = None;
                }
            }
        }

        pub fn set_error(&mut self, msg: String) {
            self.last_error = CString::new(msg).ok();
        }

        pub fn apply_return(&mut self, r: Return) -> i32 {
            if let Some(msg) = r.message {
                self.set_error(msg);
            }
            r.code
        }

        pub fn output_json_ptr(&self) -> *const c_char {
            self.last_json.as_ref().map_or(std::ptr::null(), |s| s.as_ptr())
        }
        pub fn output_json_len(&self) -> usize {
            self.last_json.as_ref().map_or(0, |s| s.as_bytes().len())
        }
        pub fn output_blob_ptr(&self) -> *const u8 {
            self.last_blob.as_ref().map_or(std::ptr::null(), |v| v.as_ptr())
        }
        pub fn output_blob_len(&self) -> usize {
            self.last_blob.as_ref().map_or(0, |v| v.len())
        }
        pub fn error_ptr(&self) -> *const c_char {
            self.last_error.as_ref().map_or(std::ptr::null(), |s| s.as_ptr())
        }
        pub fn info_ptr(&self) -> *const c_char {
            self.last_info.as_ref().map_or(std::ptr::null(), |s| s.as_ptr())
        }

        /// Store a NUL-terminated copy of `fmt` and return a pointer to it.
        /// Returns NULL for an empty format (the ABI allows NULL) or when the
        /// string contains an interior NUL.
        pub fn set_blob_format(&mut self, fmt: &str) -> *const c_char {
            if fmt.is_empty() {
                self.last_format = None;
                return std::ptr::null();
            }
            self.last_format = CString::new(fmt).ok();
            self.last_format.as_ref().map_or(std::ptr::null(), |s| s.as_ptr())
        }

        pub fn set_info<P2>(&mut self, map: std::collections::HashMap<String, String>)
        where
            P2: std::fmt::Debug,  // unused bound, just for hygiene
        {
            let obj: serde_json::Value = map.into_iter()
                .map(|(k, v)| (k, serde_json::Value::String(v)))
                .collect();
            self.last_info = CString::new(obj.to_string()).ok();
        }
    }

    // Safety: PluginState is accessed only from the single thread the loader
    // drives it on.  The raw pointers in CString/Vec never escape the instance.
    pub struct SyncWrapper<T>(pub T);
    unsafe impl<T> Sync for SyncWrapper<T> {}

    /// Mirror of `mads_rust_plugin_t` in Rust — used to build the static vtable.
    #[repr(C)]
    pub struct RawPluginTable {
        pub version:         i32,
        pub name:            *const std::ffi::c_char,
        pub kind:            *const std::ffi::c_char,
        pub create:          Option<unsafe extern "C" fn() -> *mut std::ffi::c_void>,
        pub destroy:         Option<unsafe extern "C" fn(*mut std::ffi::c_void)>,
        pub set_params:      Option<unsafe extern "C" fn(*mut std::ffi::c_void, *const std::ffi::c_char, usize)>,
        pub get_info:        Option<unsafe extern "C" fn(*mut std::ffi::c_void) -> *const std::ffi::c_char>,
        pub blob_format:     Option<unsafe extern "C" fn(*mut std::ffi::c_void) -> *const std::ffi::c_char>,
        pub get_output:      Option<unsafe extern "C" fn(*mut std::ffi::c_void) -> i32>,
        pub load_data:       Option<unsafe extern "C" fn(*mut std::ffi::c_void, *const std::ffi::c_char, usize, *const std::ffi::c_char, *const u8, usize) -> i32>,
        pub process:         Option<unsafe extern "C" fn(*mut std::ffi::c_void) -> i32>,
        pub output_json:     Option<unsafe extern "C" fn(*mut std::ffi::c_void) -> *const std::ffi::c_char>,
        pub output_json_len: Option<unsafe extern "C" fn(*mut std::ffi::c_void) -> usize>,
        pub output_blob:     Option<unsafe extern "C" fn(*mut std::ffi::c_void) -> *const u8>,
        pub output_blob_len: Option<unsafe extern "C" fn(*mut std::ffi::c_void) -> usize>,
        pub last_error:      Option<unsafe extern "C" fn(*mut std::ffi::c_void) -> *const std::ffi::c_char>,
        pub next_loop_ms:    Option<unsafe extern "C" fn(*mut std::ffi::c_void) -> i64>,
    }
}

// ── Public re-exports ─────────────────────────────────────────────────────────

pub mod prelude {
    pub use super::{FilterPlugin, SourcePlugin, SinkPlugin, Return, Output};
    pub use super::{export_filter_plugin, export_source_plugin, export_sink_plugin};
    pub use serde_json;
}

// ── Export macros ─────────────────────────────────────────────────────────────

/// Export a filter plugin.  `$name` is the plugin name string, `$ty` is the
/// type that implements [`FilterPlugin`].
#[macro_export]
macro_rules! export_filter_plugin {
    ($name:expr, $ty:ty) => {
        const _: () = {
            use std::ffi::{CStr, c_char, c_void};
            use $crate::__private::{PluginState, RawPluginTable, SyncWrapper};
            use $crate::{FilterPlugin, Output, Return};

            type State = PluginState<$ty>;

            unsafe extern "C" fn _create() -> *mut c_void {
                Box::into_raw(Box::new(State::new())) as *mut c_void
            }
            unsafe extern "C" fn _destroy(ptr: *mut c_void) {
                drop(Box::from_raw(ptr as *mut State));
            }
            unsafe extern "C" fn _set_params(ptr: *mut c_void, json: *const c_char, len: usize) {
                let s = &mut *(ptr as *mut State);
                let bytes = std::slice::from_raw_parts(json as *const u8, len);
                let v = $crate::serde_json::from_slice(bytes).unwrap_or_default();
                std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| s.inner.set_params(v))).ok();
            }
            unsafe extern "C" fn _get_info(ptr: *mut c_void) -> *const c_char {
                let s = &mut *(ptr as *mut State);
                let map = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| s.inner.info()))
                    .unwrap_or_default();
                let obj: $crate::serde_json::Value = map.into_iter()
                    .map(|(k, v)| (k, $crate::serde_json::Value::String(v)))
                    .collect();
                s.last_info = std::ffi::CString::new(obj.to_string()).ok();
                s.info_ptr()
            }
            unsafe extern "C" fn _load_data(
                ptr: *mut c_void,
                json_in: *const c_char, json_len: usize,
                topic: *const c_char,
                blob_in: *const u8, blob_in_len: usize,
            ) -> i32 {
                let s = &mut *(ptr as *mut State);
                let bytes = std::slice::from_raw_parts(json_in as *const u8, json_len);
                let input = match $crate::serde_json::from_slice(bytes) {
                    Ok(v) => v,
                    Err(e) => { s.set_error(e.to_string()); return 3; }
                };
                let topic_str = CStr::from_ptr(topic).to_str().unwrap_or("");
                let blob_opt: Option<&[u8]> = if blob_in.is_null() { None }
                    else { Some(std::slice::from_raw_parts(blob_in, blob_in_len)) };
                let ret = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    s.inner.load_data(input, topic_str, blob_opt)
                }));
                match ret {
                    Ok(r)  => s.apply_return(r),
                    Err(_) => { s.set_error("panic in load_data".into()); 4 }
                }
            }
            unsafe extern "C" fn _process(ptr: *mut c_void) -> i32 {
                let s = &mut *(ptr as *mut State);
                let ret = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    s.inner.process()
                }));
                match ret {
                    Ok((r, output)) => { s.set_output(output); s.apply_return(r) }
                    Err(_)          => { s.set_error("panic in process".into()); 4 }
                }
            }
            unsafe extern "C" fn _output_json(ptr: *mut c_void) -> *const c_char {
                (*(ptr as *mut State)).output_json_ptr()
            }
            unsafe extern "C" fn _output_json_len(ptr: *mut c_void) -> usize {
                (*(ptr as *mut State)).output_json_len()
            }
            unsafe extern "C" fn _output_blob(ptr: *mut c_void) -> *const u8 {
                (*(ptr as *mut State)).output_blob_ptr()
            }
            unsafe extern "C" fn _output_blob_len(ptr: *mut c_void) -> usize {
                (*(ptr as *mut State)).output_blob_len()
            }
            unsafe extern "C" fn _last_error(ptr: *mut c_void) -> *const c_char {
                (*(ptr as *mut State)).error_ptr()
            }
            unsafe extern "C" fn _next_loop_ms(ptr: *mut c_void) -> i64 {
                let s = &*(ptr as *mut State);
                std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| s.inner.next_loop_ms()))
                    .unwrap_or(0)
            }

            static TABLE: SyncWrapper<RawPluginTable> = SyncWrapper(RawPluginTable {
                version:         1,
                name:            concat!($name, "\0").as_ptr() as *const c_char,
                kind:            "filter\0".as_ptr() as *const c_char,
                create:          Some(_create),
                destroy:         Some(_destroy),
                set_params:      Some(_set_params),
                get_info:        Some(_get_info),
                blob_format:     None,
                get_output:      None,
                load_data:       Some(_load_data),
                process:         Some(_process),
                output_json:     Some(_output_json),
                output_json_len: Some(_output_json_len),
                output_blob:     Some(_output_blob),
                output_blob_len: Some(_output_blob_len),
                last_error:      Some(_last_error),
                next_loop_ms:    Some(_next_loop_ms),
            });

            #[no_mangle]
            pub unsafe extern "C" fn mads_rust_plugin_register()
                -> *const RawPluginTable
            {
                &TABLE.0
            }
        };
    };
}

/// Export a sink plugin.  `$name` is the plugin name string, `$ty` is the
/// type that implements [`SinkPlugin`].
#[macro_export]
macro_rules! export_sink_plugin {
    ($name:expr, $ty:ty) => {
        const _: () = {
            use std::ffi::{CStr, c_char, c_void};
            use $crate::__private::{PluginState, RawPluginTable, SyncWrapper};
            use $crate::{SinkPlugin, Output, Return};

            type State = PluginState<$ty>;

            unsafe extern "C" fn _create() -> *mut c_void {
                Box::into_raw(Box::new(State::new())) as *mut c_void
            }
            unsafe extern "C" fn _destroy(ptr: *mut c_void) {
                drop(Box::from_raw(ptr as *mut State));
            }
            unsafe extern "C" fn _set_params(ptr: *mut c_void, json: *const c_char, len: usize) {
                let s = &mut *(ptr as *mut State);
                let bytes = std::slice::from_raw_parts(json as *const u8, len);
                let v = $crate::serde_json::from_slice(bytes).unwrap_or_default();
                std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| s.inner.set_params(v))).ok();
            }
            unsafe extern "C" fn _get_info(ptr: *mut c_void) -> *const c_char {
                let s = &mut *(ptr as *mut State);
                let map = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| s.inner.info()))
                    .unwrap_or_default();
                let obj: $crate::serde_json::Value = map.into_iter()
                    .map(|(k, v)| (k, $crate::serde_json::Value::String(v)))
                    .collect();
                s.last_info = std::ffi::CString::new(obj.to_string()).ok();
                s.info_ptr()
            }
            unsafe extern "C" fn _load_data(
                ptr: *mut c_void,
                json_in: *const c_char, json_len: usize,
                topic: *const c_char,
                blob_in: *const u8, blob_in_len: usize,
            ) -> i32 {
                let s = &mut *(ptr as *mut State);
                let bytes = std::slice::from_raw_parts(json_in as *const u8, json_len);
                let input = match $crate::serde_json::from_slice(bytes) {
                    Ok(v) => v,
                    Err(e) => { s.set_error(e.to_string()); return 3; }
                };
                let topic_str = CStr::from_ptr(topic).to_str().unwrap_or("");
                let blob_opt: Option<&[u8]> = if blob_in.is_null() { None }
                    else { Some(std::slice::from_raw_parts(blob_in, blob_in_len)) };
                let ret = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    s.inner.load_data(input, topic_str, blob_opt)
                }));
                match ret {
                    Ok(r)  => s.apply_return(r),
                    Err(_) => { s.set_error("panic in load_data".into()); 4 }
                }
            }
            unsafe extern "C" fn _output_json(ptr: *mut c_void) -> *const c_char {
                (*(ptr as *mut State)).output_json_ptr()
            }
            unsafe extern "C" fn _output_json_len(ptr: *mut c_void) -> usize {
                (*(ptr as *mut State)).output_json_len()
            }
            unsafe extern "C" fn _output_blob(ptr: *mut c_void) -> *const u8 {
                (*(ptr as *mut State)).output_blob_ptr()
            }
            unsafe extern "C" fn _output_blob_len(ptr: *mut c_void) -> usize {
                (*(ptr as *mut State)).output_blob_len()
            }
            unsafe extern "C" fn _last_error(ptr: *mut c_void) -> *const c_char {
                (*(ptr as *mut State)).error_ptr()
            }
            unsafe extern "C" fn _next_loop_ms(ptr: *mut c_void) -> i64 {
                let s = &*(ptr as *mut State);
                std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| s.inner.next_loop_ms()))
                    .unwrap_or(0)
            }

            static TABLE: SyncWrapper<RawPluginTable> = SyncWrapper(RawPluginTable {
                version:         1,
                name:            concat!($name, "\0").as_ptr() as *const c_char,
                kind:            "sink\0".as_ptr() as *const c_char,
                create:          Some(_create),
                destroy:         Some(_destroy),
                set_params:      Some(_set_params),
                get_info:        Some(_get_info),
                blob_format:     None,
                get_output:      None,
                load_data:       Some(_load_data),
                process:         None,
                output_json:     Some(_output_json),
                output_json_len: Some(_output_json_len),
                output_blob:     Some(_output_blob),
                output_blob_len: Some(_output_blob_len),
                last_error:      Some(_last_error),
                next_loop_ms:    Some(_next_loop_ms),
            });

            #[no_mangle]
            pub unsafe extern "C" fn mads_rust_plugin_register()
                -> *const RawPluginTable
            {
                &TABLE.0
            }
        };
    };
}

/// Export a source plugin.  `$name` is the plugin name string, `$ty` is the
/// type that implements [`SourcePlugin`].
#[macro_export]
macro_rules! export_source_plugin {
    ($name:expr, $ty:ty) => {
        const _: () = {
            use std::ffi::{c_char, c_void};
            use $crate::__private::{PluginState, RawPluginTable, SyncWrapper};
            use $crate::{SourcePlugin, Output, Return};

            type State = PluginState<$ty>;

            unsafe extern "C" fn _create() -> *mut c_void {
                Box::into_raw(Box::new(State::new())) as *mut c_void
            }
            unsafe extern "C" fn _destroy(ptr: *mut c_void) {
                drop(Box::from_raw(ptr as *mut State));
            }
            unsafe extern "C" fn _set_params(ptr: *mut c_void, json: *const c_char, len: usize) {
                let s = &mut *(ptr as *mut State);
                let bytes = std::slice::from_raw_parts(json as *const u8, len);
                let v = $crate::serde_json::from_slice(bytes).unwrap_or_default();
                std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| s.inner.set_params(v))).ok();
            }
            unsafe extern "C" fn _get_info(ptr: *mut c_void) -> *const c_char {
                let s = &mut *(ptr as *mut State);
                let map = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| s.inner.info()))
                    .unwrap_or_default();
                let obj: $crate::serde_json::Value = map.into_iter()
                    .map(|(k, v)| (k, $crate::serde_json::Value::String(v)))
                    .collect();
                s.last_info = std::ffi::CString::new(obj.to_string()).ok();
                s.info_ptr()
            }
            unsafe extern "C" fn _blob_format(ptr: *mut c_void) -> *const c_char {
                let s = &mut *(ptr as *mut State);
                // Copy into an owned String first: the &str borrowed from the
                // plugin is not NUL-terminated, so it can never cross the ABI.
                let fmt = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    s.inner.blob_format().to_owned()
                })).unwrap_or_default();
                s.set_blob_format(&fmt)
            }
            unsafe extern "C" fn _get_output(ptr: *mut c_void) -> i32 {
                let s = &mut *(ptr as *mut State);
                let ret = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    s.inner.get_output()
                }));
                match ret {
                    Ok((r, output)) => { s.set_output(output); s.apply_return(r) }
                    Err(_)          => { s.set_error("panic in get_output".into()); 4 }
                }
            }
            unsafe extern "C" fn _output_json(ptr: *mut c_void) -> *const c_char {
                (*(ptr as *mut State)).output_json_ptr()
            }
            unsafe extern "C" fn _output_json_len(ptr: *mut c_void) -> usize {
                (*(ptr as *mut State)).output_json_len()
            }
            unsafe extern "C" fn _output_blob(ptr: *mut c_void) -> *const u8 {
                (*(ptr as *mut State)).output_blob_ptr()
            }
            unsafe extern "C" fn _output_blob_len(ptr: *mut c_void) -> usize {
                (*(ptr as *mut State)).output_blob_len()
            }
            unsafe extern "C" fn _last_error(ptr: *mut c_void) -> *const c_char {
                (*(ptr as *mut State)).error_ptr()
            }
            unsafe extern "C" fn _next_loop_ms(ptr: *mut c_void) -> i64 {
                let s = &*(ptr as *mut State);
                std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| s.inner.next_loop_ms()))
                    .unwrap_or(100)
            }

            static TABLE: SyncWrapper<RawPluginTable> = SyncWrapper(RawPluginTable {
                version:         1,
                name:            concat!($name, "\0").as_ptr() as *const c_char,
                kind:            "source\0".as_ptr() as *const c_char,
                create:          Some(_create),
                destroy:         Some(_destroy),
                set_params:      Some(_set_params),
                get_info:        Some(_get_info),
                blob_format:     Some(_blob_format),
                get_output:      Some(_get_output),
                load_data:       None,
                process:         None,
                output_json:     Some(_output_json),
                output_json_len: Some(_output_json_len),
                output_blob:     Some(_output_blob),
                output_blob_len: Some(_output_blob_len),
                last_error:      Some(_last_error),
                next_loop_ms:    Some(_next_loop_ms),
            });

            #[no_mangle]
            pub unsafe extern "C" fn mads_rust_plugin_register()
                -> *const RawPluginTable
            {
                &TABLE.0
            }
        };
    };
}

// ── Tests ─────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::__private::PluginState;
    use std::ffi::CStr;

    #[derive(Default)]
    struct Dummy;

    /// Regression: the loader does `(f && *f) ? string(f) : ""` on the pointer
    /// returned by blob_format().  Handing it `str::as_ptr()` gave a dangling
    /// 0x1 for "" (segfault) and an unterminated buffer otherwise (garbage).
    #[test]
    fn blob_format_empty_is_null() {
        let mut st: PluginState<Dummy> = PluginState::new();
        assert!(st.set_blob_format("").is_null());
    }

    #[test]
    fn blob_format_is_nul_terminated() {
        let mut st: PluginState<Dummy> = PluginState::new();
        let p = st.set_blob_format("image/png");
        assert!(!p.is_null());
        assert_eq!(unsafe { CStr::from_ptr(p) }.to_str().unwrap(), "image/png");
    }

    #[test]
    fn blob_format_survives_until_next_call() {
        let mut st: PluginState<Dummy> = PluginState::new();
        let p = st.set_blob_format("text/csv");
        // ABI contract: valid until the next call on this instance.
        st.set_error("unrelated".into());
        assert_eq!(unsafe { CStr::from_ptr(p) }.to_str().unwrap(), "text/csv");
    }

    #[test]
    fn blob_format_with_interior_nul_is_null() {
        let mut st: PluginState<Dummy> = PluginState::new();
        assert!(st.set_blob_format("bad\0fmt").is_null());
    }
}
