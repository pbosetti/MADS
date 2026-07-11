// Unit tests for Mads::LazyPayload (src/agent.hpp), the tiny lazily-converting
// text<->json cache used internally by Agent::receive() to avoid needless
// dump()/parse() round trips.
#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "agent.hpp"

using Mads::LazyPayload;

// ---------------------------------------------------------------------------
// from_text(): text() is free, doc() parses lazily on first access
// ---------------------------------------------------------------------------

TEST_CASE("LazyPayload::from_text exposes the original text verbatim",
          "[lazy_payload]") {
  auto p = LazyPayload::from_text(R"({"a":1,"b":"two"})");
  REQUIRE(p.text() == R"({"a":1,"b":"two"})");
}

TEST_CASE("LazyPayload::from_text parses doc() lazily from the cached text",
          "[lazy_payload]") {
  auto p = LazyPayload::from_text(R"({"a":1,"b":"two"})");
  const nlohmann::json &doc = p.doc();
  REQUIRE(doc.at("a") == 1);
  REQUIRE(doc.at("b") == "two");
}

TEST_CASE("LazyPayload::from_text repeated doc() access returns equal, "
          "consistent content",
          "[lazy_payload]") {
  auto p = LazyPayload::from_text(R"({"n":42})");
  const nlohmann::json &doc1 = p.doc();
  const nlohmann::json &doc2 = p.doc();
  // Second access returns the same cached object (same address), proving the
  // parse only happens once.
  REQUIRE(&doc1 == &doc2);
  REQUIRE(doc1.at("n") == 42);
}

// ---------------------------------------------------------------------------
// from_doc(): doc() is free, text() dumps lazily on first access
// ---------------------------------------------------------------------------

TEST_CASE("LazyPayload::from_doc exposes the original object verbatim",
          "[lazy_payload]") {
  nlohmann::json j = {{"x", 1}, {"y", 2}};
  auto p = LazyPayload::from_doc(j);
  REQUIRE(p.doc() == j);
}

TEST_CASE("LazyPayload::from_doc dumps text() lazily from the cached object",
          "[lazy_payload]") {
  nlohmann::json j = {{"x", 1}, {"y", 2}};
  auto p = LazyPayload::from_doc(j);
  std::string text = p.text();
  // Round-trip: re-parsing the dumped text must reproduce the same object.
  REQUIRE(nlohmann::json::parse(text) == j);
}

TEST_CASE("LazyPayload::from_doc repeated text() access returns the same "
          "cached string",
          "[lazy_payload]") {
  nlohmann::json j = {{"k", "v"}};
  auto p = LazyPayload::from_doc(j);
  const std::string &t1 = p.text();
  const std::string &t2 = p.text();
  REQUIRE(&t1 == &t2);
  REQUIRE(t1 == j.dump());
}

// ---------------------------------------------------------------------------
// Default-constructed / empty payload: caching order affects the result,
// since text()<->doc() conversion is only ever driven by whichever cache is
// already populated.
// ---------------------------------------------------------------------------

TEST_CASE("LazyPayload default construction: text() first yields empty text "
          "and a null doc()",
          "[lazy_payload]") {
  LazyPayload p;
  // text() sees no cached doc, so it caches an empty string.
  REQUIRE(p.text().empty());
  // doc() then sees a cached (empty) text and, since it's empty, produces a
  // null json rather than trying to parse an empty string.
  REQUIRE(p.doc().is_null());
}

TEST_CASE("LazyPayload default construction: doc() first yields a null doc "
          "and a \"null\" text",
          "[lazy_payload]") {
  LazyPayload p;
  // doc() sees no cached text, so it caches a default-constructed (null) json.
  REQUIRE(p.doc().is_null());
  // text() then dumps the cached null doc, producing the literal "null".
  REQUIRE(p.text() == "null");
}

TEST_CASE("LazyPayload default construction: text() and doc() are each "
          "idempotent once cached",
          "[lazy_payload]") {
  LazyPayload p;
  REQUIRE(p.text().empty());
  REQUIRE(p.text().empty()); // repeated access, still cached empty string
  REQUIRE(p.doc().is_null());
  REQUIRE(p.doc().is_null());
}

// ---------------------------------------------------------------------------
// Move semantics used by from_text/from_doc (both take by value and move in)
// ---------------------------------------------------------------------------

TEST_CASE("LazyPayload::from_text accepts an rvalue without copying it first",
          "[lazy_payload]") {
  std::string s = "\"hello\"";
  auto p = LazyPayload::from_text(std::move(s));
  REQUIRE(p.text() == "\"hello\"");
  REQUIRE(p.doc() == "hello");
}

TEST_CASE("LazyPayload::from_doc accepts an rvalue without copying it first",
          "[lazy_payload]") {
  nlohmann::json j = {{"arr", {1, 2, 3}}};
  auto p = LazyPayload::from_doc(std::move(j));
  REQUIRE(p.doc().at("arr").size() == 3);
}
