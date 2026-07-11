#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>

#include "goback.hpp"

TEST_CASE("GoBack with zero lines is a no-op", "[goback]") {
  std::ostringstream os;
  os << Mads::GoBack(0);
  REQUIRE(os.str().empty());
}

TEST_CASE("goback(0, true) produces zero lines and is a no-op", "[goback]") {
  std::ostringstream os;
  os << Mads::goback(0, true);
  REQUIRE(os.str().empty());
}

TEST_CASE("goback(n, false) is disabled and produces no output regardless of n",
          "[goback]") {
  std::ostringstream os;
  os << Mads::goback(5, false);
  REQUIRE(os.str().empty());
}

TEST_CASE("GoBack(1) emits a single cursor-up + erase-line sequence",
          "[goback]") {
  std::ostringstream os;
  os << Mads::GoBack(1);
  // One repetition of "\x1b[1A\x1b[2K" (no embedded '\r' since it's the last
  // line), followed by a trailing '\r'.
  REQUIRE(os.str() == "\x1b[1A\x1b[2K\r");
}

TEST_CASE("GoBack(n) repeats the escape sequence n times with separators",
          "[goback]") {
  std::ostringstream os;
  os << Mads::GoBack(3);
  std::string expected =
      "\x1b[1A\x1b[2K\r"
      "\x1b[1A\x1b[2K\r"
      "\x1b[1A\x1b[2K"
      "\r";
  REQUIRE(os.str() == expected);
}

TEST_CASE("goback(n, true) matches GoBack(n) output", "[goback]") {
  std::ostringstream os1;
  std::ostringstream os2;
  os1 << Mads::GoBack(4);
  os2 << Mads::goback(4, true);
  REQUIRE(os1.str() == os2.str());
}

TEST_CASE("GoBack exposes its configured line count", "[goback]") {
  Mads::GoBack gb(7);
  REQUIRE(gb.lines() == 7);

  Mads::GoBack disabled = Mads::goback(9, false);
  REQUIRE(disabled.lines() == 0);
}

TEST_CASE("GoBack streamed twice accumulates independent output",
          "[goback]") {
  std::ostringstream os;
  os << Mads::GoBack(1) << "middle" << Mads::GoBack(2);
  std::string expected = "\x1b[1A\x1b[2K\r"
                          "middle"
                          "\x1b[1A\x1b[2K\r\x1b[1A\x1b[2K\r";
  REQUIRE(os.str() == expected);
}
