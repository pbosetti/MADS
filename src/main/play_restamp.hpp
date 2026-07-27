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
#include <nlohmann/json.hpp>
#include <snappy.h>
#include <string>
#include <vector>

#include "../mads.hpp"

namespace Mads {
namespace Play {

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
  if (!payload.is_object())
    return false;

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
  if (!touched)
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

} // namespace Play
} // namespace Mads

#endif // MADS_MAIN_PLAY_RESTAMP_HPP
