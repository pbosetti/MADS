#include "bag.hpp"

#include <array>
#include <cstring>

namespace Mads {

namespace {

constexpr uint64_t HEADER_SIZE = 12;  // magic(4) + format_ver(4) + flags(4)
constexpr uint64_t TRAILER_SIZE = 16; // magic(4) + footer_offset(8) + format_ver(4)
constexpr uint64_t FOOTER_FIXED_SIZE =
    4 /*magic*/ + 8 /*count*/ + 8 /*first_ts*/ + 8 /*last_ts*/;
constexpr uint32_t FLAG_CRC = 0x1u;

// ---------------------------------------------------------------------
// Explicit little-endian (de)serialization -- never a raw struct memcpy,
// so the format is identical regardless of host endianness/padding.
// ---------------------------------------------------------------------

void put_u32(std::string &buf, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    buf.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

void put_u64(std::string &buf, uint64_t v) {
  for (int i = 0; i < 8; ++i)
    buf.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

void put_i64(std::string &buf, int64_t v) {
  put_u64(buf, static_cast<uint64_t>(v));
}

uint32_t get_u32(const unsigned char *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t get_u64(const unsigned char *p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i)
    v = (v << 8) | static_cast<uint64_t>(p[i]);
  return v;
}

int64_t get_i64(const unsigned char *p) {
  return static_cast<int64_t>(get_u64(p));
}

// Reads exactly `n` bytes into `dst`. Always clears the stream's eof/fail
// state afterwards (whether or not the read fully succeeded) so a
// subsequent seekg() is never a silent no-op on a stream left in a failed
// state -- required by both the footer probe and the linear scan, which
// routinely seek back and forth and must tolerate short reads at EOF.
bool try_read(std::ifstream &in, char *dst, std::streamsize n) {
  in.read(dst, n);
  bool ok = in.gcount() == n;
  in.clear();
  return ok;
}

// CRC-32 (IEEE 802.3 / zlib / gzip / PNG polynomial 0xEDB88320), computed
// with a lazily-built, thread-safe (C++11 magic-static) table. Used to
// detect a corrupt record; not a dependency on zlib or any other library.
const std::array<uint32_t, 256> &crc32_table() {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k)
        c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      t[i] = c;
    }
    return t;
  }();
  return table;
}

uint32_t crc32_of(const void *data, size_t len) {
  auto const &table = crc32_table();
  const auto *p = static_cast<const unsigned char *>(data);
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i)
    crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

} // namespace

// ===========================================================================
// BagWriter
// ===========================================================================

BagWriter::BagWriter(const std::filesystem::path &path, bool enable_crc)
    : _path(path), _crc_enabled(enable_crc) {
  _out.open(path, std::ios::binary | std::ios::trunc);
  if (!_out)
    throw BagError("Cannot open bag file '" + path.string() + "' for writing");

  std::string hdr;
  hdr += 'M';
  hdr += 'B';
  hdr += 'A';
  hdr += 'G';
  put_u32(hdr, BAG_FORMAT_VERSION);
  put_u32(hdr, enable_crc ? FLAG_CRC : 0u);
  _out.write(hdr.data(), static_cast<std::streamsize>(hdr.size()));
  if (!_out)
    throw BagError("Failed to write bag header to '" + path.string() + "'");
}

BagWriter::~BagWriter() {
  try {
    close();
  } catch (...) {
    // Destructors must not throw; an explicit close() call is how callers
    // that care about a failed flush/footer write observe the error.
  }
}

void BagWriter::write(int64_t timestamp_ns, const std::string &topic,
                       const std::vector<std::string> &parts) {
  if (_closed)
    throw BagError("BagWriter::write() called after close() on '" +
                    _path.string() + "'");

  std::string body;
  put_i64(body, timestamp_ns);
  put_u32(body, static_cast<uint32_t>(topic.size()));
  body += topic;
  put_u32(body, static_cast<uint32_t>(parts.size()));
  for (auto const &part : parts) {
    put_u32(body, static_cast<uint32_t>(part.size()));
    body += part;
  }

  uint32_t record_len = static_cast<uint32_t>(body.size());
  if (_crc_enabled)
    record_len += 4;

  uint64_t offset = static_cast<uint64_t>(_out.tellp());
  std::string len_field;
  put_u32(len_field, record_len);
  _out.write(len_field.data(), static_cast<std::streamsize>(len_field.size()));
  _out.write(body.data(), static_cast<std::streamsize>(body.size()));
  if (_crc_enabled) {
    std::string crc_field;
    put_u32(crc_field, crc32_of(body.data(), body.size()));
    _out.write(crc_field.data(), static_cast<std::streamsize>(crc_field.size()));
  }
  if (!_out)
    throw BagError("Failed to write record to bag file '" + _path.string() + "'");

  _offsets.push_back(offset);
  if (!_first_ts)
    _first_ts = timestamp_ns;
  _last_ts = timestamp_ns;
}

void BagWriter::close() {
  if (_closed)
    return;
  _closed = true;

  uint64_t footer_offset = static_cast<uint64_t>(_out.tellp());
  std::string footer;
  footer += 'M';
  footer += 'B';
  footer += 'F';
  footer += 'T';
  put_u64(footer, static_cast<uint64_t>(_offsets.size()));
  put_i64(footer, _first_ts.value_or(0));
  put_i64(footer, _last_ts.value_or(0));
  for (auto off : _offsets)
    put_u64(footer, off);
  _out.write(footer.data(), static_cast<std::streamsize>(footer.size()));

  std::string trailer;
  trailer += 'M';
  trailer += 'B';
  trailer += 'T';
  trailer += 'R';
  put_u64(trailer, footer_offset);
  put_u32(trailer, BAG_FORMAT_VERSION);
  _out.write(trailer.data(), static_cast<std::streamsize>(trailer.size()));
  _out.flush();

  if (!_out)
    throw BagError("Failed to write footer/trailer to bag file '" +
                    _path.string() + "'");
}

// ===========================================================================
// BagReader
// ===========================================================================

BagReader::BagReader(const std::filesystem::path &path) : _path(path) {
  _in.open(path, std::ios::binary);
  if (!_in)
    throw BagError("Cannot open bag file '" + path.string() + "' for reading");

  _in.seekg(0, std::ios::end);
  if (!_in)
    throw BagError("Cannot seek bag file '" + path.string() + "'");
  uint64_t file_size = static_cast<uint64_t>(_in.tellg());
  _in.clear();
  _in.seekg(0, std::ios::beg);

  if (file_size < HEADER_SIZE)
    throw BagError("Bag file '" + path.string() +
                    "' is too small to contain a valid header");

  unsigned char header[HEADER_SIZE];
  if (!try_read(_in, reinterpret_cast<char *>(header), HEADER_SIZE))
    throw BagError("Failed to read bag header from '" + path.string() + "'");
  if (!(header[0] == 'M' && header[1] == 'B' && header[2] == 'A' &&
        header[3] == 'G'))
    throw BagError("Bad magic in bag file '" + path.string() +
                    "' (not a MADS bag file)");

  _format_ver = get_u32(header + 4);
  if (_format_ver == 0 || _format_ver > BAG_FORMAT_VERSION)
    throw BagError("Unsupported bag format_ver " +
                    std::to_string(_format_ver) + " in '" + path.string() +
                    "' (this reader supports up to " +
                    std::to_string(BAG_FORMAT_VERSION) + ")");

  uint32_t flags = get_u32(header + 8);
  _crc_enabled = (flags & FLAG_CRC) != 0;

  _has_index = file_size >= HEADER_SIZE + TRAILER_SIZE && try_load_footer(file_size);
  if (!_has_index) {
    linear_scan(file_size);
  }
}

bool BagReader::try_load_footer(uint64_t file_size) {
  _in.clear();
  _in.seekg(static_cast<std::streamoff>(file_size - TRAILER_SIZE), std::ios::beg);
  unsigned char trailer[TRAILER_SIZE];
  if (!try_read(_in, reinterpret_cast<char *>(trailer), TRAILER_SIZE))
    return false;
  if (!(trailer[0] == 'M' && trailer[1] == 'B' && trailer[2] == 'T' &&
        trailer[3] == 'R'))
    return false;

  uint64_t footer_offset = get_u64(trailer + 4);
  uint32_t trailer_fmt_ver = get_u32(trailer + 12);
  if (trailer_fmt_ver != _format_ver)
    return false; // inconsistent trailer: don't trust it, fall back

  uint64_t footer_region_end = file_size - TRAILER_SIZE;
  if (footer_offset < HEADER_SIZE || footer_offset > footer_region_end)
    return false;

  uint64_t footer_size = footer_region_end - footer_offset;
  if (footer_size < FOOTER_FIXED_SIZE)
    return false;

  _in.clear();
  _in.seekg(static_cast<std::streamoff>(footer_offset), std::ios::beg);
  std::string footer_buf(static_cast<size_t>(footer_size), '\0');
  if (!try_read(_in, footer_buf.data(), static_cast<std::streamsize>(footer_size)))
    return false;

  const auto *p = reinterpret_cast<const unsigned char *>(footer_buf.data());
  if (!(p[0] == 'M' && p[1] == 'B' && p[2] == 'F' && p[3] == 'T'))
    return false;

  uint64_t record_count = get_u64(p + 4);
  int64_t first_ts = get_i64(p + 12);
  int64_t last_ts = get_i64(p + 20);

  // record_count must exactly account for the rest of the footer (the
  // per-record offset index); anything else means the footer/trailer pair
  // is inconsistent with the file, so it can't be trusted.
  if (record_count > (footer_size - FOOTER_FIXED_SIZE) / 8)
    return false;
  uint64_t expected_size = FOOTER_FIXED_SIZE + record_count * 8;
  if (expected_size != footer_size)
    return false;

  std::vector<uint64_t> offsets;
  offsets.reserve(static_cast<size_t>(record_count));
  const unsigned char *idx_p = p + FOOTER_FIXED_SIZE;
  for (uint64_t i = 0; i < record_count; ++i) {
    uint64_t off = get_u64(idx_p + i * 8);
    // Every indexed record must sit strictly between the header and the
    // start of the footer, or the index does not describe this file.
    if (off < HEADER_SIZE || off >= footer_offset)
      return false;
    offsets.push_back(off);
  }

  _offsets = std::move(offsets);
  if (record_count > 0) {
    _first_ts = first_ts;
    _last_ts = last_ts;
  }
  return true;
}

void BagReader::linear_scan(uint64_t file_size) {
  std::vector<uint64_t> offsets;
  std::optional<int64_t> first_ts, last_ts;
  uint64_t offset = HEADER_SIZE;

  while (offset < file_size) {
    auto rec = read_record_at(offset, file_size, /*strict=*/false);
    if (!rec)
      break; // incomplete or corrupt trailing record: stop, keep the rest

    offsets.push_back(offset);
    if (!first_ts)
      first_ts = rec->timestamp_ns;
    last_ts = rec->timestamp_ns;

    // Re-read the 4-byte length prefix (already proven to fit by the
    // successful decode above) just to compute how far to advance --
    // cheap, and keeps the on-disk size computation in one place
    // (read_record_at) instead of duplicating it here.
    _in.clear();
    _in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    unsigned char len_buf[4];
    try_read(_in, reinterpret_cast<char *>(len_buf), 4);
    uint32_t record_len = get_u32(len_buf);
    offset += 4 + record_len;
  }

  _offsets = std::move(offsets);
  _first_ts = first_ts;
  _last_ts = last_ts;
  // Anything left over (a partial record, or unrecognized trailing bytes
  // such as a footer/trailer that try_load_footer() couldn't validate)
  // means data was not fully recovered.
  _truncated = offset < file_size;
}

std::optional<BagRecord> BagReader::read_record_at(uint64_t offset,
                                                    uint64_t file_size,
                                                    bool strict) const {
  auto fail = [&](const std::string &msg) -> std::optional<BagRecord> {
    if (strict)
      throw BagError(msg);
    return std::nullopt;
  };

  constexpr uint64_t LEN_FIELD = 4;
  if (offset + LEN_FIELD > file_size)
    return fail("Bag record at offset " + std::to_string(offset) +
                " is truncated (missing length prefix)");

  _in.clear();
  _in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  unsigned char len_buf[4];
  if (!try_read(_in, reinterpret_cast<char *>(len_buf), 4))
    return fail("Failed to read record length at offset " +
                std::to_string(offset));
  uint32_t record_len = get_u32(len_buf);

  uint64_t min_len = 8 + 4 + 4 + (_crc_enabled ? 4 : 0);
  if (record_len < min_len)
    return fail("Bag record at offset " + std::to_string(offset) +
                " has an impossibly small length (" +
                std::to_string(record_len) + " bytes)");
  if (offset + LEN_FIELD + record_len > file_size)
    return fail("Bag record at offset " + std::to_string(offset) +
                " is truncated (declares " + std::to_string(record_len) +
                " bytes, only " +
                std::to_string(file_size - offset - LEN_FIELD) + " remain)");

  std::string body(static_cast<size_t>(record_len), '\0');
  if (!try_read(_in, body.data(), static_cast<std::streamsize>(record_len)))
    return fail("Failed to read record body at offset " + std::to_string(offset));

  size_t body_data_len =
      static_cast<size_t>(record_len) - (_crc_enabled ? 4 : 0);
  const auto *p = reinterpret_cast<const unsigned char *>(body.data());

  if (_crc_enabled) {
    uint32_t stored_crc = get_u32(p + body_data_len);
    uint32_t actual_crc = crc32_of(p, body_data_len);
    if (stored_crc != actual_crc)
      return fail("CRC mismatch for bag record at offset " +
                  std::to_string(offset) + " (stored " +
                  std::to_string(stored_crc) + ", computed " +
                  std::to_string(actual_crc) + ")");
  }

  size_t pos = 0;
  if (pos + 8 > body_data_len)
    return fail("Bag record at offset " + std::to_string(offset) +
                " is corrupt (short timestamp field)");
  int64_t timestamp = get_i64(p + pos);
  pos += 8;

  if (pos + 4 > body_data_len)
    return fail("Bag record at offset " + std::to_string(offset) +
                " is corrupt (short topic length field)");
  uint32_t topic_len = get_u32(p + pos);
  pos += 4;
  if (pos + topic_len > body_data_len)
    return fail("Bag record at offset " + std::to_string(offset) +
                " is corrupt (topic length exceeds record body)");
  std::string topic(reinterpret_cast<const char *>(p + pos), topic_len);
  pos += topic_len;

  if (pos + 4 > body_data_len)
    return fail("Bag record at offset " + std::to_string(offset) +
                " is corrupt (short part-count field)");
  uint32_t n_parts = get_u32(p + pos);
  pos += 4;

  std::vector<std::string> parts;
  for (uint32_t i = 0; i < n_parts; ++i) {
    if (pos + 4 > body_data_len)
      return fail("Bag record at offset " + std::to_string(offset) +
                  " is corrupt (short part-length field for part " +
                  std::to_string(i) + ")");
    uint32_t part_len = get_u32(p + pos);
    pos += 4;
    if (pos + part_len > body_data_len)
      return fail("Bag record at offset " + std::to_string(offset) +
                  " is corrupt (part " + std::to_string(i) +
                  " length exceeds record body)");
    parts.emplace_back(reinterpret_cast<const char *>(p + pos), part_len);
    pos += part_len;
  }

  if (pos != body_data_len)
    return fail("Bag record at offset " + std::to_string(offset) +
                " has trailing bytes after its declared parts");

  BagRecord rec;
  rec.timestamp_ns = timestamp;
  rec.topic = std::move(topic);
  rec.parts = std::move(parts);
  return rec;
}

BagRecord BagReader::read(size_t index) const {
  if (index >= _offsets.size())
    throw BagError("Bag record index " + std::to_string(index) +
                    " out of range (" + std::to_string(_offsets.size()) +
                    " records)");
  _in.clear();
  _in.seekg(0, std::ios::end);
  uint64_t file_size = static_cast<uint64_t>(_in.tellg());
  auto rec = read_record_at(_offsets[index], file_size, /*strict=*/true);
  return std::move(*rec); // strict=true: never nullopt without throwing
}

std::optional<BagRecord> BagReader::next() {
  if (_cursor >= _offsets.size())
    return std::nullopt;
  _in.clear();
  _in.seekg(0, std::ios::end);
  uint64_t file_size = static_cast<uint64_t>(_in.tellg());
  auto rec = read_record_at(_offsets[_cursor], file_size, /*strict=*/true);
  ++_cursor;
  return rec;
}

} // namespace Mads
