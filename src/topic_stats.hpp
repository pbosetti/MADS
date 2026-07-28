/*
  _____           _         ____  _        _
 |_   _|__  _ __ (_) ___   / ___|| |_ __ _| |_ ___
   | |/ _ \| '_ \| |/ __|  \___ \| __/ _` | __/ __|
   | | (_) | |_) | | (__    ___) | || (_| | |_\__ \
   |_|\___/| .__/|_|\___|  |____/ \__\__,_|\__|___/
           |_|

Pure, sliding-window per-topic activity aggregator for `mads top`. No ZMQ, no
Agent dependency: callers feed it (topic, bytes[, sample]) tuples with an
explicit or implicit timestamp, and it reports messages/s and bytes/s
averaged over a trailing window, plus last-seen/last-sample bookkeeping that
never expires. This split keeps the counting logic directly unit-testable
with synthetic timestamps (no sockets, no real sleeps) while `top.cpp` stays
a thin CLI/subscribe/render loop.

Author(s): Paolo Bosetti
*/
#pragma once

#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace Mads {

/**
 * @brief Snapshot of one topic's aggregated activity, as computed by
 * TopicStats::snapshot() at a given instant.
 */
struct TopicStat {
  std::string topic;
  /// Messages/bytes seen within the trailing window ending at the snapshot
  /// instant (the values messages_per_second/bytes_per_second are derived
  /// from).
  size_t window_messages = 0;
  size_t window_bytes = 0;
  double messages_per_second = 0.0;
  double bytes_per_second = 0.0;
  /// Lifetime message count for this topic, never pruned by the window.
  size_t total_messages = 0;
  /// Wall-clock-independent instant of the most recent record() call.
  std::chrono::steady_clock::time_point last_seen{};
  size_t last_size = 0;
  /// Short caller-supplied preview of the last payload (e.g. a truncated
  /// JSON dump or a blob summary); empty if record() was never given one.
  std::string last_sample;
};

/**
 * @brief Thread-safe sliding-window aggregator of per-topic msg/s and
 * bytes/s, fed by record() and read back via snapshot().
 *
 * Only events within the trailing `window` (relative to the timestamp passed
 * to record()/snapshot()) count toward the rate; total_messages/last_seen/
 * last_size/last_sample are lifetime bookkeeping that the window never
 * discards. record() prunes each topic's own expired events as it appends,
 * so memory stays bounded by (window duration x that topic's historical
 * peak rate); a topic that goes silent simply stops growing.
 */
class TopicStats {
public:
  explicit TopicStats(
      std::chrono::milliseconds window = std::chrono::seconds(5));

  /**
   * @brief Records one observed message for `topic`.
   *
   * @param topic The message's topic.
   * @param bytes Size of the message in bytes.
   * @param sample Optional short preview of the payload; when empty, the
   *   previous last_sample (if any) is left unchanged.
   * @param now Timestamp of the observation. Defaults to
   *   steady_clock::now() for production use; tests pass synthetic,
   *   monotonically-nondecreasing-per-topic values for determinism.
   */
  void record(const std::string &topic, size_t bytes,
              std::string sample = std::string(),
              std::chrono::steady_clock::time_point now =
                  std::chrono::steady_clock::now());

  /**
   * @brief Snapshot of every topic seen so far, as of `now`.
   *
   * Read-only: does not mutate any retained state (pruning of long-expired
   * events happens lazily, on the next record() for that topic). Sorted by
   * topic name for a stable, diff-friendly table.
   *
   * @param now Instant the trailing window is measured back from. Defaults
   *   to steady_clock::now(); tests pass a synthetic value.
   */
  std::vector<TopicStat> snapshot(std::chrono::steady_clock::time_point now =
                                       std::chrono::steady_clock::now()) const;

  /// Changes the trailing window used by future record()/snapshot() calls.
  void set_window(std::chrono::milliseconds window);
  std::chrono::milliseconds window() const;

  /// Forgets every topic. Not used by `mads top` today; exposed for tests
  /// and completeness.
  void clear();

private:
  struct Entry {
    // Ascending by timestamp (append-only in record(), pruned from the
    // front): (when, bytes) per observed message still within living memory
    // of the window.
    std::deque<std::pair<std::chrono::steady_clock::time_point, size_t>>
        events;
    size_t total_messages = 0;
    std::chrono::steady_clock::time_point last_seen{};
    size_t last_size = 0;
    std::string last_sample;
  };

  mutable std::mutex _mtx;
  std::chrono::milliseconds _window;
  // map (not unordered_map) so snapshot() iterates in topic-name order with
  // no extra sort step.
  std::map<std::string, Entry> _entries;
};

} // namespace Mads
