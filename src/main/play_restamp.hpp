/*
 mads-play's --restamp helper.

 Rewrites the "timestamp"/"timecode" fields of a legacy, header-less
 [topic][snappy(json)] record part (a single Snappy-compressed JSON
 object -- the shape Agent::publish() emits whenever the payload ends up
 Snappy-compressed, see agent.cpp's WireHeader comment) to fresh values,
 leaving every other field, and every other frame shape (an uncompressed
 JSON payload carrying a self-describing header, MsgPack, or a blob's
 meta+bytes parts), byte-for-byte unchanged.

 Header-only so it is directly unit-testable (tests/test_bag_roundtrip.cpp)
 without linking src/main/play.cpp's main(); mirrors the precedent of
 src/main/plugin_migrate.hpp.

 See src/main/play.cpp's file header for the full --restamp rationale/scope.

 Author(s): Paolo Bosetti
*/
#ifndef MADS_MAIN_PLAY_RESTAMP_HPP
#define MADS_MAIN_PLAY_RESTAMP_HPP

#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <nlohmann/json.hpp>
#include <optional>
#include <snappy.h>
#include <string>
#include <vector>

#include "../detail/wire_format.hpp"
#include "../mads.hpp"

namespace Mads {
namespace Play {

// Applies `edit` (json& -> bool "changed") to the JSON object of a legacy,
// header-less single-part frame, plain or Snappy-compressed, and re-encodes
// it the same way. False (parts untouched) for any other shape.
template <typename Edit>
bool edit_legacy_json(std::vector<std::string> &parts, Edit edit) {
  if (parts.size() != 1)
    return false; // extended header or blob (meta+bytes): out of scope

  std::string decompressed;
  const std::string *json_text = &parts[0];
  bool was_compressed = false;
  if (snappy::Uncompress(parts[0].data(), parts[0].size(), &decompressed)) {
    json_text = &decompressed;
    was_compressed = true;
  }

  nlohmann::json payload;
  try {
    payload = nlohmann::json::parse(*json_text);
  } catch (...) {
    return false; // not JSON: leave untouched
  }
  if (!payload.is_object() || !edit(payload))
    return false;

  std::string dumped = payload.dump();
  if (was_compressed) {
    std::string recompressed;
    snappy::Compress(dumped.data(), dumped.size(), &recompressed);
    parts[0] = std::move(recompressed);
  } else {
    parts[0] = std::move(dumped);
  }
  return true;
}

/**
 * @brief Best-effort --restamp rewrite of a raw record's parts, in place.
 *
 * @param parts The record's parts (as from BagRecord::parts /
 *   Agent::receive_raw_message()'s parts out-param), modified in place.
 * @return true if `parts` was rewritten, false if it was left untouched
 *   (more than one part, not valid JSON, or a JSON object without a
 *   "timestamp"/"timecode" field to rewrite).
 */
inline bool try_restamp(std::vector<std::string> &parts) {
  return edit_legacy_json(parts, [](nlohmann::json &payload) {
    auto now = std::chrono::system_clock::now();
    bool touched = false;
    if (payload.contains("timestamp")) {
      payload["timestamp"]["$date"] = Mads::get_ISODate_time(now);
      touched = true;
    }
    if (payload.contains("timecode")) {
      payload["timecode"] = Mads::timecode(now, MADS_FPS);
      touched = true;
    }
    return touched;
  });
}

/**
 * @brief Parses the "$date" text Mads::get_ISODate_time() writes, e.g.
 * "2023-03-30T19:49:53.005+0200". nullopt if it is not in that form.
 */
inline std::optional<std::chrono::system_clock::time_point>
parse_ISODate(const std::string &text) {
  int Y, M, D, h, m, sec, ms, tzh, tzm;
  char sign;
  if (std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d.%3d%c%2d%2d", &Y, &M,
                  &D, &h, &m, &sec, &ms, &sign, &tzh, &tzm) != 10 ||
      (sign != '+' && sign != '-'))
    return std::nullopt;
  std::tm t{};
  t.tm_year = Y - 1900;
  t.tm_mon = M - 1;
  t.tm_mday = D;
  t.tm_hour = h;
  t.tm_min = m;
  t.tm_sec = sec;
#ifdef _WIN32
  const std::time_t as_utc = _mkgmtime(&t);
#else
  const std::time_t as_utc = timegm(&t);
#endif
  if (as_utc == -1)
    return std::nullopt;
  const long offset = (tzh * 3600L + tzm * 60L) * (sign == '-' ? -1 : 1);
  return std::chrono::system_clock::from_time_t(as_utc - offset) +
         std::chrono::milliseconds(ms);
}

/**
 * @brief Moves a payload's "timestamp" ({"$date": ...}) and "timecode"
 * forward by `offset`. The timecode is seconds of the day, so it wraps at
 * midnight as it does live. True if either field was changed.
 */
inline bool shift_times(nlohmann::json &payload,
                        std::chrono::nanoseconds offset) {
  bool touched = false;
  auto ts = payload.find("timestamp");
  if (ts != payload.end() && ts->is_object()) {
    auto date = ts->find("$date");
    if (date != ts->end() && date->is_string()) {
      if (auto tp = parse_ISODate(date->get<std::string>())) {
        *date = Mads::get_ISODate_time(
            std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                *tp + offset));
        touched = true;
      }
    }
  }
  auto tc = payload.find("timecode");
  if (tc != payload.end() && tc->is_number()) {
    *tc = std::fmod(tc->get<double>() +
                        std::chrono::duration<double>(offset).count(),
                    86400.0);
    touched = true;
  }
  return touched;
}

/**
 * @brief --loop helper: shifts the timestamp/timecode of a data record by
 * `offset`, re-encoding it in its own wire format. Handles the legacy
 * header-less [snappy(json)] frame and the self-describing [header]
 * [payload] frame (JSON or MsgPack, compressed or not), reusing the codecs
 * of src/detail/wire_format.hpp. Blobs, shared-memory references and
 * anything else are left untouched (false).
 */
inline bool try_shift(std::vector<std::string> &parts,
                      std::chrono::nanoseconds offset) {
  using namespace Mads::detail;
  WireHeader hdr;
  if (parts.size() == 2 && parse_wire_header(parts[0], hdr)) {
    if (hdr.has_blob)
      return false; // blob or shm reference: no JSON payload to rewrite
    nlohmann::json payload;
    if (!decode_to_json(parts[1], hdr.format, hdr.compression, payload) ||
        !payload.is_object() || !shift_times(payload, offset))
      return false;
    std::string raw =
        encode_payload(payload, static_cast<WireFormat>(hdr.format));
    if (hdr.compression == static_cast<uint8_t>(Comp::Snappy)) {
      std::string compressed;
      snappy::Compress(raw.data(), raw.size(), &compressed);
      raw = std::move(compressed);
    }
    parts[1] = std::move(raw);
    return true;
  }
  return edit_legacy_json(parts, [&](nlohmann::json &payload) {
    return shift_times(payload, offset);
  });
}

} // namespace Play
} // namespace Mads

#endif // MADS_MAIN_PLAY_RESTAMP_HPP
