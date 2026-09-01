/*
   ____ _            _      ___   __  __          _
  / ___| | ___   ___| | __ / _ \ / _|/ _|___  ___| |_
 | |   | |/ _ \ / __| |/ /| | | | |_| |_/ __|/ _ \ __|
 | |___| | (_) | (__|   < | |_| |  _|  _\__ \  __/ |_
  \____|_|\___/ \___|_|\_\ \___/|_| |_| |___/\___|\__|

Pure, socket-free clock-offset math and cross-agent consensus, shared by the
broker exchange (source A) and the bus ping/pong (source B) that measure
`Agent::clock_offset()`. No ZMQ, no Agent dependency: callers pass in
timestamps and a `now`, exactly like Mads::TopicStats, so the NTP-style
four-timestamp estimate, the domain-consensus rule, and the passive skew
tracker used by `mads top` are all directly unit-testable with synthetic
values -- no sockets, no real sleeps.

Author(s): Paolo Bosetti
*/
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace Mads {

/**
 * @brief One four-timestamp exchange, all fields microseconds since the Unix
 * epoch: t1/t4 read from the initiator's clock, t2/t3 from the responder's.
 */
struct ClockSample {
  int64_t t1 = 0, t2 = 0, t3 = 0, t4 = 0;
  /// t4-t1 as measured on a steady_clock, immune to a wall-clock step
  /// landing inside the exchange. 0 means "fall back to t4-t1".
  int64_t local_elapsed_us = 0;
};

/// Which mechanism produced a ClockOffsetResult.
enum class ClockSource { None = 0, Broker = 1, Peer = 2 };

/**
 * @brief Result of a clock-offset measurement: `offset_us` added to a clock
 * in the measured domain yields broker-host time (broker_host_clock -
 * domain_clock). `source`/`hops`/`origin_agent_id`/`seq` are Agent-level
 * bookkeeping the estimator does not fill in -- only estimate()'s own
 * offset_us/delay_us/samples/valid are computed from the timestamps.
 */
struct ClockOffsetResult {
  int64_t offset_us = 0;
  int64_t delay_us = 0;
  int64_t jitter_us = 0;
  size_t samples = 0;
  /// 0 = measured directly against the broker; >0 = chained through that
  /// many peer hops (a "peer"-source measurement composed on top of another
  /// agent's own adopted offset).
  uint8_t hops = 0;
  ClockSource source = ClockSource::None;
  /// Which agent's measurement this is -- the tie-breaker and identity used
  /// by ClockConsensus's adoption rule.
  std::string origin_agent_id;
  uint64_t seq = 0;
  /// Local reception/measurement instant (steady_clock), used only for
  /// staleness -- never compared across hosts.
  std::chrono::steady_clock::time_point measured_at{};
  bool valid = false;

  /// "<origin_agent_id>:<seq>", stamped onto corrected payloads so two
  /// agents can be proven to have used the same offset.
  std::string clock_ref() const {
    return origin_agent_id + ":" + std::to_string(seq);
  }
};

/**
 * @brief NTP-style estimate from one four-timestamp exchange.
 *
 * offset_us = ((t2-t1) + (t3-t4)) / 2 (assumes a symmetric path; an
 * asymmetric one biases the estimate by half the asymmetry -- this is a
 * property of the algorithm, not a bug, and is why several samples and
 * ClockOffsetEstimator::best() matter).
 *
 * delay_us is (s.local_elapsed_us - (t3-t2)) when local_elapsed_us is
 * non-zero, so a system_clock step or manual `date` change occurring
 * *during* the exchange cannot produce a negative or absurd delay; it falls
 * back to (t4-t1) - (t3-t2) otherwise.
 *
 * Only offset_us/delay_us/samples(=1)/valid are set; source/hops/
 * origin_agent_id/seq are Agent-level bookkeeping filled in by the caller.
 */
ClockOffsetResult estimate(const ClockSample &s);

/**
 * @brief Accumulates ClockSamples and returns the classic NTP "best of N":
 * the retained sample with the smallest round-trip delay, i.e. the one
 * least polluted by queuing delay along the path.
 */
class ClockOffsetEstimator {
public:
  explicit ClockOffsetEstimator(size_t keep = 8);

  /// Estimates `s` and retains it, evicting the oldest sample once more
  /// than `keep` are held.
  void add(const ClockSample &s);

  /// The min-delay sample, with jitter_us/samples filled in across every
  /// retained sample. {valid=false} if add() was never called.
  ClockOffsetResult best() const;

  /// The median offset_us across every retained sample (delay_us/hops/
  /// source of the *median-offset* sample are carried along, for display
  /// only -- best() is what should drive adoption). {valid=false} if empty.
  ClockOffsetResult median() const;

  void reset();

private:
  int64_t jitter_us() const;
  size_t _keep;
  std::vector<ClockOffsetResult> _results;
};

/**
 * @brief Per-clock-domain adoption of the smallest-delay measurement heard
 * from any agent sharing that domain (§2.1 of the design): agents on one
 * host announce their own measurements, and every agent applies this same
 * deterministic rule -- so they converge on one offset without an election.
 *
 * The comparator (delay_us asc, hops asc, agent_id asc) is a total order:
 * the winner never depends on the order record() calls arrive in, and a
 * worse measurement can never displace an already-adopted better one.
 * record()/adopted() are the only clock reads this class needs, and both
 * take `now` as a parameter -- exactly like Mads::TopicStats -- so
 * convergence is directly testable with synthetic timestamps.
 */
class ClockConsensus {
public:
  explicit ClockConsensus(
      std::chrono::milliseconds stale = std::chrono::seconds(30));

  /// Records (or replaces) `domain`'s entry for `r.origin_agent_id`.
  /// Ignored if `r` is invalid or has no origin_agent_id. `now` -- not
  /// `r.measured_at` -- is what staleness is measured from, so a
  /// remotely-stamped instant (a different host's steady_clock) is never
  /// compared against this host's clock.
  void record(const std::string &domain, const ClockOffsetResult &r,
              std::chrono::steady_clock::time_point now =
                  std::chrono::steady_clock::now());

  /// The current winner for `domain` among non-stale entries, or
  /// {valid=false} if none is known.
  ClockOffsetResult adopted(const std::string &domain,
                            std::chrono::steady_clock::time_point now =
                                std::chrono::steady_clock::now()) const;

  /// True iff `agent_id` is `domain`'s current winner, i.e. the one
  /// expected to keep re-measuring when clock_interval_ms > 0 (§2.3): a
  /// deterministic function of adopted(), so exactly one agent per domain
  /// ever answers true.
  bool is_winner(const std::string &domain, const std::string &agent_id,
                std::chrono::steady_clock::time_point now =
                    std::chrono::steady_clock::now()) const;

  void clear();

private:
  mutable std::mutex _mtx;
  std::chrono::milliseconds _stale;
  // domain -> (origin_agent_id -> its latest announced/measured result).
  std::map<std::string, std::map<std::string, ClockOffsetResult>> _domains;
};

/**
 * @brief Snapshot of one host's passive clock skew, as computed by
 * HostSkewStats::snapshot() at a given instant.
 */
struct HostSkewStat {
  std::string host;
  /// Minimum of (local_rx_time_us - payload_timestamp_us) observed within
  /// the trailing window -- a tight upper bound on (offset + one-way
  /// delay), never a clean offset on its own (see HostSkewStats).
  int64_t min_skew_us = 0;
  bool has_value = false;
  size_t samples = 0;
  std::chrono::steady_clock::time_point last_seen{};
};

/**
 * @brief Thread-safe sliding-window tracker of per-host clock skew, fed by
 * `mads top`'s passive mode: for every received message, record
 * (local_rx_time - payload's own `timestamp` field), both as epoch
 * microseconds. No protocol at all -- works against any existing MADS
 * deployment, since `timestamp`/`hostname` are already stamped on every
 * published message by Agent::publish().
 *
 * The *minimum* skew over the window is what gets reported: it is the
 * sample least polluted by queuing/network delay, and so the tightest
 * available upper bound on the true offset. It can never separate offset
 * from one-way delay the way the four-timestamp exchange can -- callers
 * must label it "skew", never "offset".
 *
 * Same shape and thread-safety as Mads::TopicStats: record() prunes
 * expired events as it appends; snapshot() is read-only.
 */
class HostSkewStats {
public:
  explicit HostSkewStats(
      std::chrono::milliseconds window = std::chrono::seconds(10));

  void record(const std::string &host, int64_t skew_us,
              std::chrono::steady_clock::time_point now =
                  std::chrono::steady_clock::now());

  std::vector<HostSkewStat>
  snapshot(std::chrono::steady_clock::time_point now =
               std::chrono::steady_clock::now()) const;

  void clear();

private:
  struct Entry {
    std::deque<std::pair<std::chrono::steady_clock::time_point, int64_t>>
        events;
    std::chrono::steady_clock::time_point last_seen{};
  };

  mutable std::mutex _mtx;
  std::chrono::milliseconds _window;
  std::map<std::string, Entry> _entries;
};

} // namespace Mads
