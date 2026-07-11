#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "mads_test_helpers.hpp"
#include "watcher.hpp"

namespace fs = std::filesystem;

namespace {

// Returns a fresh, non-colliding path under the system temp directory. Each
// call bumps a static counter so tests run in the same process never clash.
fs::path unique_temp_file(const std::string &tag) {
  static std::atomic<int> counter{0};
  auto n = counter.fetch_add(1);
  auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  return fs::temp_directory_path() /
         ("mads_watcher_" + tag + "_" + std::to_string(stamp) + "_" +
          std::to_string(n) + ".txt");
}

} // namespace

TEST_CASE("Watcher can be constructed and destroyed without watching",
          "[watcher]") {
  auto file_path = unique_temp_file("ctor");
  {
    std::ofstream ofs(file_path);
    ofs << "initial\n";
  }

  {
    Mads::Watcher watcher(file_path.string());
    // Constructor/destructor only: no watch() call, so this must not hang
    // and must not throw.
  }

  fs::remove(file_path);
  SUCCEED("constructed and destroyed cleanly");
}

// NOTE: Watcher::watch() runs an unconditional `while (true)` loop with no
// stop/cancel primitive in this header. To drive it at all we must run it on
// a background thread and detach it (the process exits shortly after the
// test binary finishes, which reclaims the thread). Because the thread
// outlives the TEST_CASE scope, the Watcher and callback state below are
// deliberately heap-allocated and intentionally never freed: destroying them
// while the detached thread might still be reading from them would be a
// use-after-free. This is a test-only leak, acceptable for a short-lived
// test binary.

TEST_CASE("Watcher invokes the callback when the watched file is modified",
          "[watcher]") {
  auto file_path = unique_temp_file("fire");
  {
    std::ofstream ofs(file_path);
    ofs << "initial\n";
  }

  auto *watcher = new Mads::Watcher(file_path.string());
  auto *called = new std::atomic<bool>(false);
  auto *received = new std::string();

  std::thread t([watcher, called, received]() {
    watcher->watch([called, received](const std::string &fn) {
      *received = fn;
      called->store(true);
    });
  });
  t.detach();

  // Let the watch thread start and the OS-level watch (inotify/kqueue/etc.)
  // be armed before we mutate the file.
  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  {
    std::ofstream ofs(file_path, std::ios::app);
    ofs << "modified\n";
    ofs.flush();
  }

  bool fired = mads_test::wait_for([called]() { return called->load(); },
                                    std::chrono::milliseconds(2000));
  REQUIRE(fired);
  REQUIRE(*received == file_path.string());

  fs::remove(file_path);
}

TEST_CASE("Watcher does not fire when the file is left untouched",
          "[watcher]") {
  auto file_path = unique_temp_file("silent");
  {
    std::ofstream ofs(file_path);
    ofs << "initial\n";
  }

  auto *watcher = new Mads::Watcher(file_path.string());
  auto *called = new std::atomic<bool>(false);

  std::thread t([watcher, called]() {
    watcher->watch(
        [called](const std::string &) { called->store(true); });
  });
  t.detach();

  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  // No modification: the callback must not fire within a generous window.
  bool fired = mads_test::wait_for([called]() { return called->load(); },
                                    std::chrono::milliseconds(600));
  REQUIRE_FALSE(fired);

  fs::remove(file_path);
}
