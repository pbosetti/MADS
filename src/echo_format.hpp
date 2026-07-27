/*
  _____     _           _____                          _
 | ____|___| |__   ___ |  ___|__  _ __ _ __ ___   __ _| |_
 |  _| / __| '_ \ / _ \| |_ / _ \| '__| '_ ` _ \ / _` | __|
 | |__| (__| | | | (_) |  _| (_) | |  | | | | | | (_| | |_
 |_____\___|_| |_|\___/|_|  \___/|_|  |_| |_| |_|\__,_|\__|

Pure, ZMQ/Agent-free rendering of one received `mads echo` message: pretty
(optionally rang-colored) multi-line JSON or a one-line blob summary, or a
single compact JSON line (`--jsonl`) of either. Factored out of
src/main/echo.cpp so the exact formatting logic the executable prints is
directly callable (and testable) without spawning a subprocess -- see
tests/test_echo_loopback.cpp.

Author(s): Paolo Bosetti
*/
#pragma once

#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>

namespace Mads {

/**
 * @brief Rendering choices for format_echo_json()/format_echo_blob(),
 * mirroring `mads echo`'s CLI flags.
 */
struct EchoRenderOptions {
  /// Show the exact blob bytes (base64-encoded) instead of a one-line
  /// "<blob N bytes>" summary. Applies to blob messages only.
  bool raw = false;
  /// Emit a single compact JSON line (topic/timestamp/size/type/payload)
  /// instead of the multi-line pretty form. Intended for piping into `jq`.
  bool jsonl = false;
  /// Emit rang ANSI color codes around the pretty-printed form. Ignored
  /// (never colored) when jsonl is set, since colored output would corrupt
  /// the JSON stream a consumer like `jq` expects.
  bool color = true;
};

/**
 * @brief Formats one received JSON message for `mads echo` stdout.
 *
 * @param topic The message's topic.
 * @param payload The already-decoded JSON payload.
 * @param wire_size Size, in bytes, of the message as it arrived on the wire
 *   (independent of the pretty-printed width).
 * @param opts Rendering choices.
 * @return The complete, newline-terminated text to print.
 */
std::string format_echo_json(const std::string &topic,
                             const nlohmann::json &payload, size_t wire_size,
                             const EchoRenderOptions &opts);

/**
 * @brief Formats one received blob message for `mads echo` stdout.
 *
 * @param topic The message's topic.
 * @param format The blob's declared format (e.g. "raw"), from its metadata;
 *   "raw" is used when the metadata did not specify one.
 * @param data Pointer to the blob bytes (may be null iff len == 0).
 * @param len Number of bytes in the blob.
 * @param opts Rendering choices.
 * @return The complete, newline-terminated text to print.
 */
std::string format_echo_blob(const std::string &topic,
                             const std::string &format,
                             const unsigned char *data, size_t len,
                             const EchoRenderOptions &opts);

/**
 * @brief Standard base64 encoding (RFC 4648, with `=` padding).
 *
 * Exposed mainly so tests can decode format_echo_*'s `--raw` output without
 * duplicating the alphabet; `mads echo` itself only ever encodes.
 */
std::string base64_encode(const unsigned char *data, size_t len);

} // namespace Mads
