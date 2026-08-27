#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "detail/plugin_cache.hpp"

namespace fs = std::filesystem;
using Mads::detail::attachment_cache_root;
using Mads::detail::digest_hex;
using Mads::detail::store_attachment;

namespace {

// Every case works under its own agent name so the suite can run in any order
// (and concurrently with anything else on the host) without cases stepping on
// each other's cache directories.
std::string unique_name(const std::string &tag) {
  static std::atomic<int> counter{0};
  return "tpc_" + tag + "_" + std::to_string(counter.fetch_add(1));
}

// Removes an agent's whole cache subtree, so a case starts from nothing and
// leaves nothing behind.
struct CacheGuard {
  explicit CacheGuard(std::string agent) : name(std::move(agent)) { clear(); }
  ~CacheGuard() { clear(); }

  void clear() const {
    std::error_code ec;
    fs::remove_all(attachment_cache_root() / name, ec);
    fs::remove(attachment_cache_root() / (name + ".plugin"), ec);
    fs::remove(attachment_cache_root() / (name + ".bin"), ec);
  }

  std::string name;
};

std::string read_file(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

// Number of digest directories currently cached for an agent.
size_t digest_dir_count(const std::string &name) {
  std::error_code ec;
  size_t n = 0;
  for (fs::directory_iterator it(attachment_cache_root() / name, ec), end;
       !ec && it != end; it.increment(ec)) {
    if (it->is_directory())
      ++n;
  }
  return n;
}

} // namespace

TEST_CASE("digest_hex is 16 hex chars, content-dependent and stable",
          "[plugin_cache]") {
  const std::string d = digest_hex("BINARY-PLUGIN-DATA");
  REQUIRE(d.size() == 16);
  REQUIRE(d.find_first_not_of("0123456789abcdef") == std::string::npos);
  REQUIRE(d == digest_hex("BINARY-PLUGIN-DATA"));
  REQUIRE(d != digest_hex("BINARY-PLUGIN-DATB"));
  // Empty input still yields a full-width digest (leading zeros preserved).
  REQUIRE(digest_hex("").size() == 16);
}

TEST_CASE("store_attachment writes the bytes under a digest directory",
          "[plugin_cache]") {
  CacheGuard guard(unique_name("write"));
  const std::string bytes = "BINARY-PLUGIN-DATA-1";

  const fs::path p = store_attachment(guard.name, "plugin", bytes);

  REQUIRE(fs::exists(p));
  REQUIRE(read_file(p) == bytes);
  REQUIRE(p.parent_path().filename() == digest_hex(bytes));
  REQUIRE(p.parent_path().parent_path().filename() == guard.name);
}

// The regression guard for driver-name resolution: plugin_loader.cpp and
// worker.cpp both fall back to the plugin file's stem for the driver name when
// the settings section has no `driver` key, so the digest must never leak into
// the stem.
TEST_CASE("store_attachment keeps the file stem equal to the agent name",
          "[plugin_cache]") {
  CacheGuard guard(unique_name("stem"));

  const fs::path p = store_attachment(guard.name, "plugin", "PAYLOAD");

  REQUIRE(p.stem().string() == guard.name);
  REQUIRE(p.extension() == ".plugin");
}

TEST_CASE("store_attachment honours a custom extension", "[plugin_cache]") {
  CacheGuard guard(unique_name("ext"));

  const fs::path p = store_attachment(guard.name, "bin", "PAYLOAD");

  REQUIRE(p.extension() == ".bin");
  REQUIRE(p.stem().string() == guard.name);
}

TEST_CASE("storing identical bytes twice reuses the file without rewriting it",
          "[plugin_cache]") {
  CacheGuard guard(unique_name("reuse"));
  const std::string bytes = "BINARY-PLUGIN-DATA-2";

  const fs::path first = store_attachment(guard.name, "plugin", bytes);
  const auto stamp = fs::last_write_time(first);

  // This is what every instance after the first does when an agent is scaled:
  // it must not touch the inode a sibling may already have dlopen()'d.
  const fs::path second = store_attachment(guard.name, "plugin", bytes);

  REQUIRE(second == first);
  // Extra parens: Catch2 cannot stringify file_time_type on libc++ (its rep is
  // __int128), so decomposing this expression fails to compile.
  REQUIRE((fs::last_write_time(second) == stamp));
  REQUIRE(read_file(second) == bytes);
  REQUIRE(digest_dir_count(guard.name) == 1);
}

TEST_CASE("changed bytes land on a new path and the old version is swept",
          "[plugin_cache]") {
  CacheGuard guard(unique_name("update"));

  const fs::path old_path = store_attachment(guard.name, "plugin", "VERSION-1");
  const fs::path old_dir = old_path.parent_path();
  REQUIRE(fs::exists(old_path));

  const fs::path new_path = store_attachment(guard.name, "plugin", "VERSION-2");

  REQUIRE(new_path != old_path);
  REQUIRE(read_file(new_path) == "VERSION-2");
  REQUIRE_FALSE(fs::exists(old_dir));
  REQUIRE(digest_dir_count(guard.name) == 1);
}

TEST_CASE("the sweep never touches another agent's cache", "[plugin_cache]") {
  CacheGuard a(unique_name("sweep_a"));
  CacheGuard b(unique_name("sweep_b"));

  const fs::path a_path = store_attachment(a.name, "plugin", "AGENT-A-DATA");
  // Two stores for B, so B's own sweep definitely runs.
  store_attachment(b.name, "plugin", "AGENT-B-V1");
  const fs::path b_path = store_attachment(b.name, "plugin", "AGENT-B-V2");

  REQUIRE(fs::exists(a_path));
  REQUIRE(read_file(a_path) == "AGENT-A-DATA");
  REQUIRE(fs::exists(b_path));
  REQUIRE(digest_dir_count(a.name) == 1);
  REQUIRE(digest_dir_count(b.name) == 1);
}

TEST_CASE("a pre-2.4.3 flat cache file is cleaned up", "[plugin_cache]") {
  CacheGuard guard(unique_name("legacy"));

  // What MADS <= 2.4.2 left behind: one flat file per agent under the root.
  std::error_code ec;
  fs::create_directories(attachment_cache_root(), ec);
  const fs::path legacy = attachment_cache_root() / (guard.name + ".plugin");
  {
    std::ofstream out(legacy, std::ios::binary);
    out << "STALE";
  }
  REQUIRE(fs::exists(legacy));

  store_attachment(guard.name, "plugin", "FRESH");

  REQUIRE_FALSE(fs::exists(legacy));
}

TEST_CASE("concurrent stores of the same bytes agree on one intact file",
          "[plugin_cache]") {
  CacheGuard guard(unique_name("concurrent"));
  // Large enough that a torn write would be visible rather than atomic by luck.
  const std::string bytes(512 * 1024, 'Z');

  constexpr int THREADS = 8;
  std::vector<fs::path> results(THREADS);
  std::vector<std::string> errors(THREADS);
  std::vector<std::thread> workers;
  workers.reserve(THREADS);

  for (int i = 0; i < THREADS; ++i) {
    workers.emplace_back([&, i] {
      try {
        results[i] = store_attachment(guard.name, "plugin", bytes);
      } catch (const std::exception &e) {
        errors[i] = e.what();
      }
    });
  }
  for (auto &w : workers)
    w.join();

  std::set<std::string> distinct;
  for (int i = 0; i < THREADS; ++i) {
    REQUIRE(errors[i].empty());
    distinct.insert(results[i].string());
  }
  REQUIRE(distinct.size() == 1);
  REQUIRE(read_file(results[0]) == bytes);
  // No staging file survived.
  REQUIRE(digest_dir_count(guard.name) == 1);
  size_t files = 0;
  std::error_code ec;
  for (fs::directory_iterator it(results[0].parent_path(), ec), end;
       !ec && it != end; it.increment(ec)) {
    ++files;
  }
  REQUIRE(files == 1);
}
