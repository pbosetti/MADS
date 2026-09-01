#include "clock_offset.hpp"
#include <algorithm>

namespace Mads {

ClockOffsetResult estimate(const ClockSample &s) {
  ClockOffsetResult r;
  const int64_t theta = ((s.t2 - s.t1) + (s.t3 - s.t4)) / 2;
  const int64_t responder_processing = s.t3 - s.t2;
  const int64_t delay = s.local_elapsed_us != 0
                            ? s.local_elapsed_us - responder_processing
                            : (s.t4 - s.t1) - responder_processing;
  r.offset_us = theta;
  r.delay_us = delay;
  r.samples = 1;
  r.valid = true;
  return r;
}

ClockOffsetEstimator::ClockOffsetEstimator(size_t keep) : _keep(keep) {}

void ClockOffsetEstimator::add(const ClockSample &s) {
  _results.push_back(estimate(s));
  if (_results.size() > _keep) {
    _results.erase(_results.begin());
  }
}

int64_t ClockOffsetEstimator::jitter_us() const {
  if (_results.size() < 2)
    return 0;
  int64_t mn = _results.front().delay_us;
  int64_t mx = mn;
  for (auto const &r : _results) {
    mn = std::min(mn, r.delay_us);
    mx = std::max(mx, r.delay_us);
  }
  return mx - mn;
}

ClockOffsetResult ClockOffsetEstimator::best() const {
  if (_results.empty())
    return {};
  auto it = std::min_element(
      _results.begin(), _results.end(),
      [](const ClockOffsetResult &a, const ClockOffsetResult &b) {
        return a.delay_us < b.delay_us;
      });
  ClockOffsetResult r = *it;
  r.samples = _results.size();
  r.jitter_us = jitter_us();
  return r;
}

ClockOffsetResult ClockOffsetEstimator::median() const {
  if (_results.empty())
    return {};
  std::vector<int64_t> offsets;
  offsets.reserve(_results.size());
  for (auto const &r : _results) {
    offsets.push_back(r.offset_us);
  }
  std::sort(offsets.begin(), offsets.end());
  ClockOffsetResult r = _results.front();
  r.offset_us = offsets[offsets.size() / 2];
  r.samples = _results.size();
  r.jitter_us = jitter_us();
  return r;
}

void ClockOffsetEstimator::reset() { _results.clear(); }

namespace {
// The §2.1 total order: smallest delay wins, hops break a tie in favour of a
// more-direct measurement, agent_id makes the order total so exactly one
// entry ever compares "best".
bool clock_result_better(const ClockOffsetResult &a,
                         const ClockOffsetResult &b) {
  if (a.delay_us != b.delay_us)
    return a.delay_us < b.delay_us;
  if (a.hops != b.hops)
    return a.hops < b.hops;
  return a.origin_agent_id < b.origin_agent_id;
}
} // namespace

ClockConsensus::ClockConsensus(std::chrono::milliseconds stale)
    : _stale(stale) {}

void ClockConsensus::record(const std::string &domain,
                            const ClockOffsetResult &r,
                            std::chrono::steady_clock::time_point now) {
  if (!r.valid || r.origin_agent_id.empty())
    return;
  std::lock_guard<std::mutex> lock(_mtx);
  ClockOffsetResult stamped = r;
  stamped.measured_at = now;
  _domains[domain][r.origin_agent_id] = stamped;
}

ClockOffsetResult
ClockConsensus::adopted(const std::string &domain,
                        std::chrono::steady_clock::time_point now) const {
  std::lock_guard<std::mutex> lock(_mtx);
  auto dom_it = _domains.find(domain);
  if (dom_it == _domains.end())
    return {};
  const ClockOffsetResult *best = nullptr;
  for (auto const &[agent_id, r] : dom_it->second) {
    if (now - r.measured_at > _stale)
      continue;
    if (!best || clock_result_better(r, *best))
      best = &r;
  }
  return best ? *best : ClockOffsetResult{};
}

bool ClockConsensus::is_winner(
    const std::string &domain, const std::string &agent_id,
    std::chrono::steady_clock::time_point now) const {
  auto a = adopted(domain, now);
  return a.valid && a.origin_agent_id == agent_id;
}

void ClockConsensus::clear() {
  std::lock_guard<std::mutex> lock(_mtx);
  _domains.clear();
}

HostSkewStats::HostSkewStats(std::chrono::milliseconds window)
    : _window(window) {}

void HostSkewStats::record(const std::string &host, int64_t skew_us,
                           std::chrono::steady_clock::time_point now) {
  std::lock_guard<std::mutex> lock(_mtx);
  Entry &e = _entries[host];
  while (!e.events.empty() && (now - e.events.front().first) > _window) {
    e.events.pop_front();
  }
  e.events.emplace_back(now, skew_us);
  e.last_seen = now;
}

std::vector<HostSkewStat>
HostSkewStats::snapshot(std::chrono::steady_clock::time_point now) const {
  std::lock_guard<std::mutex> lock(_mtx);
  std::vector<HostSkewStat> out;
  out.reserve(_entries.size());
  for (auto const &[host, e] : _entries) {
    HostSkewStat stat;
    stat.host = host;
    stat.last_seen = e.last_seen;
    for (auto it = e.events.rbegin(); it != e.events.rend(); ++it) {
      if ((now - it->first) > _window)
        break;
      if (!stat.has_value || it->second < stat.min_skew_us) {
        stat.min_skew_us = it->second;
      }
      stat.has_value = true;
      ++stat.samples;
    }
    out.push_back(std::move(stat));
  }
  return out;
}

void HostSkewStats::clear() {
  std::lock_guard<std::mutex> lock(_mtx);
  _entries.clear();
}

} // namespace Mads
