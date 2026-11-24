/*
 mads-package's release selection: the NAME[@TAG] package spec, and which
 GitHub releases are listed, installed by default, or matched by a tag.

 Header-only so it is directly unit-testable (tests/test_package_releases.cpp)
 without linking src/main/package.cpp's main(); mirrors the precedent of
 src/main/play_pacing.hpp.

 Author(s): Paolo Bosetti
*/
#ifndef MADS_MAIN_PACKAGE_RELEASES_HPP
#define MADS_MAIN_PACKAGE_RELEASES_HPP

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Mads {
namespace Package {

struct Spec {
  std::string name;
  std::string tag; // empty: the latest release
};

// "NAME" or "NAME@TAG". Split at the last '@'; the tag is matched verbatim.
inline Spec parse_spec(const std::string &spec) {
  auto at = spec.rfind('@');
  if (at == std::string::npos)
    return {spec, ""};
  Spec s{spec.substr(0, at), spec.substr(at + 1)};
  if (s.name.empty() || s.tag.empty())
    throw std::invalid_argument("Invalid package spec '" + spec +
                                "': expected NAME or NAME@TAG");
  return s;
}

inline std::string release_timestamp(const nlohmann::json &release) {
  for (const char *key : {"published_at", "created_at"})
    if (release.contains(key) && release[key].is_string() &&
        !release[key].get<std::string>().empty())
      return release[key].get<std::string>();
  return "";
}

inline bool is_published(const nlohmann::json &release) {
  return release.is_object() && !release.value("draft", false) &&
         !release.value("prerelease", false);
}

// Published releases (no drafts, no pre-releases), newest first. GitHub
// timestamps are ISO 8601 UTC, so they compare as strings.
inline nlohmann::json published_releases(const nlohmann::json &releases) {
  std::vector<nlohmann::json> out;
  if (releases.is_array())
    for (const auto &r : releases)
      if (is_published(r))
        out.push_back(r);
  std::stable_sort(out.begin(), out.end(), [](const auto &a, const auto &b) {
    return release_timestamp(a) > release_timestamp(b);
  });
  return nlohmann::json(out);
}

// The release installed by default: the newest published one, else the
// newest non-draft pre-release (prerelease_fallback = true). null if none.
inline nlohmann::json latest_release(const nlohmann::json &releases,
                                     bool &prerelease_fallback) {
  prerelease_fallback = false;
  nlohmann::json published = published_releases(releases);
  if (!published.empty())
    return published.front();
  nlohmann::json best;
  if (releases.is_array())
    for (const auto &r : releases)
      if (r.is_object() && !r.value("draft", false) &&
          (best.is_null() || release_timestamp(r) > release_timestamp(best)))
        best = r;
  prerelease_fallback = !best.is_null();
  return best;
}

// The non-draft release tagged exactly `tag`, or null.
inline nlohmann::json release_with_tag(const nlohmann::json &releases,
                                       const std::string &tag) {
  if (releases.is_array())
    for (const auto &r : releases)
      if (r.is_object() && !r.value("draft", false) &&
          r.value("tag_name", "") == tag)
        return r;
  return nullptr;
}

inline std::string join(const std::vector<std::string> &items,
                        const std::string &sep = ", ") {
  std::string out;
  for (const auto &item : items)
    out += (out.empty() ? "" : sep) + item;
  return out;
}

} // namespace Package
} // namespace Mads

#endif // MADS_MAIN_PACKAGE_RELEASES_HPP
