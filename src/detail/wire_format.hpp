/*
Internal helper: encodes MADS's self-describing wire frame header (see
agent.cpp's original comment, reproduced below) and the payload encoding it
wraps. Extracted out of Agent so the broker can emit an ordinary MADS-format
frame too (ZMQ_DEVELOPMENT.md §2.2, the live subscription table) without
duplicating the format. Not part of the installed SDK (src/detail/ is
excluded from the LIB_HEADERS install glob in CMakeLists.txt).

Self-describing frame header (see REFACTOR.md §1.3 / MSGPACK.md §5.1).

A message published in the *extended* format carries a small header part right
after the topic:

  [ topic ] [ header ] [ payload ]            (data)
  [ topic ] [ header ] [ meta ] [ raw bytes ] (blob, has_blob flag set)

The header begins with the 4-byte magic "MADS" and has a fixed size, which
makes it reliably distinguishable from legacy frames:

  - legacy data:  [ topic ] [ snappy(json) ]               (2 parts)
  - legacy blob:  [ topic ] [ json meta ] [ raw bytes ]    (3 parts)

A reader first checks part #1 for the magic + exact size; if absent it falls
back to the legacy part-count interpretation. Legacy peers never see a header
because the header is only emitted for non-default (e.g. MsgPack) formats.
*/
#pragma once

#include <cstdint>
#include <cstring>
#include <nlohmann/json.hpp>
#include <snappy.h>
#include <string>

#include "../mads.hpp"

namespace Mads::detail {

constexpr char WIRE_MAGIC[4] = {'M', 'A', 'D', 'S'};
constexpr uint8_t WIRE_HDR_VERSION = 1;
constexpr uint8_t WIRE_FLAG_BLOB = 0x01;
constexpr size_t WIRE_HEADER_SIZE = 4 /*magic*/ + 1 /*ver*/ + 1 /*format*/ +
                                    1 /*compression*/ + 1 /*flags*/ +
                                    4 /*schema*/;

enum class Comp : uint8_t { None = 0, Snappy = 1 };

struct WireHeader {
  uint8_t hdr_version = WIRE_HDR_VERSION;
  uint8_t format = static_cast<uint8_t>(WireFormat::Json);
  uint8_t compression = static_cast<uint8_t>(Comp::None);
  bool has_blob = false;
  uint32_t schema = LIB_VERSION_NUM;
};

inline std::string make_wire_header(WireFormat fmt, Comp comp, bool has_blob) {
  std::string h;
  h.reserve(WIRE_HEADER_SIZE);
  h.append(WIRE_MAGIC, 4);
  h.push_back(static_cast<char>(WIRE_HDR_VERSION));
  h.push_back(static_cast<char>(fmt));
  h.push_back(static_cast<char>(comp));
  h.push_back(static_cast<char>(has_blob ? WIRE_FLAG_BLOB : 0));
  uint32_t schema = LIB_VERSION_NUM;
  h.push_back(static_cast<char>((schema >> 24) & 0xFF));
  h.push_back(static_cast<char>((schema >> 16) & 0xFF));
  h.push_back(static_cast<char>((schema >> 8) & 0xFF));
  h.push_back(static_cast<char>(schema & 0xFF));
  return h;
}

// Returns true and fills `out` if `part` is a valid frame header.
inline bool parse_wire_header(const std::string &part, WireHeader &out) {
  if (part.size() != WIRE_HEADER_SIZE)
    return false;
  if (std::memcmp(part.data(), WIRE_MAGIC, 4) != 0)
    return false;
  out.hdr_version = static_cast<uint8_t>(part[4]);
  out.format = static_cast<uint8_t>(part[5]);
  out.compression = static_cast<uint8_t>(part[6]);
  out.has_blob = (static_cast<uint8_t>(part[7]) & WIRE_FLAG_BLOB) != 0;
  out.schema = (static_cast<uint32_t>(static_cast<uint8_t>(part[8])) << 24) |
               (static_cast<uint32_t>(static_cast<uint8_t>(part[9])) << 16) |
               (static_cast<uint32_t>(static_cast<uint8_t>(part[10])) << 8) |
               static_cast<uint32_t>(static_cast<uint8_t>(part[11]));
  return true;
}

// Resolve a compression policy to the concrete codec for a payload of the
// given size. Compression::Auto compresses only at/above the threshold.
inline Comp resolve_compression(Compression policy, size_t size) {
  switch (policy) {
  case Compression::None:
    return Comp::None;
  case Compression::Snappy:
    return Comp::Snappy;
  case Compression::Auto:
  default:
    return size >= COMPRESSION_AUTO_THRESHOLD ? Comp::Snappy : Comp::None;
  }
}

// Encode a JSON object into the bytes for the given wire format.
inline std::string encode_payload(const nlohmann::json &j, WireFormat fmt) {
  if (fmt == WireFormat::MsgPack) {
    auto v = nlohmann::json::to_msgpack(j);
    return std::string(reinterpret_cast<const char *>(v.data()), v.size());
  }
  return j.dump();
}

// Materialise an encoded payload into JSON *text* (the representation the
// rest of MADS expects). Returns false on any decompression/decoding
// failure.
inline bool decode_to_json_text(const std::string &raw, uint8_t format,
                                uint8_t comp, std::string &json_text_out) {
  const std::string *bytes = &raw;
  std::string uncompressed;
  if (comp == static_cast<uint8_t>(Comp::Snappy)) {
    if (!snappy::Uncompress(raw.data(), raw.size(), &uncompressed))
      return false;
    bytes = &uncompressed;
  }
  if (format == static_cast<uint8_t>(WireFormat::MsgPack)) {
    try {
      nlohmann::json j = nlohmann::json::from_msgpack(*bytes);
      json_text_out = j.dump();
    } catch (...) {
      return false;
    }
  } else {
    // Already JSON text (possibly after decompression).
    json_text_out = *bytes;
  }
  return true;
}

} // namespace Mads::detail
