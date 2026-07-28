#include "echo_format.hpp"

#include <chrono>
#include <cstdint>
#include <sstream>

#include "mads.hpp"
#include <rang.hpp>

namespace Mads {

namespace {

const char kBase64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Writes `j` as indented, optionally rang-colored JSON into `os`, mirroring
// nlohmann::json::dump(2)'s layout. Written by hand (rather than colorizing
// dump()'s text output after the fact with regexes, which would risk
// miscoloring punctuation-like characters that happen to appear inside
// string values) so structure and coloring can never disagree. String
// escaping is still delegated to nlohmann (via dump() on a string-only
// json value) rather than reimplemented here.
void write_json_colored(std::ostream &os, const nlohmann::json &j, int depth,
                        bool color) {
  const std::string pad(static_cast<size_t>(depth) * 2, ' ');
  const std::string pad_in(static_cast<size_t>(depth + 1) * 2, ' ');

  if (j.is_object()) {
    if (j.empty()) {
      os << "{}";
      return;
    }
    os << "{\n";
    size_t i = 0, n = j.size();
    for (auto it = j.begin(); it != j.end(); ++it, ++i) {
      os << pad_in;
      if (color) os << rang::fg::cyan;
      os << nlohmann::json(it.key()).dump();
      if (color) os << rang::fg::reset;
      os << ": ";
      write_json_colored(os, it.value(), depth + 1, color);
      if (i + 1 < n) os << ",";
      os << "\n";
    }
    os << pad << "}";
  } else if (j.is_array()) {
    if (j.empty()) {
      os << "[]";
      return;
    }
    os << "[\n";
    for (size_t i = 0; i < j.size(); ++i) {
      os << pad_in;
      write_json_colored(os, j[i], depth + 1, color);
      if (i + 1 < j.size()) os << ",";
      os << "\n";
    }
    os << pad << "]";
  } else if (j.is_string()) {
    if (color) os << rang::fg::green;
    os << j.dump();
    if (color) os << rang::fg::reset;
  } else if (j.is_boolean()) {
    if (color) os << rang::fg::magenta;
    os << (j.get<bool>() ? "true" : "false");
    if (color) os << rang::fg::reset;
  } else if (j.is_null()) {
    if (color) os << rang::style::dim;
    os << "null";
    if (color) os << rang::style::reset;
  } else if (j.is_number()) {
    if (color) os << rang::fg::yellow;
    os << j.dump();
    if (color) os << rang::fg::reset;
  } else {
    os << j.dump();
  }
}

void write_header(std::ostream &os, const std::string &topic, size_t size,
                  bool color) {
  const std::string ts =
      Mads::get_ISODate_time(std::chrono::system_clock::now());
  if (color) os << rang::style::bold << rang::fg::blue;
  os << topic;
  if (color) os << rang::style::reset << rang::fg::reset;
  os << "  ";
  if (color) os << rang::style::dim;
  os << ts;
  if (color) os << rang::style::reset;
  os << "  (" << size << " bytes)\n";
}

} // namespace

std::string base64_encode(const unsigned char *data, size_t len) {
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  size_t i = 0;
  while (i + 3 <= len) {
    const uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                       (static_cast<uint32_t>(data[i + 1]) << 8) |
                       static_cast<uint32_t>(data[i + 2]);
    out += kBase64Table[(n >> 18) & 0x3F];
    out += kBase64Table[(n >> 12) & 0x3F];
    out += kBase64Table[(n >> 6) & 0x3F];
    out += kBase64Table[n & 0x3F];
    i += 3;
  }
  const size_t rem = len - i;
  if (rem == 1) {
    const uint32_t n = static_cast<uint32_t>(data[i]) << 16;
    out += kBase64Table[(n >> 18) & 0x3F];
    out += kBase64Table[(n >> 12) & 0x3F];
    out += "==";
  } else if (rem == 2) {
    const uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                       (static_cast<uint32_t>(data[i + 1]) << 8);
    out += kBase64Table[(n >> 18) & 0x3F];
    out += kBase64Table[(n >> 12) & 0x3F];
    out += kBase64Table[(n >> 6) & 0x3F];
    out += "=";
  }
  return out;
}

std::string format_echo_json(const std::string &topic,
                             const nlohmann::json &payload, size_t wire_size,
                             const EchoRenderOptions &opts) {
  std::ostringstream os;
  if (opts.jsonl) {
    nlohmann::json line;
    line["topic"] = topic;
    line["timestamp"] =
        Mads::get_ISODate_time(std::chrono::system_clock::now());
    line["size"] = wire_size;
    line["type"] = "json";
    line["payload"] = payload;
    os << line.dump() << "\n";
    return os.str();
  }
  write_header(os, topic, wire_size, opts.color);
  write_json_colored(os, payload, 0, opts.color);
  os << "\n\n";
  return os.str();
}

std::string format_echo_blob(const std::string &topic,
                             const std::string &format,
                             const unsigned char *data, size_t len,
                             const EchoRenderOptions &opts) {
  std::ostringstream os;
  if (opts.jsonl) {
    nlohmann::json line;
    line["topic"] = topic;
    line["timestamp"] =
        Mads::get_ISODate_time(std::chrono::system_clock::now());
    line["size"] = len;
    line["type"] = "blob";
    line["format"] = format;
    line["payload"] = opts.raw ? base64_encode(data, len)
                               : ("<blob " + std::to_string(len) + " bytes>");
    os << line.dump() << "\n";
    return os.str();
  }
  write_header(os, topic, len, opts.color);
  if (opts.color) os << rang::fg::yellow;
  if (opts.raw) {
    os << base64_encode(data, len);
  } else {
    os << "<blob " << len << " bytes, format=" << format << ">";
  }
  if (opts.color) os << rang::fg::reset;
  os << "\n\n";
  return os.str();
}

} // namespace Mads
