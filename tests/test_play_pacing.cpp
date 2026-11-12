// Unit tests for mads-play's --rate pacing schedule (src/main/play_pacing.hpp).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>

#include "main/play_pacing.hpp"
#include "precise_wait.hpp"

using Mads::Play::Pacer;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {
constexpr int64_t ms = 1'000'000; // ns per millisecond
}

TEST_CASE("Pacer scales offsets by rate", "[play_pacing]") {
  Pacer pacer(2.0);
  CHECK(pacer.offset(0).count() == 0);
  CHECK(pacer.offset(10 * ms).count() == 5 * ms);
  CHECK(pacer.offset(20 * ms).count() == 10 * ms);
}

TEST_CASE("Pacer does not drift over many records", "[play_pacing]") {
  Pacer pacer(3.0);
  std::chrono::nanoseconds last{0};
  for (int64_t i = 0; i < 1'000'000; ++i) {
    last = pacer.offset(i * 1000);
  }
  CHECK(last.count() == std::llround(999'999'000 / 3.0));
  CHECK(pacer.elapsed_ns() == 999'999'000);
}

TEST_CASE("Pacer handles equal and backward timestamps", "[play_pacing]") {
  Pacer pacer(1.0);
  CHECK(pacer.offset(0).count() == 0);
  CHECK(pacer.offset(10 * ms).count() == 10 * ms);
  // Equal timestamp shares the offset.
  CHECK(pacer.offset(10 * ms).count() == 10 * ms);
  // Backward step adds no bag time.
  CHECK(pacer.offset(5 * ms).count() == 10 * ms);
  // Next positive gap continues from the previous timestamp (5 ms -> 15 ms).
  CHECK(pacer.offset(15 * ms).count() == 20 * ms);
}

TEST_CASE("Pacer seam continues one span after the previous pass", "[play_pacing]") {
  Pacer pacer(2.0);
  CHECK(pacer.offset(0).count() == 0);
  CHECK(pacer.offset(10 * ms).count() == 5 * ms);
  CHECK(pacer.offset(20 * ms).count() == 10 * ms);
  pacer.seam(30 * ms);
  // Second pass: ts 0 is one mean gap after the last record (30 ms / 2 = 15 ms).
  CHECK(pacer.offset(0).count() == 15 * ms);
  CHECK(pacer.offset(10 * ms).count() == 20 * ms);
}

TEST_CASE("precise_wait_until returns immediately for a past deadline", "[play_pacing]") {
  auto t0 = Clock::now();
  CHECK(Mads::precise_wait_until(t0 - 5s, [] { return true; }));
  CHECK(Clock::now() - t0 < 100ms);
}

TEST_CASE("precise_wait_until returns false at once when keep_going starts false", "[play_pacing]") {
  auto t0 = Clock::now();
  CHECK(!Mads::precise_wait_until(t0 + 5s, [] { return false; }));
  CHECK(Clock::now() - t0 < 100ms);
}

TEST_CASE("precise_wait_until keeps an absolute schedule", "[play_pacing][timing]") {
  // The old relative sleep_for pattern took ~3.1 s on Windows for this loop.
  auto t0 = Clock::now();
  for (int k = 1; k <= 200; ++k) {
    REQUIRE(Mads::precise_wait_until(t0 + k * 1ms, [] { return true; }));
  }
  double elapsed_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  CHECK(elapsed_ms >= 0.95 * 200.0);
  CHECK(elapsed_ms <= 1.25 * 200.0);
}

TEST_CASE("precise_wait_until stops early when keep_going turns false", "[play_pacing][timing]") {
  std::atomic<bool> go{true};
  std::thread stopper([&go] {
    std::this_thread::sleep_for(200ms);
    go = false;
  });
  auto t0 = Clock::now();
  bool r = Mads::precise_wait_until(t0 + 5s, [&go] { return go.load(); });
  auto elapsed = Clock::now() - t0;
  stopper.join();
  CHECK(!r);
  CHECK(elapsed < 200ms + 150ms);
}
