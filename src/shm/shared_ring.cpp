// Mads::shm::SharedRing and SegmentCache (see shared_ring.hpp).
#include "shm/shared_ring.hpp"
#include "detail/host_token.hpp"

#include <cstring>
#include <limits>
#include <new>
#include <random>
#include <stdexcept>
#include <system_error>

namespace Mads::shm {

namespace {

constexpr uint64_t U64_MAX = std::numeric_limits<uint64_t>::max();

std::string to_hex(unsigned char const *bytes, size_t n) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string s;
  s.reserve(2 * n);
  for (size_t i = 0; i < n; ++i) {
    s.push_back(digits[bytes[i] >> 4]);
    s.push_back(digits[bytes[i] & 0x0F]);
  }
  return s;
}

std::string to_lower(std::string s) {
  for (auto &c : s)
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  return s;
}

std::nullopt_t fail(std::string *error, std::string msg) {
  if (error)
    *error = std::move(msg);
  return std::nullopt;
}

} // namespace

std::optional<RingLayout> ring_layout(uint32_t slot_count,
                                      uint64_t slot_size) {
  if (slot_count < 2 || slot_size == 0 || slot_size > U64_MAX - (RING_PAGE - 1))
    return std::nullopt;
  RingLayout l{};
  l.slot_stride = (slot_size + RING_PAGE - 1) / RING_PAGE * RING_PAGE;
  uint64_t table_end =
      sizeof(RingHeader) + uint64_t(slot_count) * sizeof(SlotHeader);
  l.data_offset = (table_end + RING_PAGE - 1) / RING_PAGE * RING_PAGE;
  if (l.slot_stride > (U64_MAX - l.data_offset) / slot_count)
    return std::nullopt;
  l.total_size = l.data_offset + l.slot_stride * slot_count;
  if (l.total_size > std::numeric_limits<size_t>::max())
    return std::nullopt;
  return l;
}

/*
 * SharedRing
 */

SharedRing::SharedRing(std::unique_ptr<SharedSegment> segment,
                       uint32_t slot_count, size_t slot_size,
                       RingLayout layout, std::string nonce_hex)
    : _segment(std::move(segment)), _slot_count(slot_count),
      _slot_size(slot_size), _layout(layout), _nonce_hex(std::move(nonce_hex)) {
}

std::unique_ptr<SharedRing> SharedRing::create(uint32_t slot_count,
                                               size_t slot_size) {
  if (slot_count < 2)
    throw std::invalid_argument("SharedRing needs at least 2 slots");
  if (slot_size == 0)
    throw std::invalid_argument("SharedRing slot size must be > 0");
  auto layout = ring_layout(slot_count, slot_size);
  if (!layout)
    throw std::invalid_argument("SharedRing geometry overflows (" +
                                std::to_string(slot_count) + " x " +
                                std::to_string(slot_size) + " bytes)");

  unsigned char nonce[16];
  {
    std::random_device rd;
    for (size_t i = 0; i < sizeof(nonce); i += 4) {
      uint32_t r = rd();
      std::memcpy(nonce + i, &r, 4);
    }
  }

  // Names are unique within this process; a collision means a dead process
  // with the same pid left its segment behind. Skip to the next name.
  std::unique_ptr<SharedSegment> seg;
  for (int attempt = 0;; ++attempt) {
    try {
      seg = SharedSegment::create(make_segment_name(),
                                  static_cast<size_t>(layout->total_size));
      break;
    } catch (std::system_error const &e) {
      if (attempt >= 15 ||
          e.code() != std::make_error_code(std::errc::file_exists))
        throw;
    }
  }

  auto *base = static_cast<unsigned char *>(seg->data());
  auto *hdr = new (base) RingHeader{};
  hdr->layout_version = RING_LAYOUT_VERSION;
  hdr->slot_count = slot_count;
  hdr->slot_size = slot_size;
  hdr->slot_stride = layout->slot_stride;
  hdr->data_offset = layout->data_offset;
  hdr->owner_pid = current_pid();
  std::memcpy(hdr->nonce, nonce, sizeof(nonce));
  auto *slots = reinterpret_cast<SlotHeader *>(base + sizeof(RingHeader));
  for (uint32_t i = 0; i < slot_count; ++i)
    new (&slots[i]) SlotHeader{};
  // Magic last: a header with a valid magic is a fully initialized header.
  std::atomic_thread_fence(std::memory_order_release);
  std::memcpy(hdr->magic, RING_MAGIC, sizeof(RING_MAGIC));

  return std::unique_ptr<SharedRing>(new SharedRing(
      std::move(seg), slot_count, slot_size, *layout,
      to_hex(nonce, sizeof(nonce))));
}

SharedRing::~SharedRing() {
  if (_segment)
    _segment->unlink();
}

SlotHeader &SharedRing::slot_header(uint32_t index) const {
  auto *base = static_cast<unsigned char *>(_segment->data());
  return reinterpret_cast<SlotHeader *>(base + sizeof(RingHeader))[index];
}

unsigned char *SharedRing::slot_data(uint32_t index) const {
  auto *base = static_cast<unsigned char *>(_segment->data());
  return base + _layout.data_offset + uint64_t(index) * _layout.slot_stride;
}

WritableSlot SharedRing::acquire() {
  uint32_t idx = _next;
  _next = (_next + 1) % _slot_count;
  auto &h = slot_header(idx);
  uint64_t s = h.seq.load(std::memory_order_relaxed);
  // Odd only if a previous acquire() of this slot was never committed: move
  // to a new odd value anyway, so that stale WritableSlots are detectable.
  uint64_t w = (s & 1) ? s + 2 : s + 1;
  h.seq.store(w, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  WritableSlot slot;
  slot.data = slot_data(idx);
  slot.capacity = _slot_size;
  slot.index = idx;
  slot.write_seq = w;
  return slot;
}

ShmDescriptor SharedRing::commit(WritableSlot &slot, size_t size) {
  if (slot.index >= _slot_count || slot.data != slot_data(slot.index))
    throw std::logic_error("WritableSlot does not belong to this ring");
  if (size > slot.capacity)
    throw std::length_error("Shared-memory payload of " + std::to_string(size) +
                            " bytes exceeds slot capacity of " +
                            std::to_string(slot.capacity));
  auto &h = slot_header(slot.index);
  if ((slot.write_seq & 1) == 0 ||
      h.seq.load(std::memory_order_relaxed) != slot.write_seq)
    throw std::logic_error("WritableSlot is not being written (already "
                           "committed, or re-acquired after a wrap)");
  h.size = size;
  uint64_t seq = slot.write_seq + 1;
  h.seq.store(seq, std::memory_order_release);

  ShmDescriptor d;
  d.segment = _segment->name();
  d.slot = slot.index;
  d.seq = seq;
  d.size = size;
  d.nonce = _nonce_hex;
  d.host = detail::host_token();
  slot.data = nullptr; // consumed
  return d;
}

/*
 * SegmentCache
 */

SegmentCache::SegmentCache(size_t capacity)
    : _capacity(capacity == 0 ? 1 : capacity) {}

size_t SegmentCache::size() const {
  std::lock_guard lock(_mutex);
  return _lru.size();
}

void SegmentCache::clear() {
  std::lock_guard lock(_mutex);
  _index.clear();
  _lru.clear();
}

std::optional<SegmentCache::Entry>
SegmentCache::open_entry(std::string const &name, std::string *error) {
  Entry e;
  try {
    e.segment = SharedSegment::open(name);
  } catch (std::exception const &ex) {
    return fail(error, "Cannot open shared-memory segment " + name +
                           " (producer gone?): " + ex.what());
  }
  if (e.segment->size() < sizeof(RingHeader))
    return fail(error, "Shared-memory segment " + name + " is too small");
  auto const *base = static_cast<unsigned char const *>(e.segment->data());
  RingHeader hdr;
  std::memcpy(&hdr, base, sizeof(hdr));
  std::atomic_thread_fence(std::memory_order_acquire);
  if (std::memcmp(hdr.magic, RING_MAGIC, sizeof(RING_MAGIC)) != 0)
    return fail(error, "Shared-memory segment " + name +
                           " is not a MADS ring (bad magic)");
  if (hdr.layout_version != RING_LAYOUT_VERSION)
    return fail(error, "Shared-memory segment " + name + " has layout version " +
                           std::to_string(hdr.layout_version) + ", expected " +
                           std::to_string(RING_LAYOUT_VERSION));
  auto layout = ring_layout(hdr.slot_count, hdr.slot_size);
  if (!layout || layout->slot_stride != hdr.slot_stride ||
      layout->data_offset != hdr.data_offset ||
      layout->total_size > e.segment->size())
    return fail(error,
                "Shared-memory segment " + name + " has an inconsistent header");
  e.nonce_hex = to_hex(hdr.nonce, sizeof(hdr.nonce));
  e.slot_count = hdr.slot_count;
  e.slot_size = hdr.slot_size;
  e.layout = *layout;
  return e;
}

void SegmentCache::evict(List::iterator it) {
  _index.erase(it->segment->name());
  _lru.erase(it);
}

SegmentCache::List::iterator SegmentCache::lookup(std::string const &name,
                                                  std::string *error) {
  auto found = _index.find(name);
  if (found != _index.end()) {
    _lru.splice(_lru.begin(), _lru, found->second);
    return _lru.begin();
  }
  auto e = open_entry(name, error);
  if (!e)
    return _lru.end();
  _lru.push_front(std::move(*e));
  _index[name] = _lru.begin();
  while (_lru.size() > _capacity)
    evict(std::prev(_lru.end()));
  return _lru.begin();
}

std::optional<Mads::SharedView> SegmentCache::view(ShmDescriptor const &d,
                                                   nlohmann::json meta,
                                                   std::string *error) {
  if (d.host != detail::host_token())
    return fail(error, "Shared-memory reference comes from a foreign host (" +
                           d.host + "); it can only be read on the producer's "
                           "host");
  if (d.seq == 0 || (d.seq & 1))
    return fail(error, "Shared-memory descriptor has an invalid sequence " +
                           std::to_string(d.seq));
  std::string nonce = to_lower(d.nonce);

  std::lock_guard lock(_mutex);
  auto it = lookup(d.segment, error);
  if (it == _lru.end())
    return std::nullopt;
  if (it->nonce_hex != nonce) {
    // The name may have been reused by a new producer since we mapped it.
    evict(it);
    it = lookup(d.segment, error);
    if (it == _lru.end())
      return std::nullopt;
    if (it->nonce_hex != nonce)
      return fail(error, "Shared-memory segment " + d.segment +
                             " nonce mismatch: descriptor does not belong to "
                             "this segment");
  }
  if (d.slot >= it->slot_count)
    return fail(error, "Shared-memory slot " + std::to_string(d.slot) +
                           " out of range (ring has " +
                           std::to_string(it->slot_count) + " slots)");
  if (d.size > it->slot_size)
    return fail(error, "Shared-memory payload size " + std::to_string(d.size) +
                           " exceeds slot size " +
                           std::to_string(it->slot_size));

  auto *base = static_cast<unsigned char *>(it->segment->data());
  auto const *slot = reinterpret_cast<SlotHeader const *>(
      base + sizeof(RingHeader) + uint64_t(d.slot) * sizeof(SlotHeader));
  // Seqlock read side, first half: acquire pairs with commit()'s release.
  if (slot->seq.load(std::memory_order_acquire) != d.seq)
    return fail(error, "Shared-memory slot " + std::to_string(d.slot) +
                           " of " + d.segment +
                           " was overwritten by the producer");
  auto const *data =
      base + it->layout.data_offset + uint64_t(d.slot) * it->layout.slot_stride;
  return Mads::SharedView(data, static_cast<size_t>(d.size), std::move(meta),
                          &slot->seq, d.seq);
}

} // namespace Mads::shm
