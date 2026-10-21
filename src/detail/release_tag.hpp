/*
Internal helper: parsing and ordering of release tags (vX.Y.Z and
vX.Y.Z-betaN), and the selection of the beta releases of the next version used
by `mads beta`. Header-only and free of I/O so that it can be unit-tested. Not
part of the installed SDK.

Ordering: by X.Y.Z, then X.Y.Z-betaN < X.Y.Z, and betaN < beta(N+1).
Development tags such as v2.5.1-p3-gabc1234 are not release tags: parse()
rejects them.
*/
#pragma once

#include <algorithm>
#include <compare>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Mads::detail {

struct ReleaseTag {
  int major = 0;
  int minor = 0;
  int patch = 0;
  std::optional<int> beta; // nullopt for a final release

  bool is_beta() const { return beta.has_value(); }

  // Same X.Y.Z, ignoring the beta number.
  bool same_version(const ReleaseTag &o) const {
    return major == o.major && minor == o.minor && patch == o.patch;
  }

  std::strong_ordering operator<=>(const ReleaseTag &o) const {
    if (auto c = major <=> o.major; c != 0) return c;
    if (auto c = minor <=> o.minor; c != 0) return c;
    if (auto c = patch <=> o.patch; c != 0) return c;
    if (beta.has_value() != o.beta.has_value())
      return beta.has_value() ? std::strong_ordering::less
                              : std::strong_ordering::greater;
    if (beta && o.beta) return *beta <=> *o.beta;
    return std::strong_ordering::equal;
  }
  bool operator==(const ReleaseTag &o) const {
    return (*this <=> o) == std::strong_ordering::equal;
  }
};

// Parses "vX.Y.Z" or "vX.Y.Z-betaN" (N >= 1). Anything else, including
// "v2.5.1-p3-gabc1234" and "v2.5.1-beta0", yields nullopt.
inline std::optional<ReleaseTag> parse_release_tag(const std::string &tag) {
  static const std::regex re(R"(^v(\d+)\.(\d+)\.(\d+)(?:-beta([1-9]\d*))?$)");
  std::smatch m;
  if (!std::regex_match(tag, m, re)) return std::nullopt;
  try {
    ReleaseTag t;
    t.major = std::stoi(m[1]);
    t.minor = std::stoi(m[2]);
    t.patch = std::stoi(m[3]);
    if (m[4].matched) t.beta = std::stoi(m[4]);
    return t;
  } catch (const std::exception &) { // out of range
    return std::nullopt;
  }
}

// From the JSON array returned by GitHub's /releases, selects the non-draft
// pre-releases tagged vX.Y.Z-betaN whose X.Y.Z is strictly greater than that of
// `latest_stable_tag`, sorted newest first (by version, then beta number).
// Returns the release objects. An unparsable `latest_stable_tag` or a non-array
// input yields an empty list.
inline std::vector<nlohmann::json>
select_next_betas(const nlohmann::json &releases,
                  const std::string &latest_stable_tag) {
  std::vector<std::pair<ReleaseTag, nlohmann::json>> found;
  auto stable = parse_release_tag(latest_stable_tag);
  if (!stable || !releases.is_array()) return {};
  for (auto const &rel : releases) {
    if (!rel.is_object() || rel.value("draft", false) ||
        !rel.value("prerelease", false))
      continue;
    auto tag = parse_release_tag(rel.value("tag_name", std::string()));
    if (!tag || !tag->is_beta()) continue;
    // Compare X.Y.Z only: strip the beta from both sides.
    ReleaseTag base = *tag;
    base.beta.reset();
    ReleaseTag ref = *stable;
    ref.beta.reset();
    if (!(base > ref)) continue;
    found.emplace_back(*tag, rel);
  }
  std::stable_sort(found.begin(), found.end(),
                   [](auto const &a, auto const &b) { return a.first > b.first; });
  std::vector<nlohmann::json> out;
  out.reserve(found.size());
  for (auto &f : found) out.push_back(std::move(f.second));
  return out;
}

} // namespace Mads::detail
