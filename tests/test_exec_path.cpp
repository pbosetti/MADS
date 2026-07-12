#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "exec_path.hpp"

namespace fs = std::filesystem;

TEST_CASE("exec_path returns an existing file named after the test binary",
          "[exec_path]") {
  fs::path p = Mads::exec_path();
  REQUIRE(fs::exists(p));
  REQUIRE(p.is_absolute());
  REQUIRE(p.filename() == "test_exec_path");
}

TEST_CASE("exec_dir with no argument returns the binary's parent directory",
          "[exec_path]") {
  std::string dir = Mads::exec_dir();
  fs::path expected = Mads::exec_path().parent_path();
  REQUIRE(fs::path(dir) == fs::weakly_canonical(expected));
  REQUIRE(fs::is_directory(dir));
}

TEST_CASE("exec_dir with a relative subpath composes under the binary's directory",
          "[exec_path]") {
  std::string dir = Mads::exec_dir();
  std::string composed = Mads::exec_dir("subdir/file.txt");
  fs::path expected = fs::weakly_canonical(fs::path(dir) / "subdir/file.txt");
  REQUIRE(fs::path(composed) == expected);
}

TEST_CASE("prefix returns the parent of the binary's parent directory",
          "[exec_path]") {
  fs::path expected =
      fs::weakly_canonical(fs::path(Mads::exec_dir()) / "..");
  fs::path actual = Mads::prefix();
  REQUIRE(actual == expected);
}
