/*
  MADS Rust Plugin ABI
  ====================
  C header defining the vtable that every Rust (or plain-C) plugin must export.

  The sole exported symbol is:
      const mads_rust_plugin_t *mads_rust_plugin_register(void);

  It returns a pointer to a static vtable.  Per-instance state lives in the
  opaque void* returned by create() and freed by destroy().

  Output buffers (output_json, output_blob, last_error, get_info) are owned by
  the plugin instance and remain valid until the next call on that instance.
  The loader never frees them directly.
*/
#pragma once
#include <stddef.h>
#include <stdint.h>

#define MADS_RUST_PLUGIN_VERSION 1

/* Return codes — mirror Mads::return_type */
typedef int mads_return_t;
#define MADS_SUCCESS  0
#define MADS_RETRY    1
#define MADS_WARNING  2
#define MADS_ERROR    3
#define MADS_CRITICAL 4

typedef struct mads_rust_plugin_t {
  /* Must equal MADS_RUST_PLUGIN_VERSION; loader rejects mismatches. */
  int version;

  /* Static null-terminated strings identifying the plugin. */
  const char *name; /* e.g. "my_filter" */
  const char *kind; /* "source" | "filter" | "sink" */

  /* ── Lifecycle ─────────────────────────────────────────────────────────── */
  void *(*create)(void);
  void  (*destroy)(void *self);

  /* ── Configuration ─────────────────────────────────────────────────────── */
  /* Called once after create() with a JSON settings object (UTF-8, not NUL-
     terminated; use json_len). */
  void (*set_params)(void *self, const char *json, size_t json_len);

  /* ── Metadata ──────────────────────────────────────────────────────────── */
  /* Returns a JSON object {"key":"value",...} in an internal buffer.
     Valid until the next call on this instance. */
  const char *(*get_info)(void *self);

  /* ── Source-only ───────────────────────────────────────────────────────── */
  /* Returns the blob MIME/format string (static, may be NULL or ""). */
  const char *(*blob_format)(void *self);

  /* Produce one output frame.  Output is accessed via output_json / output_blob
     accessors below.  NULL for filter and sink plugins. */
  mads_return_t (*get_output)(void *self);

  /* ── Filter + Sink ─────────────────────────────────────────────────────── */
  /* Load an incoming frame.
     - json_in  : serialised JSON frame (UTF-8), length json_len
     - topic    : NUL-terminated pub-sub topic string
     - blob_in  : optional binary blob (NULL when none)
     - blob_in_len : byte length of blob_in */
  mads_return_t (*load_data)(void *self,
                             const char  *json_in, size_t json_len,
                             const char  *topic,
                             const uint8_t *blob_in, size_t blob_in_len);

  /* ── Filter-only ───────────────────────────────────────────────────────── */
  /* Transform the last loaded frame into an output frame.
     Output is accessed via output_json / output_blob accessors.
     NULL for source and sink plugins. */
  mads_return_t (*process)(void *self);

  /* ── Output accessors ──────────────────────────────────────────────────── */
  /* Valid after a successful get_output() or process() call.
     Buffers are owned by the plugin instance; do not free them. */
  const char    *(*output_json)(void *self);
  size_t         (*output_json_len)(void *self);
  const uint8_t *(*output_blob)(void *self);     /* NULL when no blob */
  size_t         (*output_blob_len)(void *self);

  /* ── Error accessor ────────────────────────────────────────────────────── */
  /* NUL-terminated string valid after any non-success return code. */
  const char *(*last_error)(void *self);

  /* ── Timing hint ───────────────────────────────────────────────────────── */
  /* Milliseconds the loader should wait before the next iteration.
       -1  : free-run (let the loop decide)
        0  : as fast as possible
       >0  : sleep this many ms */
  long (*next_loop_ms)(void *self);
} mads_rust_plugin_t;

/* ── Entry point ─────────────────────────────────────────────────────────── */
/* Every Rust plugin .so must export exactly this symbol. */
#ifdef __cplusplus
extern "C"
#endif
const mads_rust_plugin_t *mads_rust_plugin_register(void);
