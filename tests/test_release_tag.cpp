#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "detail/release_tag.hpp"

using Mads::detail::parse_release_tag;
using Mads::detail::ReleaseTag;
using Mads::detail::select_next_betas;
using nlohmann::json;

TEST_CASE("release_tag: parse accepts release and beta tags", "[release_tag]") {
  auto r = parse_release_tag("v2.6.0");
  REQUIRE(r);
  CHECK(r->major == 2);
  CHECK(r->minor == 6);
  CHECK(r->patch == 0);
  CHECK_FALSE(r->beta.has_value());

  auto b = parse_release_tag("v2.6.0-beta12");
  REQUIRE(b);
  CHECK(b->patch == 0);
  REQUIRE(b->beta.has_value());
  CHECK(*b->beta == 12);

  CHECK(parse_release_tag("v10.20.30-beta1")->major == 10);
}

TEST_CASE("release_tag: parse rejects everything else", "[release_tag]") {
  for (auto const *s :
       {"", "2.6.0", "v2.6", "v2.6.0-beta", "v2.6.0-beta0", "v2.6.0-beta1x",
        "v2.6.0-p3-gabc1234", "v2.6.0-beta1-p3-gabc1234", "v2.6.0-rc1",
        "v2.6.0-dirty", " v2.6.0", "v2.6.0 ", "v2.6.0-beta-1",
        "v99999999999.0.0"})
    CHECK_FALSE(parse_release_tag(s).has_value());
}

TEST_CASE("release_tag: ordering", "[release_tag]") {
  auto t = [](char const *s) { return *parse_release_tag(s); };
  CHECK(t("v2.6.0-beta1") < t("v2.6.0"));
  CHECK(t("v2.6.0-beta1") < t("v2.6.0-beta2"));
  CHECK(t("v2.6.0-beta2") < t("v2.6.0-beta12")); // numeric, not lexicographic
  CHECK(t("v2.5.9") < t("v2.6.0-beta1"));
  CHECK(t("v2.6.0") < t("v2.6.1-beta1"));
  CHECK(t("v2.9.0") < t("v2.10.0"));
  CHECK(t("v2.6.0") < t("v3.0.0"));
  CHECK(t("v2.6.0-beta3") == t("v2.6.0-beta3"));
  CHECK(t("v2.6.0") == t("v2.6.0"));
  CHECK(t("v2.6.0") > t("v2.6.0-beta99"));
}

namespace {
json rel(char const *tag, bool prerelease = true, bool draft = false) {
  return {{"tag_name", tag}, {"prerelease", prerelease}, {"draft", draft}};
}
std::vector<std::string> tags(std::vector<json> const &v) {
  std::vector<std::string> out;
  for (auto const &r : v) out.push_back(r["tag_name"]);
  return out;
}
} // namespace

TEST_CASE("release_tag: select_next_betas sorts newest first", "[release_tag]") {
  json releases = json::array({rel("v2.6.0-beta1"), rel("v2.7.0-beta1"),
                               rel("v2.6.0-beta12"), rel("v2.6.0-beta2")});
  auto out = select_next_betas(releases, "v2.5.1");
  CHECK(tags(out) == std::vector<std::string>{"v2.7.0-beta1", "v2.6.0-beta12",
                                              "v2.6.0-beta2", "v2.6.0-beta1"});
}

TEST_CASE("release_tag: select_next_betas filters", "[release_tag]") {
  json releases = json::array({
      rel("v2.6.0-beta3", true, true),   // draft
      rel("v2.5.1-p3-gabc1234"),         // dev pre-release
      rel("v2.5.1-beta1-p3-gabc1234"),   // dev build after a beta
      rel("v2.5.1-beta2"),               // same version as the stable
      rel("v2.5.0-beta1"),               // older
      rel("v2.4.9-beta1"),               // older
      rel("v2.6.0", false),              // final release, not a beta
      rel("v2.6.0-beta1", false),        // beta tag but not a pre-release
      rel("v2.6.0-beta2"),               // the only keeper
      json::object(),                    // junk
      json("junk"),
  });
  auto out = select_next_betas(releases, "v2.5.1");
  CHECK(tags(out) == std::vector<std::string>{"v2.6.0-beta2"});
}

TEST_CASE("release_tag: select_next_betas none and bad input", "[release_tag]") {
  CHECK(select_next_betas(json::array(), "v2.5.1").empty());
  CHECK(select_next_betas(json::array({rel("v2.5.1-beta1")}), "v2.5.1").empty());
  CHECK(select_next_betas(json::object(), "v2.5.1").empty());
  CHECK(select_next_betas(json::array({rel("v2.6.0-beta1")}), "garbage").empty());
}
