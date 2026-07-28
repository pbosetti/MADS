#include "topic_stats.hpp"

namespace Mads {

TopicStats::TopicStats(std::chrono::milliseconds window) : _window(window) {}

void TopicStats::record(const std::string &topic, size_t bytes,
                        std::string sample,
                        std::chrono::steady_clock::time_point now) {
  std::lock_guard<std::mutex> lock(_mtx);
  Entry &entry = _entries[topic];
  // Drop already-expired events before appending, so a topic that keeps
  // publishing never grows its deque past ~(window / inter-arrival time).
  while (!entry.events.empty() &&
        (now - entry.events.front().first) > _window) {
    entry.events.pop_front();
  }
  entry.events.emplace_back(now, bytes);
  entry.total_messages++;
  entry.last_seen = now;
  entry.last_size = bytes;
  if (!sample.empty()) {
    entry.last_sample = std::move(sample);
  }
}

std::vector<TopicStat>
TopicStats::snapshot(std::chrono::steady_clock::time_point now) const {
  std::lock_guard<std::mutex> lock(_mtx);
  std::vector<TopicStat> out;
  out.reserve(_entries.size());
  const double window_s = std::chrono::duration<double>(_window).count();

  for (auto const &[topic, entry] : _entries) {
    // Read-only windowed count: walk from the most recent event backward and
    // stop at the first one older than `now - window`, rather than mutating
    // `entry.events` (this method is const; pruning-on-write happens in
    // record()). A topic that has gone silent since its last record() simply
    // reports zero here without needing another record() call to age out.
    size_t count = 0;
    size_t bytes = 0;
    for (auto it = entry.events.rbegin(); it != entry.events.rend(); ++it) {
      if ((now - it->first) > _window) {
        break;
      }
      ++count;
      bytes += it->second;
    }

    TopicStat stat;
    stat.topic = topic;
    stat.window_messages = count;
    stat.window_bytes = bytes;
    stat.messages_per_second = window_s > 0.0 ? count / window_s : 0.0;
    stat.bytes_per_second = window_s > 0.0 ? bytes / window_s : 0.0;
    stat.total_messages = entry.total_messages;
    stat.last_seen = entry.last_seen;
    stat.last_size = entry.last_size;
    stat.last_sample = entry.last_sample;
    out.push_back(std::move(stat));
  }
  return out;
}

void TopicStats::set_window(std::chrono::milliseconds window) {
  std::lock_guard<std::mutex> lock(_mtx);
  _window = window;
}

std::chrono::milliseconds TopicStats::window() const {
  std::lock_guard<std::mutex> lock(_mtx);
  return _window;
}

void TopicStats::clear() {
  std::lock_guard<std::mutex> lock(_mtx);
  _entries.clear();
}

} // namespace Mads
