// Unit tests for Mads::TopicStats (src/topic_stats.hpp/.cpp): the windowed
// msg/s and bytes/s aggregator behind `mads top`. Pure: no sockets, no
// Agent -- every test feeds a synthetic, explicitly-timestamped message
// stream and asserts the computed rates, using steady_clock::now() as an
// arbitrary reference point offset by hand-picked deltas (never real
// sleeps), so this suite is fast and immune to scheduler jitter.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

#include "topic_stats.hpp"

using namespace std::chrono_literals;

namespace {
// A fixed, arbitrary reference instant every test offsets from.
std::chrono::steady_clock::time_point epoch() {
  static const auto t0 = std::chrono::steady_clock::now();
  return t0;
}
} // namespace

TEST_CASE("snapshot() of a fresh TopicStats is empty", "[top_stats]") {
  Mads::TopicStats stats(1s);
  auto snap = stats.snapshot(epoch());
  REQUIRE(snap.empty());
}

TEST_CASE("record() populates a single topic with correct lifetime and "
          "window counts",
          "[top_stats]") {
  Mads::TopicStats stats(1s);
  stats.record("sensors/x", 100, "sample-1", epoch());
  auto snap = stats.snapshot(epoch());
  REQUIRE(snap.size() == 1);
  CHECK(snap[0].topic == "sensors/x");
  CHECK(snap[0].total_messages == 1);
  CHECK(snap[0].window_messages == 1);
  CHECK(snap[0].window_bytes == 100);
  CHECK(snap[0].last_size == 100);
  CHECK(snap[0].last_sample == "sample-1");
  CHECK(snap[0].last_seen == epoch());
}

TEST_CASE("messages/bytes per second are averaged over the configured "
          "window",
          "[top_stats]") {
  // 1-second window; 5 messages of 200 bytes each land exactly at the start
  // of the window (t=0..4 * 200ms), snapshot taken at t=1000ms so all 5 are
  // still inside the trailing 1s window ending there.
  Mads::TopicStats stats(1000ms);
  for (int i = 0; i < 5; ++i) {
    stats.record("sensors/x", 200, "", epoch() + i * 200ms);
  }
  auto snap = stats.snapshot(epoch() + 1000ms);
  REQUIRE(snap.size() == 1);
  CHECK(snap[0].window_messages == 5);
  CHECK(snap[0].window_bytes == 1000);
  CHECK(snap[0].messages_per_second == Catch::Approx(5.0));
  CHECK(snap[0].bytes_per_second == Catch::Approx(1000.0));
  CHECK(snap[0].total_messages == 5);
}

TEST_CASE("events older than the window are excluded from the rate but not "
          "from the lifetime total",
          "[top_stats]") {
  Mads::TopicStats stats(1000ms);
  // Two messages well before the window, three recent ones.
  stats.record("t", 10, "", epoch());
  stats.record("t", 10, "", epoch() + 100ms);
  stats.record("t", 10, "", epoch() + 5000ms);
  stats.record("t", 10, "", epoch() + 5200ms);
  stats.record("t", 10, "", epoch() + 5400ms);

  // Snapshot at t=5400ms: only the last three (5000/5200/5400) fall within
  // the trailing 1000ms window ending there.
  auto snap = stats.snapshot(epoch() + 5400ms);
  REQUIRE(snap.size() == 1);
  CHECK(snap[0].window_messages == 3);
  CHECK(snap[0].window_bytes == 30);
  CHECK(snap[0].messages_per_second == Catch::Approx(3.0));
  // Lifetime total still counts every record() call, window or not.
  CHECK(snap[0].total_messages == 5);
}

TEST_CASE("snapshot() at a `now` far past the last record() reports zero "
          "rate without discarding last-seen/last-sample",
          "[top_stats]") {
  Mads::TopicStats stats(1000ms);
  stats.record("t", 42, "last-payload", epoch());

  // No further record() calls: a snapshot taken long after the window has
  // elapsed must report zero window activity (read-only, no mutation
  // required to reflect this) while last_seen/last_sample/total_messages
  // remain the historical values.
  auto snap = stats.snapshot(epoch() + 60s);
  REQUIRE(snap.size() == 1);
  CHECK(snap[0].window_messages == 0);
  CHECK(snap[0].window_bytes == 0);
  CHECK(snap[0].messages_per_second == Catch::Approx(0.0));
  CHECK(snap[0].bytes_per_second == Catch::Approx(0.0));
  CHECK(snap[0].total_messages == 1);
  CHECK(snap[0].last_seen == epoch());
  CHECK(snap[0].last_sample == "last-payload");
}

TEST_CASE("multiple topics are tracked independently and snapshot() sorts "
          "by topic name",
          "[top_stats]") {
  Mads::TopicStats stats(1000ms);
  stats.record("z_topic", 5, "", epoch());
  stats.record("a_topic", 7, "", epoch());
  stats.record("m_topic", 9, "", epoch());
  stats.record("a_topic", 7, "", epoch() + 10ms);

  auto snap = stats.snapshot(epoch() + 10ms);
  REQUIRE(snap.size() == 3);
  CHECK(snap[0].topic == "a_topic");
  CHECK(snap[0].total_messages == 2);
  CHECK(snap[1].topic == "m_topic");
  CHECK(snap[1].total_messages == 1);
  CHECK(snap[2].topic == "z_topic");
  CHECK(snap[2].total_messages == 1);
}

TEST_CASE("an empty sample argument leaves the previous last_sample "
          "unchanged",
          "[top_stats]") {
  Mads::TopicStats stats(1000ms);
  stats.record("t", 1, "first", epoch());
  stats.record("t", 1, "", epoch() + 10ms); // no sample this time
  auto snap = stats.snapshot(epoch() + 10ms);
  REQUIRE(snap.size() == 1);
  CHECK(snap[0].last_sample == "first");
  // last_size/last_seen still advance even when sample is empty.
  CHECK(snap[0].last_seen == epoch() + 10ms);
}

TEST_CASE("set_window()/window() change the trailing window used by future "
          "record()/snapshot() calls",
          "[top_stats]") {
  Mads::TopicStats stats(1000ms);
  CHECK(stats.window() == 1000ms);
  stats.record("t", 1, "", epoch());
  stats.record("t", 1, "", epoch() + 500ms);

  stats.set_window(200ms);
  CHECK(stats.window() == 200ms);
  auto snap = stats.snapshot(epoch() + 500ms);
  REQUIRE(snap.size() == 1);
  // Only the second record (at +500ms) is within a 200ms trailing window
  // ending at +500ms; the first (at +0ms) is 500ms old, outside it.
  CHECK(snap[0].window_messages == 1);
  CHECK(snap[0].total_messages == 2);
}

TEST_CASE("clear() forgets every topic", "[top_stats]") {
  Mads::TopicStats stats(1000ms);
  stats.record("a", 1, "", epoch());
  stats.record("b", 1, "", epoch());
  REQUIRE(stats.snapshot(epoch()).size() == 2);
  stats.clear();
  REQUIRE(stats.snapshot(epoch()).empty());
}

TEST_CASE("default construction uses a 5-second window", "[top_stats]") {
  Mads::TopicStats stats;
  CHECK(stats.window() == 5000ms);
}
