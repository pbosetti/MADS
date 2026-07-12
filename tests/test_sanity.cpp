#include <catch2/catch_test_macros.hpp>

#include "mads_test_helpers.hpp"

TEST_CASE("Library version is available", "[sanity]") {
  REQUIRE_FALSE(Mads::version().empty());
}

TEST_CASE("check_version accepts the library's own version", "[sanity]") {
  REQUIRE(Mads::check_version(Mads::version()));
}
