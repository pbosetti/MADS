// Unit tests for Mads::topic_match() / Mads::literal_prefix()
// (src/topic_match.hpp, src/topic_match.cpp). Pure, table-driven: no ZMQ, no
// Agent, no sockets.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "topic_match.hpp"

namespace {

struct MatchCase {
  std::string pattern;
  std::string topic;
  bool expected;
};

} // namespace

// ---------------------------------------------------------------------------
// Exact literal matching (no '+'/'#' at all)
// ---------------------------------------------------------------------------

TEST_CASE("topic_match: literal patterns match exactly, not by prefix",
          "[topic_match]") {
  std::vector<MatchCase> cases = {
      {"sensors/acc/x", "sensors/acc/x", true},
      {"sensors/acc/x", "sensors/acc/xx", false},
      {"sensors/acc/x", "sensors/acc", false},
      {"sensors/acc/x", "sensors/acc/x/y", false},
      {"sensors", "sensors", true},
      {"sensors", "sensor", false},
      {"", "", true},
      {"", "sensors", false},
  };
  for (auto const &c : cases) {
    INFO("pattern=\"" << c.pattern << "\" topic=\"" << c.topic << "\"");
    CHECK(Mads::topic_match(c.pattern, c.topic) == c.expected);
  }
}

// ---------------------------------------------------------------------------
// '+' single-level wildcard
// ---------------------------------------------------------------------------

TEST_CASE("topic_match: '+' matches exactly one topic level",
          "[topic_match]") {
  std::vector<MatchCase> cases = {
      {"sensors/+/x", "sensors/acc/x", true},
      {"sensors/+/x", "sensors/gyro/x", true},
      {"sensors/+/x", "sensors/acc/gyro/x", false}, // '+' is exactly one level
      {"sensors/+/x", "sensors/x", false},           // '+' requires a level to exist
      {"sensors/+", "sensors/acc", true},
      {"sensors/+", "sensors", false}, // missing level altogether
      {"+/temp", "sensors/temp", true},
      {"+/temp", "sensors/acc/temp", false},
      {"+", "sensors", true},
      {"+", "sensors/acc", false},
      {"+/+", "sensors/acc", true},
      {"+/+", "sensors", false},
  };
  for (auto const &c : cases) {
    INFO("pattern=\"" << c.pattern << "\" topic=\"" << c.topic << "\"");
    CHECK(Mads::topic_match(c.pattern, c.topic) == c.expected);
  }
}

// ---------------------------------------------------------------------------
// '#' multi-level wildcard, including matching the parent level itself
// ---------------------------------------------------------------------------

TEST_CASE("topic_match: '#' matches this level and everything below it",
          "[topic_match]") {
  std::vector<MatchCase> cases = {
      // MQTT quirk: "sensors/#" also matches the bare parent "sensors".
      {"sensors/#", "sensors", true},
      {"sensors/#", "sensors/acc", true},
      {"sensors/#", "sensors/acc/x", true},
      {"sensors/#", "sensors/acc/x/y/z", true},
      {"sensors/#", "other", false},
      {"sensors/#", "sensorsX", false}, // level-boundary, not a byte prefix
      {"#", "anything", true},
      {"#", "a/b/c", true},
      {"#", "", true},
      {"a/b/#", "a/b", true},
      {"a/b/#", "a", false},
      {"a/b/#", "a/x", false},
  };
  for (auto const &c : cases) {
    INFO("pattern=\"" << c.pattern << "\" topic=\"" << c.topic << "\"");
    CHECK(Mads::topic_match(c.pattern, c.topic) == c.expected);
  }
}

TEST_CASE("topic_match: '#' is rejected unless it is the final token",
          "[topic_match]") {
  std::vector<MatchCase> cases = {
      // '#' mid-pattern is invalid: never matches anything, regardless of topic.
      {"sensors/#/x", "sensors/foo/x", false},
      {"sensors/#/x", "sensors/#/x", false},
      {"#/sensors", "a/sensors", false},
      {"a/#/b/#", "a/x/b/y", false},
      // Sanity: '#' as the sole or final token remains legal.
      {"#", "sensors/acc", true},
      {"sensors/#", "sensors/acc", true},
  };
  for (auto const &c : cases) {
    INFO("pattern=\"" << c.pattern << "\" topic=\"" << c.topic << "\"");
    CHECK(Mads::topic_match(c.pattern, c.topic) == c.expected);
  }
}

// ---------------------------------------------------------------------------
// literal_prefix()
// ---------------------------------------------------------------------------

TEST_CASE("literal_prefix: plain literals return the whole string unchanged",
          "[topic_match]") {
  CHECK(Mads::literal_prefix("sensors") == "sensors");
  CHECK(Mads::literal_prefix("sensors/acc/x") == "sensors/acc/x");
  CHECK(Mads::literal_prefix("") == "");
}

TEST_CASE("literal_prefix: patterns starting with a wildcard have an empty "
          "prefix",
          "[topic_match]") {
  CHECK(Mads::literal_prefix("+") == "");
  CHECK(Mads::literal_prefix("#") == "");
  CHECK(Mads::literal_prefix("+/temp") == "");
  CHECK(Mads::literal_prefix("+/+/x") == "");
}

TEST_CASE("literal_prefix: stops at the first wildcard token, at a level "
          "boundary",
          "[topic_match]") {
  // '+' always requires one more level to follow, so the prefix keeps a
  // trailing '/' -- tighter, and still never excludes a real match.
  CHECK(Mads::literal_prefix("sensors/+") == "sensors/");
  CHECK(Mads::literal_prefix("sensors/+/x") == "sensors/");
  CHECK(Mads::literal_prefix("a/b/+/d") == "a/b/");
  // '#' also matches the literal path built so far with NO further level
  // required (MQTT's "matches the parent topic itself" rule), so NO
  // trailing '/' is appended -- "sensors/" would be a byte-prefix that
  // excludes the bare topic "sensors" itself at the ZMQ layer.
  CHECK(Mads::literal_prefix("sensors/#") == "sensors");
  CHECK(Mads::literal_prefix("a/b/c/#") == "a/b/c");
  // The first wildcard token wins even if invalid grammar (mid-pattern '#')
  // follows further along the pattern.
  CHECK(Mads::literal_prefix("sensors/#/x") == "sensors");
}

TEST_CASE("literal_prefix: the '#'-parent-level match stays reachable "
          "through the ZMQ-level prefix (regression: no trailing separator "
          "before a terminal '#')",
          "[topic_match]") {
  // The ZMQ SUBSCRIBE frame issued for a wildcard pattern is
  // literal_prefix(pattern); it must be a byte-prefix of every topic that
  // topic_match(pattern, topic) accepts, or a real match is silently
  // dropped by ZMQ before topic_match() ever runs. This directly exercises
  // that invariant for the "'#' also matches the parent topic itself" case,
  // which previously slipped through: literal_prefix("sensors/#") used to
  // return "sensors/", a byte-prefix that "sensors" (7 chars) can never
  // start with, so the exact-parent-topic match was dropped at the ZMQ
  // layer even though topic_match() itself correctly accepted it.
  struct Case {
    std::string pattern;
    std::string topic;
  };
  std::vector<Case> cases = {
      {"sensors/#", "sensors"},
      {"a/b/c/#", "a/b/c"},
      {"#", ""},
  };
  for (auto const &c : cases) {
    INFO("pattern=\"" << c.pattern << "\" topic=\"" << c.topic << "\"");
    std::string prefix = Mads::literal_prefix(c.pattern);
    CHECK(c.topic.compare(0, prefix.size(), prefix) == 0);
    CHECK(Mads::topic_match(c.pattern, c.topic));
  }
}
