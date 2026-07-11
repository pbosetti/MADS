#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <regex>
#include <sys/time.h>
#include <vector>

#include "mads_test_helpers.hpp"

// ---------------------------------------------------------------------------
// version() / check_version()
// ---------------------------------------------------------------------------

TEST_CASE("version returns the library version string", "[mads_core]") {
  REQUIRE(Mads::version() == std::string(LIB_VERSION));
}

TEST_CASE("check_version matches on identical minor version", "[mads_core]") {
  // The library's own version always matches its own check.
  REQUIRE(Mads::check_version(Mads::version()));
  // Same major.minor, different patch also matches, since only the part
  // before the last dot is compared. Derive the string from the library's
  // own version, which depends on the git tags visible at configure time.
  std::string v = Mads::version();
  REQUIRE(Mads::check_version(v.substr(0, v.find_last_of('.')) + ".999"));
}

TEST_CASE("check_version rejects a non-matching minor version",
          "[mads_core]") {
  REQUIRE_FALSE(Mads::check_version("v9.9.9"));
  REQUIRE_FALSE(Mads::check_version("v1.0.1"));
}

TEST_CASE("check_version throws on a dot-less version string",
          "[mads_core]") {
  REQUIRE_THROWS_AS(Mads::check_version("noversion"), std::runtime_error);
  REQUIRE_THROWS_AS(Mads::check_version(""), std::runtime_error);
}

// ---------------------------------------------------------------------------
// event_name() / event_map
// ---------------------------------------------------------------------------

TEST_CASE("event_map contains every event_type with a readable name",
          "[mads_core]") {
  REQUIRE(Mads::event_map.size() == 6);
  REQUIRE(Mads::event_name(Mads::event_type::marker) == "marker");
  REQUIRE(Mads::event_name(Mads::event_type::marker_in) == "marker in");
  REQUIRE(Mads::event_name(Mads::event_type::marker_out) == "marker out");
  REQUIRE(Mads::event_name(Mads::event_type::startup) == "startup");
  REQUIRE(Mads::event_name(Mads::event_type::shutdown) == "shutdown");
  REQUIRE(Mads::event_name(Mads::event_type::message) == "message");
}

// ---------------------------------------------------------------------------
// get_ISODate_time()
// ---------------------------------------------------------------------------

TEST_CASE("get_ISODate_time produces the expected shape", "[mads_core]") {
  auto now = std::chrono::system_clock::now();
  std::string s = Mads::get_ISODate_time(now);

  // "YYYY-MM-DDTHH:MM:SS.mmm+HHMM" (timezone suffix optional/platform
  // dependent, but the date/time/millis prefix is fixed width).
  std::regex re(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3})");
  REQUIRE(std::regex_search(s, re));
}

TEST_CASE("get_ISODate_time pads milliseconds to 3 digits", "[mads_core]") {
  using namespace std::chrono;
  // Construct a time_point at an exact second boundary, then add a small
  // millisecond offset via the `offset` parameter to control the ms value.
  auto now = system_clock::now();
  auto truncated = floor<seconds>(now);

  std::string s5 = Mads::get_ISODate_time(truncated, 5);
  std::string ms_part5 = s5.substr(20, 3); // after "...SS."
  REQUIRE(ms_part5 == "005");

  std::string s0 = Mads::get_ISODate_time(truncated, 0);
  std::string ms_part0 = s0.substr(20, 3);
  REQUIRE(ms_part0 == "000");

  std::string s123 = Mads::get_ISODate_time(truncated, 123);
  std::string ms_part123 = s123.substr(20, 3);
  REQUIRE(ms_part123 == "123");
}

// ---------------------------------------------------------------------------
// timecode()
// ---------------------------------------------------------------------------

TEST_CASE("timecode bins milliseconds to the fps grid", "[mads_core]") {
  using namespace std::chrono;

  // Build a time_point for today at a known local wall-clock second, then
  // add a millisecond offset to check the binning behavior.
  auto now = system_clock::now();
  time_t now_c = system_clock::to_time_t(now);
  auto sec_point = system_clock::from_time_t(now_c);

  // fps = 25 -> bin width 40ms. 47ms should bin down to 40ms -> 0.04s
  // fractional part.
  auto with_ms = sec_point + milliseconds(47);
  double tc = Mads::timecode(with_ms, 25);
  double frac = tc - std::floor(tc);
  // 47 / 40 = 1.175 -> floor -> 1 -> 1*40 = 40ms = 0.04s
  REQUIRE(frac == Catch::Approx(0.04).margin(0.001));

  // 0ms should bin to 0.
  auto with_zero = sec_point + milliseconds(0);
  double tc0 = Mads::timecode(with_zero, 25);
  double frac0 = tc0 - std::floor(tc0);
  REQUIRE(frac0 == Catch::Approx(0.0).margin(0.001));
}

TEST_CASE("timecode integer part matches local wall-clock seconds-of-day",
          "[mads_core]") {
  using namespace std::chrono;
  auto now = system_clock::now();
  time_t now_c = system_clock::to_time_t(now);
  tm *lt = localtime(&now_c);
  double expected_seconds_of_day =
      lt->tm_hour * 3600 + lt->tm_min * 60 + lt->tm_sec;

  double tc = Mads::timecode(system_clock::from_time_t(now_c), 25);
  REQUIRE(std::floor(tc) == Catch::Approx(expected_seconds_of_day));
}

// ---------------------------------------------------------------------------
// tv_to_milliseconds() / milliseconds_to_tv() round-trip
// ---------------------------------------------------------------------------

#ifndef _WIN32
TEST_CASE("tv_to_milliseconds converts a timeval to milliseconds",
          "[mads_core]") {
  struct timeval tv;
  tv.tv_sec = 2;
  tv.tv_usec = 500000; // 0.5s
  auto ms = Mads::tv_to_milliseconds(tv);
  REQUIRE(ms.count() == 2500);
}

TEST_CASE("milliseconds_to_tv converts milliseconds to a timeval",
          "[mads_core]") {
  struct timeval tv{};
  Mads::milliseconds_to_tv(std::chrono::milliseconds(2500), tv);
  REQUIRE(tv.tv_sec == 2);
  REQUIRE(tv.tv_usec == 500000);
}

TEST_CASE("tv_to_milliseconds / milliseconds_to_tv round-trip",
          "[mads_core]") {
  std::vector<long long> values_ms = {0, 1, 999, 1000, 1001, 123456, 7};
  for (auto v : values_ms) {
    struct timeval tv{};
    Mads::milliseconds_to_tv(std::chrono::milliseconds(v), tv);
    auto back = Mads::tv_to_milliseconds(tv);
    REQUIRE(back.count() == v);
  }
}
#endif

// ---------------------------------------------------------------------------
// AgentError
// ---------------------------------------------------------------------------

TEST_CASE("AgentError carries and reports its message", "[mads_core]") {
  Mads::AgentError err("something went wrong");
  REQUIRE(std::string(err.what()) == "something went wrong");

  try {
    throw Mads::AgentError("boom");
  } catch (const std::exception &e) {
    REQUIRE(std::string(e.what()) == "boom");
  }
}
