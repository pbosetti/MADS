// Unit tests for Mads::BagWriter / Mads::BagReader (src/bag.hpp/.cpp).
// Pure file I/O: no sockets, no Agent, no ZMQ.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "bag.hpp"

namespace fs = std::filesystem;
using namespace Mads;

namespace {

int current_pid() {
#if defined(_WIN32)
  return _getpid();
#else
  return ::getpid();
#endif
}

// Unique per-process, per-call temp path so parallel test runs (this suite
// alone, or several worktrees sharing the OS temp dir) never collide.
fs::path unique_temp_path(const std::string &tag) {
  static std::atomic<int> counter{0};
  return fs::temp_directory_path() /
         ("mads_test_bag_" + tag + "_" + std::to_string(current_pid()) + "_" +
          std::to_string(counter++) + ".bag");
}

// RAII: removes the temp file on scope exit.
struct ScopedPath {
  fs::path p;
  explicit ScopedPath(fs::path p_) : p(std::move(p_)) {}
  ~ScopedPath() {
    std::error_code ec;
    fs::remove(p, ec);
  }
};

struct SampleRecord {
  int64_t ts;
  std::string topic;
  std::vector<std::string> parts;
};

std::vector<SampleRecord> make_sample_records() {
  std::vector<SampleRecord> recs;
  recs.push_back({1000, "sensors/acc/x", {std::string("\x01\x02\x03", 3)}});
  recs.push_back(
      {2000, "sensors/acc/y", {std::string("header"), std::string("payload")}});
  // A part with embedded NUL bytes must round-trip byte-identical (length-
  // prefixed framing, not NUL-terminated).
  std::string with_nul;
  with_nul.push_back('a');
  with_nul.push_back('\0');
  with_nul.push_back('b');
  recs.push_back({3000, "control", {with_nul}});
  // A ~1 MB blob part, filled with a repeating-but-not-trivial byte pattern
  // so any accidental truncation/corruption would change the content.
  std::string blob(1024 * 1024 + 37, '\0');
  for (size_t i = 0; i < blob.size(); ++i)
    blob[i] = static_cast<char>((i * 31 + 7) & 0xFF);
  recs.push_back({4000, "camera/frame", {std::string("{\"format\":\"raw\"}"), blob}});
  recs.push_back({5000, "sensors/gyro/x", {}}); // zero-part record must round-trip too
  return recs;
}

void write_sample_bag(const fs::path &path, const std::vector<SampleRecord> &recs,
                      bool enable_crc = true) {
  BagWriter w(path, enable_crc);
  for (auto const &r : recs)
    w.write(r.ts, r.topic, r.parts);
  w.close();
}

} // namespace

// ---------------------------------------------------------------------------
// Round-trip: byte-identical parts/ordering, including a ~1 MB blob part.
// ---------------------------------------------------------------------------

TEST_CASE("BagWriter/BagReader round-trip N records byte-identical, "
          "including a ~1MB blob part",
          "[bag]") {
  auto path = unique_temp_path("roundtrip");
  ScopedPath cleanup(path);
  auto recs = make_sample_records();

  write_sample_bag(path, recs);

  BagReader r(path);
  REQUIRE(r.record_count() == recs.size());
  REQUIRE(r.has_index());
  REQUIRE_FALSE(r.truncated());
  REQUIRE(r.first_timestamp().has_value());
  REQUIRE(r.last_timestamp().has_value());
  CHECK(*r.first_timestamp() == recs.front().ts);
  CHECK(*r.last_timestamp() == recs.back().ts);

  for (size_t i = 0; i < recs.size(); ++i) {
    BagRecord got = r.read(i);
    INFO("record " << i);
    CHECK(got.timestamp_ns == recs[i].ts);
    CHECK(got.topic == recs[i].topic);
    REQUIRE(got.parts.size() == recs[i].parts.size());
    for (size_t j = 0; j < recs[i].parts.size(); ++j)
      CHECK(got.parts[j] == recs[i].parts[j]);
  }

  // Sequential iteration (next()/rewind()) agrees with random access.
  r.rewind();
  size_t count = 0;
  while (auto rec = r.next()) {
    REQUIRE(count < recs.size());
    CHECK(rec->timestamp_ns == recs[count].ts);
    CHECK(rec->topic == recs[count].topic);
    CHECK(rec->parts == recs[count].parts);
    ++count;
  }
  CHECK(count == recs.size());
}

TEST_CASE("BagWriter/BagReader round-trip without CRC still works",
          "[bag]") {
  auto path = unique_temp_path("nocrc");
  ScopedPath cleanup(path);
  auto recs = make_sample_records();
  write_sample_bag(path, recs, /*enable_crc=*/false);

  BagReader r(path);
  REQUIRE_FALSE(r.crc_enabled());
  REQUIRE(r.record_count() == recs.size());
  for (size_t i = 0; i < recs.size(); ++i) {
    auto got = r.read(i);
    CHECK(got.parts == recs[i].parts);
  }
}

// ---------------------------------------------------------------------------
// Index vs. linear scan agreement
// ---------------------------------------------------------------------------

TEST_CASE("BagReader: missing-footer linear-scan fallback matches the "
          "indexed path (count, first/last timestamp, record content)",
          "[bag]") {
  auto path = unique_temp_path("footer_strip");
  ScopedPath cleanup(path);
  auto recs = make_sample_records();
  write_sample_bag(path, recs);

  // Baseline: the indexed reader.
  BagReader indexed(path);
  REQUIRE(indexed.has_index());
  REQUIRE_FALSE(indexed.truncated());

  // Strip the footer+trailer entirely by truncating a copy of the file
  // right after the last record -- simulating a process killed before
  // BagWriter::close() ever ran (close() is what appends footer+trailer).
  // BagWriter's own destructor always calls close(), so the only way to
  // produce a genuinely footer-less file here is to chop the tail off a
  // complete one after the fact, using the on-disk footer/trailer layout
  // documented in src/bag.hpp: footer = magic(4) + count(8) + first_ts(8) +
  // last_ts(8) + count * offset(8); trailer = magic(4) + footer_offset(8) +
  // format_ver(4) = 16 bytes (fixed).
  auto stripped = unique_temp_path("footer_strip_raw");
  ScopedPath cleanup2(stripped);
  fs::copy_file(path, stripped, fs::copy_options::overwrite_existing);
  uintmax_t full_size = fs::file_size(stripped);
  uintmax_t footer_size = 28 + 8 * static_cast<uintmax_t>(recs.size());
  uintmax_t trailer_size = 16;
  REQUIRE(full_size > footer_size + trailer_size);
  fs::resize_file(stripped, full_size - footer_size - trailer_size);

  BagReader fallback(stripped);
  REQUIRE_FALSE(fallback.has_index());
  REQUIRE_FALSE(fallback.truncated()); // complete data, just no footer
  CHECK(fallback.record_count() == indexed.record_count());
  CHECK(fallback.first_timestamp() == indexed.first_timestamp());
  CHECK(fallback.last_timestamp() == indexed.last_timestamp());

  for (size_t i = 0; i < recs.size(); ++i) {
    auto a = indexed.read(i);
    auto b = fallback.read(i);
    CHECK(a.timestamp_ns == b.timestamp_ns);
    CHECK(a.topic == b.topic);
    CHECK(a.parts == b.parts);
  }
}

// ---------------------------------------------------------------------------
// Truncated-tail recovery
// ---------------------------------------------------------------------------

TEST_CASE("BagReader: a bag truncated mid-record recovers the complete "
          "valid prefix and reports truncated()",
          "[bag]") {
  auto path = unique_temp_path("truncated");
  ScopedPath cleanup(path);
  auto recs = make_sample_records();
  write_sample_bag(path, recs);

  uintmax_t full_size = fs::file_size(path);
  // Chop off the last 200 bytes: guaranteed to land inside the footer index
  // (each entry is 8 bytes, so a clean multiple would still corrupt the
  // trailer/footer-size relationship) or, for a small bag, into the tail
  // record itself either way this must not be a byte offset that happens
  // to fall exactly on a record boundary that also has a coincidentally
  // still-valid trailer -- 200 bytes is comfortably larger than the
  // trailer (16 bytes) so the trailer is always destroyed.
  REQUIRE(full_size > 200);
  uintmax_t new_size = full_size - 200;
  fs::resize_file(path, new_size);

  BagReader r(path);
  REQUIRE_FALSE(r.has_index()); // trailer destroyed -> fallback path
  REQUIRE(r.truncated());
  // Every record fully before the cut must still be recoverable...
  REQUIRE(r.record_count() >= 1);
  REQUIRE(r.record_count() <= recs.size());
  for (size_t i = 0; i < r.record_count(); ++i) {
    auto got = r.read(i);
    CHECK(got.timestamp_ns == recs[i].ts);
    CHECK(got.topic == recs[i].topic);
    CHECK(got.parts == recs[i].parts);
  }
  // ...and the very next one (if any existed) must NOT have been silently
  // fabricated from partial bytes.
  if (r.record_count() < recs.size()) {
    CHECK(*r.last_timestamp() == recs[r.record_count() - 1].ts);
  }
}

TEST_CASE("BagReader: truncation right at the header (zero recoverable "
          "records) is reported cleanly, not as UB",
          "[bag]") {
  auto path = unique_temp_path("truncated_empty");
  ScopedPath cleanup(path);
  auto recs = make_sample_records();
  write_sample_bag(path, recs);

  // Cut down to just the 12-byte header plus a couple of stray bytes: not
  // even one full record survives.
  fs::resize_file(path, 14);

  BagReader r(path);
  REQUIRE_FALSE(r.has_index());
  REQUIRE(r.truncated());
  CHECK(r.record_count() == 0);
  CHECK_FALSE(r.first_timestamp().has_value());
  CHECK_FALSE(r.last_timestamp().has_value());
}

// ---------------------------------------------------------------------------
// CRC mismatch detection
// ---------------------------------------------------------------------------

TEST_CASE("BagReader: a corrupted record byte is detected via CRC mismatch",
          "[bag]") {
  auto path = unique_temp_path("crc_corrupt");
  ScopedPath cleanup(path);
  auto recs = make_sample_records();
  write_sample_bag(path, recs, /*enable_crc=*/true);

  // Flip one byte inside the first record's topic text ("sensors/acc/x"):
  // header(12) + record_len(4) + timestamp(8) + topic_len(4) = offset 28 is
  // the topic's first byte. The footer/index itself is untouched, so
  // has_index() and record_count() must still reflect the original file;
  // only decoding this specific record must fail.
  {
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(f);
    const std::streamoff target_offset = 28;
    char c;
    f.seekg(target_offset, std::ios::beg);
    f.read(&c, 1);
    c = static_cast<char>(~c);
    f.seekp(target_offset, std::ios::beg);
    f.write(&c, 1);
  }

  BagReader r(path);
  REQUIRE(r.has_index()); // footer untouched
  REQUIRE(r.record_count() == recs.size());
  REQUIRE_THROWS_AS(r.read(0), BagError);
  // Records that were not touched must remain readable.
  REQUIRE_NOTHROW(r.read(1));
}

// ---------------------------------------------------------------------------
// Bad magic / future format_ver -> clean, reported error, never UB.
// ---------------------------------------------------------------------------

TEST_CASE("BagReader: bad magic bytes are rejected with a clean BagError",
          "[bag]") {
  auto path = unique_temp_path("bad_magic");
  ScopedPath cleanup(path);
  {
    std::ofstream f(path, std::ios::binary);
    f.write("XXXX\x01\x00\x00\x00\x00\x00\x00\x00", 12);
  }
  REQUIRE_THROWS_AS(BagReader(path), BagError);
}

TEST_CASE("BagReader: a format_ver newer than supported is rejected with a "
          "clean BagError",
          "[bag]") {
  auto path = unique_temp_path("future_ver");
  ScopedPath cleanup(path);
  {
    std::ofstream f(path, std::ios::binary);
    // "MBAG" + format_ver = 0xFFFFFFFF (LE) + flags = 0
    const unsigned char bytes[12] = {'M', 'B', 'A', 'G', 0xFF, 0xFF,
                                     0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
    f.write(reinterpret_cast<const char *>(bytes), sizeof(bytes));
  }
  REQUIRE_THROWS_AS(BagReader(path), BagError);
}

TEST_CASE("BagReader: an empty/too-small file is rejected with a clean "
          "BagError, not UB",
          "[bag]") {
  auto path = unique_temp_path("too_small");
  ScopedPath cleanup(path);
  {
    std::ofstream f(path, std::ios::binary);
    f.write("MBAG", 4); // header truncated before format_ver/flags
  }
  REQUIRE_THROWS_AS(BagReader(path), BagError);
}

TEST_CASE("BagReader: opening a nonexistent file is a clean BagError",
          "[bag]") {
  auto path = unique_temp_path("nonexistent");
  // Deliberately never created.
  REQUIRE_THROWS_AS(BagReader(path), BagError);
}

// ---------------------------------------------------------------------------
// Misc accessors / edge cases
// ---------------------------------------------------------------------------

TEST_CASE("BagWriter/BagReader: an empty bag (zero records) round-trips "
          "cleanly",
          "[bag]") {
  auto path = unique_temp_path("empty");
  ScopedPath cleanup(path);
  { BagWriter w(path); w.close(); }

  BagReader r(path);
  REQUIRE(r.has_index());
  CHECK(r.record_count() == 0);
  CHECK_FALSE(r.first_timestamp().has_value());
  CHECK_FALSE(r.last_timestamp().has_value());
  CHECK_FALSE(r.next().has_value());
}

TEST_CASE("BagReader::read() throws BagError for an out-of-range index",
          "[bag]") {
  auto path = unique_temp_path("out_of_range");
  ScopedPath cleanup(path);
  write_sample_bag(path, make_sample_records());

  BagReader r(path);
  REQUIRE_THROWS_AS(r.read(r.record_count()), BagError);
  REQUIRE_THROWS_AS(r.read(r.record_count() + 100), BagError);
}

TEST_CASE("BagWriter::write() after close() throws BagError", "[bag]") {
  auto path = unique_temp_path("write_after_close");
  ScopedPath cleanup(path);
  BagWriter w(path);
  w.write(1, "a", {"x"});
  w.close();
  REQUIRE_THROWS_AS(w.write(2, "b", {"y"}), BagError);
}
