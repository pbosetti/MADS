/*
Internal helper: the lossy single-producer ring of fixed-size slots behind the
shared-memory data plane (public types in src/mads_shm.hpp). Not part of the
installed SDK.

LAYOUT of the segment (all offsets from its start):

  0                 RingHeader (64 B): magic "MADSSHM1", layout_version,
                    slot_count, slot_size, slot_stride, data_offset,
                    owner_pid, nonce[16]
  64                slot table: slot_count x SlotHeader (64 B each, so two
                    slots' counters never share a cache line):
                    { std::atomic<uint64_t> seq; uint64_t size; }
  data_offset       slot_count x slot_stride bytes of payload; data_offset
                    and slot_stride are multiples of 4096, so every slot
                    starts on a page boundary.

SEQLOCK. A slot's seq is even when stable and odd while the producer writes.
acquire() makes it odd (relaxed store + release fence), commit() makes it the
next even value (release store) and returns a descriptor carrying that value.
A consumer's view is valid while seq still equals the descriptor's value; the
producer never waits for readers, so a slow reader sees its view invalidated
when the ring wraps around to its slot.

ERRORS. SharedRing::create() throws (std::invalid_argument on bad geometry,
std::system_error from the OS); commit() throws std::length_error on an
oversize payload and std::logic_error on a slot that is not the one
currently being written. SegmentCache::view() never throws: it returns
std::nullopt and fills *error.

THREAD SAFETY. SharedRing: one producer thread (acquire/commit are not
synchronized). SegmentCache: view() and the other members are serialized by
an internal mutex, so one cache may be shared by several consumer threads;
the returned SharedView is then read without locking.
*/
#pragma once

#include "mads_shm.hpp"
#include "shm/shared_segment.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace Mads::shm {

static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "shared-memory seqlock needs lock-free 64-bit atomics");

constexpr char RING_MAGIC[8] = {'M', 'A', 'D', 'S', 'S', 'H', 'M', '1'};
constexpr uint32_t RING_LAYOUT_VERSION = 1;
constexpr size_t RING_PAGE = 4096;

struct alignas(64) RingHeader {
  char magic[8];
  uint32_t layout_version;
  uint32_t slot_count;
  uint64_t slot_size;   ///< usable bytes per slot
  uint64_t slot_stride; ///< slot_size rounded up to RING_PAGE
  uint64_t data_offset; ///< start of slot 0's payload
  uint64_t owner_pid;
  unsigned char nonce[16];
};
static_assert(sizeof(RingHeader) == 64);

struct alignas(64) SlotHeader {
  std::atomic<uint64_t> seq;
  uint64_t size;
};
static_assert(sizeof(SlotHeader) == 64);

/// Byte offsets/sizes of a ring with the given geometry, or nullopt if they
/// overflow or the geometry is invalid (slot_count < 2, slot_size == 0).
struct RingLayout {
  uint64_t slot_stride;
  uint64_t data_offset;
  uint64_t total_size;
};
std::optional<RingLayout> ring_layout(uint32_t slot_count, uint64_t slot_size);

/// Producer side: owns the segment and unlinks it on destruction.
class SharedRing {
public:
  /// Creates a ring of `slot_count` (>= 2) slots of `slot_size` (> 0) bytes
  /// in a fresh segment with a random nonce.
  static std::unique_ptr<SharedRing> create(uint32_t slot_count,
                                            size_t slot_size);

  SharedRing(SharedRing const &) = delete;
  SharedRing &operator=(SharedRing const &) = delete;
  ~SharedRing();

  /// Next slot in round-robin order, marked as being written (odd seq).
  /// Views of its previous contents become invalid immediately.
  WritableSlot acquire();

  /// Publishes `size` bytes written into `slot` and returns the descriptor to
  /// send. Throws std::length_error if size > slot.capacity, and
  /// std::logic_error if `slot` is not currently being written (already
  /// committed, or re-acquired after a full wrap).
  ShmDescriptor commit(WritableSlot &slot, size_t size);

  uint32_t slot_count() const { return _slot_count; }
  size_t slot_size() const { return _slot_size; }
  std::string const &segment_name() const { return _segment->name(); }
  std::string const &nonce_hex() const { return _nonce_hex; }

private:
  SharedRing(std::unique_ptr<SharedSegment> segment, uint32_t slot_count,
             size_t slot_size, RingLayout layout, std::string nonce_hex);
  SlotHeader &slot_header(uint32_t index) const;
  unsigned char *slot_data(uint32_t index) const;

  std::unique_ptr<SharedSegment> _segment;
  uint32_t _slot_count = 0;
  size_t _slot_size = 0;
  RingLayout _layout{};
  std::string _nonce_hex;
  uint32_t _next = 0;
};

/// Consumer side: an LRU of mapped segments, turning descriptors into views.
///
/// A SharedView borrows the cache's mapping: it dangles once its segment is
/// evicted, which happens when view() maps more than `capacity` distinct
/// segments, when a segment name turns out to have been reused (nonce
/// mismatch), or on clear() / destruction. Do not keep views across those.
class SegmentCache {
public:
  explicit SegmentCache(size_t capacity = 16);

  std::optional<Mads::SharedView> view(ShmDescriptor const &d,
                                       nlohmann::json meta,
                                       std::string *error = nullptr);

  size_t size() const;
  size_t capacity() const { return _capacity; }
  void clear();

private:
  struct Entry {
    std::unique_ptr<SharedSegment> segment;
    std::string nonce_hex;
    uint32_t slot_count = 0;
    uint64_t slot_size = 0;
    RingLayout layout{};
  };
  using List = std::list<Entry>;

  // Maps and validates segment `name`; nullopt (and *error) on failure.
  static std::optional<Entry> open_entry(std::string const &name,
                                         std::string *error);
  List::iterator lookup(std::string const &name, std::string *error);
  void evict(List::iterator it);

  size_t _capacity;
  mutable std::mutex _mutex;
  List _lru; ///< most recently used first
  std::unordered_map<std::string, List::iterator> _index;
};

} // namespace Mads::shm
