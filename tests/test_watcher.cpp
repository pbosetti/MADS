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

TEST_CASE("Watcher invokes the callback when the watched file is modified",
          "[watcher]") {
  auto file_path = unique_temp_file("fire");
  {
    std::ofstream ofs(file_path);
    ofs << "initial\n";
  }

  Mads::Watcher watcher(file_path.string());
  std::atomic<bool> called{false};
  std::string received;

  std::thread t([&]() {
    watcher.watch([&](const std::string &fn) {
      received = fn;
      called = true;
    });
  });

  // Let the watch thread start and the OS-level watch (inotify/kqueue/etc.)
  // be armed before we mutate the file.
  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  {
    std::ofstream ofs(file_path, std::ios::app);
    ofs << "modified\n";
    ofs.flush();
  }

  bool fired = mads_test::wait_for([&]() { return called.load(); },
                                    std::chrono::milliseconds(2000));
  watcher.stop();
  t.join();
  REQUIRE(fired);
  REQUIRE(received == file_path.string());

  fs::remove(file_path);
}

TEST_CASE("Watcher does not fire when the file is left untouched",
          "[watcher]") {
  auto file_path = unique_temp_file("silent");
  {
    std::ofstream ofs(file_path);
    ofs << "initial\n";
  }

  Mads::Watcher watcher(file_path.string());
  std::atomic<bool> called{false};

  std::thread t([&]() {
    watcher.watch([&](const std::string &) { called = true; });
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  // No modification: the callback must not fire within a generous window.
  bool fired = mads_test::wait_for([&]() { return called.load(); },
                                    std::chrono::milliseconds(600));
  watcher.stop();
  t.join();
  REQUIRE_FALSE(fired);

  fs::remove(file_path);
}

TEST_CASE("Watcher::stop() ends watch() promptly and the watcher is reusable",
          "[watcher]") {
  auto file_path = unique_temp_file("stop");
  {
    std::ofstream ofs(file_path);
    ofs << "initial\n";
  }

  Mads::Watcher watcher(file_path.string());

  for (int round = 0; round < 2; ++round) {
    std::atomic<bool> returned{false};
    std::thread t([&]() {
      watcher.watch([](const std::string &) {});
      returned = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    auto t0 = std::chrono::steady_clock::now();
    watcher.stop();
    bool done = mads_test::wait_for([&]() { return returned.load(); },
                                     std::chrono::milliseconds(3000));
    t.join();
    auto elapsed = std::chrono::steady_clock::now() - t0;
    REQUIRE(done);
    REQUIRE(elapsed < std::chrono::milliseconds(3000));
  }

  fs::remove(file_path);
}

TEST_CASE("a process-wide stop request also ends watch()", "[watcher]") {
  mads_test::RunningGuard guard;
  auto file_path = unique_temp_file("procstop");
  {
    std::ofstream ofs(file_path);
    ofs << "initial\n";
  }

  Mads::Watcher watcher(file_path.string());
  std::atomic<bool> returned{false};
  std::thread t([&]() {
    watcher.watch([](const std::string &) {});
    returned = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  Mads::Runtime::stop_process();
  bool done = mads_test::wait_for([&]() { return returned.load(); },
                                   std::chrono::milliseconds(3000));
  t.join();
  REQUIRE(done);

  fs::remove(file_path);
}
