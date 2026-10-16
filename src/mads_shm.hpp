/*
  ____  _                        _
 / ___|| |__   __ _ _ __ ___  __| |  _ __ ___   ___ _ __ ___   ___  _ __ _   _
 \___ \| '_ \ / _` | '__/ _ \/ _` | | '_ ` _ \ / _ \ '_ ` _ \ / _ \| '__| | | |
  ___) | | | | (_| | | |  __/ (_| | | | | | | |  __/ | | | | | (_) | |  | |_| |
 |____/|_| |_|\__,_|_|  \___|\__,_| |_| |_| |_|\___|_| |_| |_|\___/|_|   \__, |
                                                                         |___/
Public types of the shared-memory data plane.

A producer owns a ring of N fixed-size slots in one shared segment. It writes
a payload into a slot and publishes only a small descriptor over ZeroMQ; a
consumer on the same host maps the segment and reads the bytes in place.

The ring is lossy and read-only for consumers: the producer never waits, and
overwrites the oldest slot when it wraps. Every slot carries a sequence
counter (a seqlock): a consumer's view is valid only while the slot's
counter still equals the one in the descriptor, so a consumer that is too
slow detects the overwrite instead of silently reading torn data.
*/
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

namespace Mads {

/**
 * @brief Reference to a payload in a shared-memory ring, as carried on the
 * wire (under the "shm" key of a message's meta).
 */
struct ShmDescriptor {
  std::string segment; ///< OS name of the segment (<= 31 chars)
  uint32_t slot = 0;   ///< slot index in the ring
  uint64_t seq = 0;    ///< even seqlock value the slot had when published
  uint64_t size = 0;   ///< payload bytes in the slot
  std::string nonce;   ///< 32 hex chars, must match the segment header
  std::string host;    ///< producer's host token
};

inline void to_json(nlohmann::json &j, ShmDescriptor const &d) {
  j = nlohmann::json{{"seg", d.segment}, {"slot", d.slot}, {"seq", d.seq},
                     {"size", d.size},   {"nonce", d.nonce}, {"host", d.host}};
}

inline void from_json(nlohmann::json const &j, ShmDescriptor &d) {
  j.at("seg").get_to(d.segment);
  j.at("slot").get_to(d.slot);
  j.at("seq").get_to(d.seq);
  j.at("size").get_to(d.size);
  j.at("nonce").get_to(d.nonce);
  j.at("host").get_to(d.host);
}

/**
 * @brief A slot handed to the producer for writing.
 *
 * Obtained from Agent::acquire_slot(); write at most `capacity` bytes at
 * `data`, then hand it back with Agent::publish_slot(). Between the two calls
 * the slot is marked as being written (odd sequence), so consumers that still
 * hold a view of its previous contents see their view invalidated.
 */
struct WritableSlot {
  unsigned char *data = nullptr;
  size_t capacity = 0;
  uint32_t index = 0;     ///< slot index in the ring
  uint64_t write_seq = 0; ///< odd seqlock value set by acquire()
  uint64_t prev_seq = 0;  ///< seqlock value before acquire()

  explicit operator bool() const { return data != nullptr; }
};

/**
 * @brief Read-only, zero-copy view of a shared-memory payload.
 *
 * Obtained from Agent::last_shared() after receive() returned
 * message_type::shared. The view points straight into the producer's ring:
 * it is only meaningful until the producer wraps around to this slot again.
 * Check valid() *after* using the data; a false result means the bytes may
 * have been overwritten while you read them. Copy the data if you need to
 * keep it.
 *
 * A view (and every copy of it) pins the mapping it points into, so data()
 * never dangles, even after the consumer's segment cache dropped the
 * segment or the producer exited: at worst valid() turns false.
 */
class SharedView {
public:
  SharedView() = default;
  /// `pin` keeps the memory under `data` and `seq` mapped for as long as the
  /// view (or a copy of it) exists.
  SharedView(const unsigned char *data, size_t size, nlohmann::json meta,
             const std::atomic<uint64_t> *seq, uint64_t expected,
             std::shared_ptr<const void> pin = nullptr)
      : _data(data), _size(size), _meta(std::move(meta)), _seq(seq),
        _expected(expected), _pin(std::move(pin)) {}

  const unsigned char *data() const { return _data; }
  size_t size() const { return _size; }
  nlohmann::json const &meta() const { return _meta; }

  /// True while the slot still holds the published payload.
  bool valid() const {
    // Seqlock read side: the fence keeps the payload reads that precede this
    // call from being reordered after the counter load.
    std::atomic_thread_fence(std::memory_order_acquire);
    return _seq && _seq->load(std::memory_order_relaxed) == _expected;
  }
  explicit operator bool() const { return _data != nullptr; }

private:
  const unsigned char *_data = nullptr;
  size_t _size = 0;
  nlohmann::json _meta;
  const std::atomic<uint64_t> *_seq = nullptr;
  uint64_t _expected = 0;
  std::shared_ptr<const void> _pin;
};

} // namespace Mads
