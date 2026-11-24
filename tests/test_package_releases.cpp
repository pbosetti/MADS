// Unit tests for mads-package's release selection
// (src/main/package_releases.hpp).
#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

#include "main/package_releases.hpp"

using namespace Mads::Package;
using json = nlohmann::json;

namespace {
json rel(const std::string &tag, const std::string &published,
         bool prerelease = false, bool draft = false) {
  return {{"tag_name", tag},
          {"published_at", published},
          {"prerelease", prerelease},
          {"draft", draft}};
}

std::vector<std::string> tags_of(const json &releases) {
  std::vector<std::string> out;
  for (const auto &r : releases)
    out.push_back(r["tag_name"].get<std::string>());
  return out;
}
} // namespace

TEST_CASE("parse_spec splits NAME@TAG", "[package_releases]") {
  CHECK(parse_spec("mongodb.plugin").name == "mongodb.plugin");
  CHECK(parse_spec("mongodb.plugin").tag.empty());
  auto s = parse_spec("mongodb.plugin@v3.0.0");
  CHECK(s.name == "mongodb.plugin");
  CHECK(s.tag == "v3.0.0");
  // Split at the last '@'; the tag is taken verbatim, no "v" is added.
  CHECK(parse_spec("a@b@3.0").name == "a@b");
  CHECK(parse_spec("a@b@3.0").tag == "3.0");
  CHECK_THROWS_AS(parse_spec("mongodb.plugin@"), std::invalid_argument);
  CHECK_THROWS_AS(parse_spec("@v3.0.0"), std::invalid_argument);
}

TEST_CASE("published_releases drops drafts and pre-releases, newest first",
          "[package_releases]") {
  json releases = json::array({
      rel("v2.0.0", "2026-03-01T10:00:00Z"),
      rel("ci-abc", "2026-11-20T10:00:00Z", true),
      rel("v3.0.0", "2026-11-01T10:00:00Z"),
      rel("v3.1.0", "2026-11-23T10:00:00Z", false, true),
      rel("v1.0.0", "2025-01-01T10:00:00Z"),
  });
  CHECK(tags_of(published_releases(releases)) ==
        std::vector<std::string>{"v3.0.0", "v2.0.0", "v1.0.0"});
  CHECK(published_releases(json::object()).empty());
}

TEST_CASE("published_releases orders by date, not by tag",
          "[package_releases]") {
  // A backport released after the next major is listed first.
  json releases = json::array({rel("v3.0.0", "2026-11-01T10:00:00Z"),
                               rel("v2.9.1", "2026-11-10T10:00:00Z")});
  CHECK(tags_of(published_releases(releases)) ==
        std::vector<std::string>{"v2.9.1", "v3.0.0"});
}

TEST_CASE("release_timestamp falls back to created_at", "[package_releases]") {
  CHECK(release_timestamp({{"published_at", nullptr},
                           {"created_at", "2026-01-01T00:00:00Z"}}) ==
        "2026-01-01T00:00:00Z");
  CHECK(release_timestamp(json::object()).empty());
}

TEST_CASE("latest_release prefers published, then the newest pre-release",
          "[package_releases]") {
  bool fallback = true;
  json mixed = json::array({rel("v1.0.0", "2026-01-01T00:00:00Z"),
                            rel("ci-new", "2026-11-01T00:00:00Z", true)});
  CHECK(latest_release(mixed, fallback)["tag_name"] == "v1.0.0");
  CHECK_FALSE(fallback);

  json pre_only = json::array({rel("ci-old", "2026-01-01T00:00:00Z", true),
                               rel("ci-new", "2026-11-01T00:00:00Z", true),
                               rel("draft", "2026-12-01T00:00:00Z", true, true)});
  CHECK(latest_release(pre_only, fallback)["tag_name"] == "ci-new");
  CHECK(fallback);

  CHECK(latest_release(json::array(), fallback).is_null());
  CHECK_FALSE(fallback);
}

TEST_CASE("release_with_tag matches exactly, pre-releases included",
          "[package_releases]") {
  json releases = json::array({rel("v3.0.0", "2026-11-01T00:00:00Z"),
                               rel("ci-abc", "2026-11-20T00:00:00Z", true),
                               rel("v4.0.0", "2026-12-01T00:00:00Z", false,
                                   true)});
  CHECK(release_with_tag(releases, "v3.0.0")["tag_name"] == "v3.0.0");
  CHECK(release_with_tag(releases, "ci-abc")["tag_name"] == "ci-abc");
  CHECK(release_with_tag(releases, "3.0.0").is_null());
  CHECK(release_with_tag(releases, "v4.0.0").is_null()); // draft
}

TEST_CASE("join separates with comma and space", "[package_releases]") {
  CHECK(join({}).empty());
  CHECK(join({"v3.0.0"}) == "v3.0.0");
  CHECK(join({"v3.0.0", "v2.0.0", "v1.0.0"}) == "v3.0.0, v2.0.0, v1.0.0");
}
