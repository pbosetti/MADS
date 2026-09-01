// Unit tests for Mads::estimate()/ClockOffsetEstimator/ClockConsensus/
// HostSkewStats (src/clock_offset.hpp/.cpp). Pure: no sockets, no Agent --
// every test feeds hand-computed timestamps and synthetic steady_clock
// instants, so this suite is fast and immune to scheduler jitter (same
// pattern as test_top_stats.cpp).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "clock_offset.hpp"

using namespace std::chrono_literals;
using Mads::ClockConsensus;
using Mads::ClockOffsetEstimator;
using Mads::ClockOffsetResult;
using Mads::ClockSample;
using Mads::ClockSource;
using Mads::estimate;
using Mads::HostSkewStats;

namespace {
std::chrono::steady_clock::time_point epoch() {
  static const auto t0 = std::chrono::steady_clock::now();
  return t0;
}

ClockOffsetResult make_result(int64_t delay_us, uint8_t hops,
                              std::string agent_id, int64_t offset_us = 0) {
  ClockOffsetResult r;
  r.offset_us = offset_us;
  r.delay_us = delay_us;
  r.hops = hops;
  r.origin_agent_id = std::move(agent_id);
  r.source = ClockSource::Broker;
  r.valid = true;
  return r;
}
} // namespace

// ---- estimate() ----------------------------------------------------------

TEST_CASE("estimate() recovers a known offset under a symmetric delay",
          "[clock_offset]") {
  // True offset: responder is 5000us ahead. Symmetric 1000us one-way delay.
  const int64_t true_offset = 5000;
  const int64_t one_way = 1000;
  ClockSample s;
  s.t1 = 1'000'000;
  s.t2 = s.t1 + one_way + true_offset;
  s.t3 = s.t2 + 10; // 10us of responder-side processing
  s.t4 = s.t3 - true_offset + one_way;
  s.local_elapsed_us = s.t4 - s.t1;

  auto r = estimate(s);
  REQUIRE(r.valid);
  CHECK(r.offset_us == true_offset);
  CHECK(r.delay_us == 2 * one_way);
  CHECK(r.samples == 1);
}

TEST_CASE("estimate() biases the offset by half an asymmetric delay",
          "[clock_offset]") {
  // Classic NTP bias: with outbound delay != inbound delay, the offset
  // error is exactly (out_delay - in_delay)/2. Documented, not hidden.
  const int64_t true_offset = 0;
  const int64_t out_delay = 1000;
  const int64_t in_delay = 3000;
  ClockSample s;
  s.t1 = 0;
  s.t2 = s.t1 + out_delay + true_offset;
  s.t3 = s.t2;
  s.t4 = s.t3 - true_offset + in_delay;
  s.local_elapsed_us = s.t4 - s.t1;

  auto r = estimate(s);
  REQUIRE(r.valid);
  CHECK(r.offset_us == true_offset + (out_delay - in_delay) / 2);
}

TEST_CASE("estimate() uses local_elapsed_us so a mid-exchange system_clock "
          "step cannot produce a negative delay",
          "[clock_offset]") {
  ClockSample s;
  s.t1 = 1'000'000;
  s.t2 = 1'000'500;
  s.t3 = 1'000'600;
  // t4 makes it LOOK like the exchange took negative time on system_clock
  // (e.g. the wall clock was stepped backward mid-flight).
  s.t4 = 500'000;
  // But the steady_clock elapsed is sane: 2000us round trip.
  s.local_elapsed_us = 2000;

  auto r = estimate(s);
  REQUIRE(r.valid);
  CHECK(r.delay_us == s.local_elapsed_us - (s.t3 - s.t2));
  CHECK(r.delay_us > 0);
}

TEST_CASE("estimate() falls back to t4-t1 when local_elapsed_us is unset",
          "[clock_offset]") {
  ClockSample s;
  s.t1 = 0;
  s.t2 = 600;
  s.t3 = 700;
  s.t4 = 1000;
  s.local_elapsed_us = 0;

  auto r = estimate(s);
  CHECK(r.delay_us == (s.t4 - s.t1) - (s.t3 - s.t2));
}

// ---- ClockOffsetEstimator --------------------------------------------------

TEST_CASE("ClockOffsetEstimator::best() picks the min-delay sample among "
          "outliers",
          "[clock_offset]") {
  ClockOffsetEstimator est;
  // Three noisy, high-delay samples and one clean low-delay one.
  for (int64_t delay : {5000, 8000, 6000}) {
    ClockSample s;
    s.t1 = 0;
    s.t2 = delay / 2 + 100; // offset ~100
    s.t3 = s.t2;
    s.t4 = delay;
    s.local_elapsed_us = delay;
    est.add(s);
  }
  ClockSample clean;
  clean.t1 = 0;
  clean.t2 = 250 + 42; // offset 42, delay 500
  clean.t3 = clean.t2;
  clean.t4 = 500;
  clean.local_elapsed_us = 500;
  est.add(clean);

  auto best = est.best();
  REQUIRE(best.valid);
  CHECK(best.delay_us == 500);
  CHECK(best.offset_us == 42);
  CHECK(best.samples == 4);
}

TEST_CASE("ClockOffsetEstimator::best() on an empty estimator is invalid",
          "[clock_offset]") {
  ClockOffsetEstimator est;
  CHECK_FALSE(est.best().valid);
  CHECK_FALSE(est.median().valid);
}

TEST_CASE("ClockOffsetEstimator evicts the oldest sample beyond `keep`",
          "[clock_offset]") {
  ClockOffsetEstimator est(2);
  auto add = [&](int64_t offset, int64_t delay) {
    ClockSample s;
    s.t1 = 0;
    s.t2 = delay / 2 + offset;
    s.t3 = s.t2;
    s.t4 = delay;
    s.local_elapsed_us = delay;
    est.add(s);
  };
  add(1, 100); // evicted
  add(2, 50);
  add(3, 30);
  auto best = est.best();
  CHECK(best.samples == 2);
  CHECK(best.delay_us == 30);
}

// ---- ClockConsensus: the property this design exists to provide -----------

TEST_CASE("ClockConsensus converges to the same winner regardless of "
          "arrival order",
          "[clock_offset]") {
  std::vector<ClockOffsetResult> entries = {
      make_result(/*delay*/ 900, /*hops*/ 0, "agent-b", /*offset*/ 111),
      make_result(/*delay*/ 400, /*hops*/ 0, "agent-a", /*offset*/ 222),
      make_result(/*delay*/ 400, /*hops*/ 1, "agent-c", /*offset*/ 333),
      make_result(/*delay*/ 1500, /*hops*/ 0, "agent-d", /*offset*/ 444),
  };

  std::mt19937 rng(42);
  for (int trial = 0; trial < 20; ++trial) {
    auto shuffled = entries;
    std::shuffle(shuffled.begin(), shuffled.end(), rng);

    ClockConsensus consensus;
    for (auto const &e : shuffled) {
      consensus.record("domain-1", e, epoch());
    }
    auto winner = consensus.adopted("domain-1", epoch());
    REQUIRE(winner.valid);
    // agent-a: smallest delay (400) among all; ties with agent-c on delay
    // but agent-a has fewer hops.
    CHECK(winner.origin_agent_id == "agent-a");
    CHECK(winner.offset_us == 222);
  }
}

TEST_CASE("ClockConsensus: a worse late arrival never displaces the winner, "
          "a better one does",
          "[clock_offset]") {
  ClockConsensus consensus;
  consensus.record("d", make_result(500, 0, "a"), epoch());
  CHECK(consensus.adopted("d", epoch()).origin_agent_id == "a");

  // Worse (higher delay) from a different agent: winner unchanged.
  consensus.record("d", make_result(900, 0, "b"), epoch() + 1s);
  CHECK(consensus.adopted("d", epoch() + 1s).origin_agent_id == "a");

  // Better: displaces the winner.
  consensus.record("d", make_result(100, 0, "c"), epoch() + 2s);
  CHECK(consensus.adopted("d", epoch() + 2s).origin_agent_id == "c");
}

TEST_CASE("ClockConsensus ties on delay and hops break on agent_id",
          "[clock_offset]") {
  ClockConsensus consensus;
  consensus.record("d", make_result(500, 0, "zebra"), epoch());
  consensus.record("d", make_result(500, 0, "alpha"), epoch());
  CHECK(consensus.adopted("d", epoch()).origin_agent_id == "alpha");
}

TEST_CASE("ClockConsensus evicts stale entries from consideration",
          "[clock_offset]") {
  ClockConsensus consensus(1000ms);
  consensus.record("d", make_result(100, 0, "a"), epoch());
  consensus.record("d", make_result(900, 0, "b"), epoch());
  // At t=0, "a" (delay 100) wins.
  CHECK(consensus.adopted("d", epoch()).origin_agent_id == "a");
  // At t=1500ms, "a"'s entry (recorded at t=0) is stale (>1000ms old); only
  // "b" remains fresh if it's re-recorded, but here neither is re-recorded,
  // so both are stale and nothing is adopted.
  auto stale = consensus.adopted("d", epoch() + 1500ms);
  CHECK_FALSE(stale.valid);
}

TEST_CASE("ClockConsensus::is_winner selects exactly one agent per domain",
          "[clock_offset]") {
  ClockConsensus consensus;
  consensus.record("d", make_result(500, 0, "a"), epoch());
  consensus.record("d", make_result(700, 0, "b"), epoch());
  consensus.record("d", make_result(900, 0, "c"), epoch());

  CHECK(consensus.is_winner("d", "a", epoch()));
  CHECK_FALSE(consensus.is_winner("d", "b", epoch()));
  CHECK_FALSE(consensus.is_winner("d", "c", epoch()));
  CHECK_FALSE(consensus.is_winner("d", "nonexistent", epoch()));
  // An unknown domain has no winner at all.
  CHECK_FALSE(consensus.is_winner("other-domain", "a", epoch()));
}

TEST_CASE("ClockConsensus.record() ignores an invalid or unattributed result",
          "[clock_offset]") {
  ClockConsensus consensus;
  ClockOffsetResult invalid; // valid=false by default
  consensus.record("d", invalid, epoch());
  CHECK_FALSE(consensus.adopted("d", epoch()).valid);

  ClockOffsetResult no_origin = make_result(100, 0, "");
  consensus.record("d", no_origin, epoch());
  CHECK_FALSE(consensus.adopted("d", epoch()).valid);
}

// ---- HostSkewStats ---------------------------------------------------------

TEST_CASE("HostSkewStats reports the minimum skew within the window",
          "[clock_offset]") {
  HostSkewStats stats(1000ms);
  stats.record("host-a", 5000, epoch());
  stats.record("host-a", 1200, epoch() + 100ms);
  stats.record("host-a", 3000, epoch() + 200ms);

  auto snap = stats.snapshot(epoch() + 200ms);
  REQUIRE(snap.size() == 1);
  CHECK(snap[0].host == "host-a");
  CHECK(snap[0].has_value);
  CHECK(snap[0].min_skew_us == 1200);
  CHECK(snap[0].samples == 3);
}

TEST_CASE("HostSkewStats expires samples outside the window", "[clock_offset]") {
  HostSkewStats stats(1000ms);
  stats.record("host-a", 100, epoch());
  auto snap = stats.snapshot(epoch() + 5000ms);
  REQUIRE(snap.size() == 1);
  CHECK_FALSE(snap[0].has_value);
  CHECK(snap[0].samples == 0);
}

TEST_CASE("HostSkewStats tracks a silent host as simply absent from a fresh "
          "snapshot until it is recorded",
          "[clock_offset]") {
  HostSkewStats stats(1000ms);
  auto snap = stats.snapshot(epoch());
  CHECK(snap.empty());
}
