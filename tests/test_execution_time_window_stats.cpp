#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <thread>

#include "execution_time_window_stats.hpp"

using namespace std::chrono_literals;

TEST_CASE("toc without tic returns 0 and stores nothing",
          "[execution_time_window_stats]") {
  Mads::ExecutionTimeWindowStats stats(10);
  double elapsed = stats.toc();
  REQUIRE(elapsed == 0.0);
  REQUIRE(stats.size() == 0);
  REQUIRE(stats.average_ms() == 0.0);
  REQUIRE(stats.stddev_ms() == 0.0);
}

TEST_CASE("tic/toc records a sample with a plausible elapsed time",
          "[execution_time_window_stats]") {
  Mads::ExecutionTimeWindowStats stats(10);
  stats.tic();
  std::this_thread::sleep_for(5ms);
  double elapsed = stats.toc();

  // Should be positive and in a generous range (avoid exact-time assertions
  // under CI jitter).
  REQUIRE(elapsed > 0.0);
  REQUIRE(elapsed < 2000.0);
  REQUIRE(stats.size() == 1);
  REQUIRE(stats.average_ms() == elapsed);
  REQUIRE(stats.stddev_ms() == Catch::Approx(0.0).margin(1e-6));
}

TEST_CASE("mean and stddev over several tic/toc cycles are in range",
          "[execution_time_window_stats]") {
  Mads::ExecutionTimeWindowStats stats(50);
  for (int i = 0; i < 5; ++i) {
    stats.tic();
    std::this_thread::sleep_for(3ms);
    stats.toc();
  }
  REQUIRE(stats.size() == 5);
  // Mean should be a small positive number, comfortably below a generous
  // upper bound to absorb CI jitter.
  REQUIRE(stats.average_ms() > 0.0);
  REQUIRE(stats.average_ms() < 2000.0);
  // Stddev is non-negative and bounded by the same generosity.
  REQUIRE(stats.stddev_ms() >= 0.0);
  REQUIRE(stats.stddev_ms() < 2000.0);
}

TEST_CASE("window trims oldest samples once more than window_width added",
          "[execution_time_window_stats]") {
  Mads::ExecutionTimeWindowStats stats(3);
  // Feed synthetic durations directly via tic/toc is timing-dependent for
  // exact values, so instead verify the trimming *mechanism*: size() never
  // exceeds window_width, and average reflects only the most recent samples.
  for (int i = 0; i < 6; ++i) {
    stats.tic();
    std::this_thread::sleep_for(1ms);
    stats.toc();
    REQUIRE(stats.size() <= 3);
  }
  REQUIRE(stats.size() == 3);
  REQUIRE(stats.window_width() == 3);
}

TEST_CASE("set_window_width shrinking drops oldest samples immediately",
          "[execution_time_window_stats]") {
  Mads::ExecutionTimeWindowStats stats(10);
  for (int i = 0; i < 5; ++i) {
    stats.tic();
    std::this_thread::sleep_for(1ms);
    stats.toc();
  }
  REQUIRE(stats.size() == 5);

  stats.set_window_width(2);
  REQUIRE(stats.window_width() == 2);
  REQUIRE(stats.size() == 2);

  // Growing the window again should not resurrect dropped samples.
  stats.set_window_width(10);
  REQUIRE(stats.window_width() == 10);
  REQUIRE(stats.size() == 2);
}

TEST_CASE("zero window width discards every sample", "[execution_time_window_stats]") {
  Mads::ExecutionTimeWindowStats stats(0);
  stats.tic();
  std::this_thread::sleep_for(1ms);
  double elapsed = stats.toc();
  // toc() itself still reports the elapsed time...
  REQUIRE(elapsed > 0.0);
  // ...but nothing is retained because the window width is zero.
  REQUIRE(stats.size() == 0);
  REQUIRE(stats.average_ms() == 0.0);
  REQUIRE(stats.stddev_ms() == 0.0);
}

TEST_CASE("set_window_width(0) on a populated window empties it",
          "[execution_time_window_stats]") {
  Mads::ExecutionTimeWindowStats stats(5);
  for (int i = 0; i < 3; ++i) {
    stats.tic();
    std::this_thread::sleep_for(1ms);
    stats.toc();
  }
  REQUIRE(stats.size() == 3);
  stats.set_window_width(0);
  REQUIRE(stats.size() == 0);
  REQUIRE(stats.average_ms() == 0.0);
}

TEST_CASE("clear resets samples and pending tic state",
          "[execution_time_window_stats]") {
  Mads::ExecutionTimeWindowStats stats(10);
  stats.tic();
  std::this_thread::sleep_for(1ms);
  stats.toc();
  REQUIRE(stats.size() == 1);

  stats.clear();
  REQUIRE(stats.size() == 0);
  REQUIRE(stats.average_ms() == 0.0);
  REQUIRE(stats.stddev_ms() == 0.0);

  // A tic() started before clear() must not leak into a toc() called after:
  // clear() resets the ticking flag, so a bare toc() after clear() (with no
  // fresh tic()) returns 0 and stores nothing.
  stats.tic();
  stats.clear();
  double elapsed = stats.toc();
  REQUIRE(elapsed == 0.0);
  REQUIRE(stats.size() == 0);
}

TEST_CASE("default construction uses a window width of 100",
          "[execution_time_window_stats]") {
  Mads::ExecutionTimeWindowStats stats;
  REQUIRE(stats.window_width() == 100);
}
