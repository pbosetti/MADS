#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "detail/config_dir.hpp"

namespace fs = std::filesystem;
using Mads::detail::config_dir;

namespace {

// Sets (or, when `value` is nullopt, clears) an environment variable. The
// previous value is restored when the guard goes out of scope.
class ScopedEnv {
public:
  ScopedEnv(char const *name, std::optional<std::string> value)
      : _name(name) {
    if (const char *old = std::getenv(name))
      _old = std::string(old);
    set(std::move(value));
  }
  ~ScopedEnv() { set(_old); }
  ScopedEnv(ScopedEnv const &) = delete;
  ScopedEnv &operator=(ScopedEnv const &) = delete;

private:
  void set(std::optional<std::string> const &value) {
#ifdef _WIN32
    // _putenv_s with an empty value removes the variable.
    _putenv_s(_name.c_str(), value ? value->c_str() : "");
#else
    if (value)
      ::setenv(_name.c_str(), value->c_str(), 1);
    else
      ::unsetenv(_name.c_str());
#endif
  }

  std::string _name;
  std::optional<std::string> _old;
};

// Returns a fresh, non-existing directory under the system temp directory.
fs::path unique_temp_dir(const std::string &tag) {
  static std::atomic<int> counter{0};
  auto n = counter.fetch_add(1);
  auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  return fs::temp_directory_path() /
         ("mads_config_dir_" + tag + "_" + std::to_string(stamp) + "_" +
          std::to_string(n));
}

} // namespace

TEST_CASE("config_dir: MADS_CONFIG_DIR overrides the default location",
          "[config_dir]") {
  auto override_dir = unique_temp_dir("override");
  ScopedEnv env("MADS_CONFIG_DIR", override_dir.string());

  fs::path result = config_dir();
  CHECK(result == override_dir);
  CHECK(fs::is_directory(result));

#ifndef _WIN32
  struct stat st {};
  REQUIRE(::stat(result.c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0700);
#endif

  std::error_code ec;
  fs::remove_all(override_dir, ec);
}

TEST_CASE("config_dir: returns the same path on repeated calls",
          "[config_dir]") {
  auto override_dir = unique_temp_dir("repeat");
  ScopedEnv env("MADS_CONFIG_DIR", override_dir.string());

  fs::path first = config_dir();
  fs::path second = config_dir();
  CHECK(first == second);

  std::error_code ec;
  fs::remove_all(override_dir, ec);
}

#ifdef __linux__
TEST_CASE("config_dir: honours XDG_CONFIG_HOME on Linux", "[config_dir]") {
  auto xdg_home = unique_temp_dir("xdg");
  ScopedEnv no_override("MADS_CONFIG_DIR", std::nullopt);
  ScopedEnv xdg("XDG_CONFIG_HOME", xdg_home.string());

  fs::path result = config_dir();
  CHECK(result == xdg_home / "mads");
  CHECK(fs::is_directory(result));

  std::error_code ec;
  fs::remove_all(xdg_home, ec);
}
#endif
