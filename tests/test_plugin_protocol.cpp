// Unit tests for the plugin protocol compatibility check the plugin loaders
// run before instantiating a plugin (src/detail/plugin_protocol.hpp).
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "detail/plugin_protocol.hpp"

using Mads::detail::plugin_protocol_mismatch;

TEST_CASE("a plugin built for the host's protocol is accepted",
          "[plugin_protocol]") {
  CHECK(plugin_protocol_mismatch(9, 9, 9).empty());
  CHECK(plugin_protocol_mismatch(8, 9, 7).empty());
}

TEST_CASE("a plugin built for a newer protocol is rejected",
          "[plugin_protocol]") {
  // The case pugg lets through: a P9 plugin in a P8 host.
  auto why = plugin_protocol_mismatch(9, 8, 8);
  REQUIRE_FALSE(why.empty());
  CHECK(why.find("plugin built for protocol 9") != std::string::npos);
  CHECK(why.find("this MADS supports 8") != std::string::npos);
  CHECK(why.find("upgrade MADS") != std::string::npos);
}

TEST_CASE("a plugin built for an older protocol is rejected",
          "[plugin_protocol]") {
  auto why = plugin_protocol_mismatch(7, 9, 9);
  REQUIRE_FALSE(why.empty());
  CHECK(why.find("plugin built for protocol 7") != std::string::npos);
  CHECK(why.find("rebuild the plugin") != std::string::npos);

  auto ranged = plugin_protocol_mismatch(6, 9, 7);
  CHECK(ranged.find("supports 7 to 9") != std::string::npos);
}
