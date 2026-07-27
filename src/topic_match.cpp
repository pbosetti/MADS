#include "topic_match.hpp"

#include <vector>

namespace Mads {

namespace {

// Splits `s` into its '/'-separated topic levels. An empty string yields a
// single empty level (mirrors how MQTT treats "" as one, empty, level).
std::vector<std::string_view> split_levels(std::string_view s) {
  std::vector<std::string_view> levels;
  std::size_t start = 0;
  while (true) {
    std::size_t pos = s.find('/', start);
    if (pos == std::string_view::npos) {
      levels.push_back(s.substr(start));
      break;
    }
    levels.push_back(s.substr(start, pos - start));
    start = pos + 1;
  }
  return levels;
}

} // namespace

bool topic_match(std::string_view pattern, std::string_view topic) {
  auto ptoks = split_levels(pattern);

  // '#' is only legal as the final token; a pattern that breaks this rule
  // is invalid and never matches anything.
  for (std::size_t i = 0; i < ptoks.size(); ++i) {
    if (ptoks[i] == "#" && i + 1 != ptoks.size())
      return false;
  }

  auto ttoks = split_levels(topic);

  std::size_t ti = 0;
  for (std::size_t pi = 0; pi < ptoks.size(); ++pi) {
    if (ptoks[pi] == "#") {
      // Matches this level -- even if the topic has nothing left -- and
      // everything below it.
      return true;
    }
    if (ti >= ttoks.size()) {
      return false; // pattern still expects a level the topic doesn't have
    }
    if (ptoks[pi] == "+") {
      ++ti;
      continue;
    }
    if (ptoks[pi] != ttoks[ti]) {
      return false;
    }
    ++ti;
  }
  // No trailing '#' consumed the rest: the topic must be fully consumed too
  // (exact-match-literal semantics), or this isn't a match.
  return ti == ttoks.size();
}

std::string literal_prefix(std::string_view pattern) {
  auto toks = split_levels(pattern);
  std::string prefix;
  for (std::size_t i = 0; i < toks.size(); ++i) {
    if (toks[i] == "+" || toks[i] == "#") {
      // The ZMQ-level SUBSCRIBE frame must be a byte-prefix of every topic
      // topic_match() would accept -- otherwise ZMQ silently drops a real
      // match before topic_match() ever runs. '+' always requires one more
      // level to follow, so appending '/' is still safe and keeps the
      // subscribe tighter. '#' additionally matches the literal path built
      // so far with NO further level required (MQTT's "also matches the
      // parent topic itself" rule, e.g. "sensors/#" matches "sensors"), so
      // a trailing '/' here would wrongly exclude that exact-parent topic.
      if (toks[i] == "+" && !prefix.empty())
        prefix += '/';
      return prefix;
    }
    if (i > 0)
      prefix += '/';
    prefix += toks[i];
  }
  return prefix; // no wildcard token found: the whole pattern is literal
}

} // namespace Mads
