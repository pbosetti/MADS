/*
  ____
 | __ )  __ _  __ _
 |  _ \ / _` |/ _` |
 | |_) | (_| | (_| |
 |____/ \__,_|\__, |
              |___/

Bespoke binary bag file format for MADS message recording/replay (P3).

Stores the exact multi-part ZMQ wire frame MADS agents already exchange
(topic + N parts, e.g. [topic][header][payload] or [topic][meta][blob
bytes]) with no interpretation of the parts -- a bag round-trips whatever
was on the wire byte-identical, whether it is JSON or a binary blob.

BagWriter/BagReader are pure file I/O: no sockets, no Agent dependency, no
ZMQ. mads-record/mads-play (src/main/record.cpp, src/main/play.cpp) are the
only callers that combine this with Agent::receive_raw_message()/
publish_raw_message().

File layout
-----------
All multi-byte integers are little-endian, written/read byte-by-byte (never
via a raw struct memcpy) so the format is identical across x86/ARM and
32/64-bit builds regardless of host endianness or struct padding.

  Header (12 bytes):
    magic[4]        = 'M','B','A','G'
    format_ver: u32  = BAG_FORMAT_VERSION
    flags: u32        bit 0: per-record CRC32 enabled

  Record (repeated):
    record_len: u32   number of bytes following this field for this record
                       (timestamp + topic + parts + optional crc32); lets a
                       linear scan detect a truncated trailing record with a
                       single bounds check instead of parsing into the void.
    timestamp: i64     nanoseconds
    topic_len: u32
    topic: bytes[topic_len]
    n_parts: u32
    parts: n_parts * { part_len: u32; bytes[part_len] }
    crc32: u32         present only if the header's CRC flag is set; CRC-32
                       (IEEE 802.3 / zlib polynomial) over every byte of this
                       record from `timestamp` through the last part, i.e.
                       everything `record_len` counts except the crc32 field
                       itself.

  Footer (optional, written by BagWriter::close()):
    footer_magic[4]  = 'M','B','F','T'
    record_count: u64
    first_timestamp: i64
    last_timestamp: i64
    index: record_count * u64   byte offset of each record's `record_len`
                                 field, for O(1) random access / seeking.

  Trailer (fixed 16 bytes, always the last bytes of a cleanly-closed file):
    trailer_magic[4] = 'M','B','T','R'
    footer_offset: u64          absolute offset where footer_magic begins
    trailer_format_ver: u32     duplicated here so a reader can sanity-check
                                 the trailer without re-reading the header

A reader first seeks to (file_size - 16) and checks for the trailer magic.
If found (and consistent), the footer is read directly: O(1) `mads bag
info` and O(1) seeking to any record via the index, with no need to scan
every record. If the trailer is missing or inconsistent (e.g. the recording
process was killed before close()), the reader falls back to a linear scan
from just after the header, recovering the complete valid prefix and
reporting truncated() -- never undefined behaviour, and never a hard
failure just because the footer never got written.

A bad magic or a format_ver newer than BAG_FORMAT_VERSION is a clean
BagError, thrown from the BagReader constructor -- never garbage reads.

Author(s): Paolo Bosetti
*/

#ifndef MADS_BAG_HPP
#define MADS_BAG_HPP

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace Mads {

/// Current on-disk format version written by BagWriter. BagReader accepts
/// any format_ver <= this value.
inline constexpr uint32_t BAG_FORMAT_VERSION = 1;

/**
 * @brief Error thrown by BagWriter/BagReader for any I/O or format problem
 * (bad magic, unsupported format_ver, unreadable/unwritable file, ...).
 *
 * Bag parsing never relies on exceptions/UB for a merely-truncated file --
 * that is handled by BagReader's linear-scan fallback and truncated(). This
 * exception is reserved for cases that make the file impossible to use at
 * all (missing file, unknown magic, a format_ver from the future).
 */
class BagError : public std::runtime_error {
public:
  explicit BagError(const std::string &msg) : std::runtime_error(msg) {}
};

/// One decoded bag record: the same shape as an Agent raw message (topic +
/// N parts), plus the recorded timestamp.
struct BagRecord {
  int64_t timestamp_ns = 0;
  std::string topic;
  std::vector<std::string> parts;
};

/**
 * @brief Sequentially appends BagRecord entries to a bag file.
 *
 * Writes the header immediately (constructor), then one record per write()
 * call, and the trailing index + footer on close() (also invoked by the
 * destructor if the caller never calls it explicitly). A file whose writing
 * process is killed before close() ever runs is still a valid, readable bag
 * -- just without the O(1) index, handled by BagReader's linear-scan
 * fallback.
 */
class BagWriter {
public:
  /**
   * @brief Creates (truncating) `path` and writes the bag header.
   *
   * @param path Destination file path.
   * @param enable_crc Whether to append a CRC32 to every record (default
   *   true, per the P3 spec's "enabled by default").
   * @throws BagError if the file cannot be created for writing.
   */
  explicit BagWriter(const std::filesystem::path &path, bool enable_crc = true);

  ~BagWriter();

  BagWriter(const BagWriter &) = delete;
  BagWriter &operator=(const BagWriter &) = delete;

  /**
   * @brief Appends one record.
   *
   * @param timestamp_ns Record timestamp, nanoseconds.
   * @param topic Topic frame.
   * @param parts Frames after the topic, in wire order; stored byte-for-byte
   *   (embedded NUL bytes included), so JSON text and binary blob parts both
   *   round-trip identically.
   * @throws BagError on write failure.
   */
  void write(int64_t timestamp_ns, const std::string &topic,
             const std::vector<std::string> &parts);

  /**
   * @brief Finalizes the file: writes the index/footer/trailer and flushes.
   *
   * Idempotent -- a second call (or the destructor, after an explicit call)
   * is a no-op. Not calling this at all (process killed, or a test
   * deliberately simulating that) leaves a valid bag with only the records
   * written so far; BagReader recovers it via linear scan.
   */
  void close();

  /// Number of records written so far.
  size_t record_count() const { return _offsets.size(); }

  bool crc_enabled() const { return _crc_enabled; }

private:
  std::ofstream _out;
  std::filesystem::path _path;
  bool _crc_enabled;
  bool _closed = false;
  std::vector<uint64_t> _offsets; // start of each record's record_len field
  std::optional<int64_t> _first_ts, _last_ts;
};

/**
 * @brief Reads a bag file written by BagWriter (or a compatible/truncated
 * one), transparently using the trailing index when present and falling
 * back to a linear scan otherwise.
 */
class BagReader {
public:
  /**
   * @brief Opens and parses `path`.
   *
   * @throws BagError if the file cannot be opened, the magic is wrong, or
   *   format_ver is newer than BAG_FORMAT_VERSION.
   */
  explicit BagReader(const std::filesystem::path &path);

  /// Number of complete, valid records found (indexed or recovered).
  size_t record_count() const { return _offsets.size(); }

  /// Timestamp of the first record, if any.
  std::optional<int64_t> first_timestamp() const { return _first_ts; }

  /// Timestamp of the last record, if any.
  std::optional<int64_t> last_timestamp() const { return _last_ts; }

  /**
   * @brief True if the file was missing a valid footer/trailer and some
   * trailing bytes could not be recovered as a complete, CRC-valid record
   * (e.g. the writer process was killed mid-record). False for a complete
   * file, whether or not it happens to have a footer.
   */
  bool truncated() const { return _truncated; }

  /// True if a valid footer/index was found (O(1) path); false if this
  /// reader recovered its record list via a linear scan.
  bool has_index() const { return _has_index; }

  /// Whether per-record CRC32 is present (mirrors the header flag).
  bool crc_enabled() const { return _crc_enabled; }

  /**
   * @brief Random access to record `index` (0-based).
   *
   * @throws BagError if index is out of range, the record is corrupt, or a
   *   CRC mismatch is detected.
   */
  BagRecord read(size_t index) const;

  /// Resets sequential iteration (next()) to the first record.
  void rewind() { _cursor = 0; }

  /// Sequential iteration; returns std::nullopt once every recovered record
  /// has been returned.
  std::optional<BagRecord> next();

private:
  // Attempts to load the trailer+footer at the end of the file. Returns
  // true (and populates _offsets/_first_ts/_last_ts) only if a complete,
  // internally-consistent footer was found.
  bool try_load_footer(uint64_t file_size);

  // Recovers as many complete records as possible starting right after the
  // header, stopping at the first incomplete/corrupt record. Sets
  // _truncated accordingly.
  void linear_scan(uint64_t file_size);

  // Reads and decodes the record whose `record_len` field starts at
  // `offset`. If `strict` is true, any problem (short file, bad internal
  // length, CRC mismatch) throws BagError -- used by read()/next() on
  // offsets already trusted (from the footer index or a completed linear
  // scan), where corruption is a real error to surface. If `strict` is
  // false, the same problems are reported by returning std::nullopt
  // instead -- used by linear_scan() while probing the (possibly corrupt
  // or truncated) tail record, so recovery of the valid prefix can stop
  // cleanly instead of throwing out of the constructor.
  std::optional<BagRecord> read_record_at(uint64_t offset, uint64_t file_size,
                                          bool strict) const;

  mutable std::ifstream _in;
  std::filesystem::path _path;
  uint32_t _format_ver = 0;
  bool _crc_enabled = false;
  bool _has_index = false;
  bool _truncated = false;
  std::vector<uint64_t> _offsets;
  std::optional<int64_t> _first_ts, _last_ts;
  size_t _cursor = 0;
};

} // namespace Mads

#endif // MADS_BAG_HPP
