// Unit tests for the OS shared-memory segment wrapper and its orphan registry
// (src/shm/shared_segment.hpp).
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include "shm/shared_segment.hpp"

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace Mads::shm;
namespace fs = std::filesystem;

TEST_CASE("make_segment_name fits macOS PSHMNAMLEN and is unique",
          "[shm][segment]") {
  auto a = make_segment_name();
  auto b = make_segment_name();
  CHECK(a != b);
  CHECK(a.size() <= 31);
  CHECK(a.rfind("/mads_", 0) == 0);
  CHECK(a.find('/', 1) == std::string::npos);
}

TEST_CASE("SharedSegment create/open share data", "[shm][segment]") {
  auto name = make_segment_name();
  auto seg = SharedSegment::create(name, 10000);
  REQUIRE(seg);
  CHECK(seg->name() == name);
  CHECK(seg->size() == 10000);
  auto *p = static_cast<unsigned char *>(seg->data());
  CHECK(p[0] == 0);
  CHECK(p[9999] == 0);
  std::memcpy(p, "hello", 6);

  auto other = SharedSegment::open(name);
  REQUIRE(other);
  CHECK(other->size() >= 10000); // may be rounded up to a page
  CHECK(other->data() != seg->data());
  CHECK(std::strcmp(static_cast<char *>(other->data()), "hello") == 0);

  // Writes are visible both ways.
  static_cast<char *>(other->data())[0] = 'J';
  CHECK(p[0] == 'J');

  // A second create with the same name collides.
  CHECK_THROWS_AS(SharedSegment::create(name, 100), std::system_error);
  seg->unlink();
}

TEST_CASE("SharedSegment rejects bad arguments", "[shm][segment]") {
  CHECK_THROWS_AS(SharedSegment::create("noslash", 10), std::invalid_argument);
  CHECK_THROWS_AS(SharedSegment::create("/a/b", 10), std::invalid_argument);
  CHECK_THROWS_AS(SharedSegment::create("/" + std::string(31, 'x'), 10),
                  std::invalid_argument);
  CHECK_THROWS_AS(SharedSegment::create(make_segment_name(), 0),
                  std::invalid_argument);
  CHECK_THROWS(SharedSegment::open(make_segment_name())); // never created
}

TEST_CASE("SharedSegment cannot be opened after unlink", "[shm][segment]") {
  auto name = make_segment_name();
  {
    auto seg = SharedSegment::create(name, 4096);
    std::memcpy(seg->data(), "x", 2);
    seg->unlink();
    seg->unlink(); // idempotent
    // The creator's mapping stays usable after unlink.
    CHECK(static_cast<char *>(seg->data())[0] == 'x');
  }
  // POSIX: name removed by unlink. Windows: section freed with last handle.
  CHECK_THROWS_AS(SharedSegment::open(name), std::system_error);
}

#ifndef _WIN32

TEST_CASE("SharedSegment registry file tracks the owner", "[shm][segment]") {
  auto name = make_segment_name();
  auto seg = SharedSegment::create(name, 4096);
  auto reg = registry_file(name);
  CHECK(reg.filename().string() == name.substr(1));
  REQUIRE(fs::exists(reg));
  long long pid = 0;
  {
    std::ifstream f(reg);
    f >> pid;
  }
  CHECK(pid == static_cast<long long>(::getpid()));

  // A live owner is never swept.
  sweep_orphans();
  CHECK(fs::exists(reg));
  CHECK_NOTHROW(SharedSegment::open(name));

  seg->unlink();
  CHECK_FALSE(fs::exists(reg));
}

TEST_CASE("sweep_orphans unlinks segments of dead owners", "[shm][segment]") {
  // A pid that is certainly dead: a child we already reaped.
  pid_t child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0)
    ::_exit(0);
  int status = 0;
  REQUIRE(::waitpid(child, &status, 0) == child);

  auto name = make_segment_name();
  {
    auto seg = SharedSegment::create(name, 4096);
    // Pretend the dead child created it and never unlinked it.
    std::ofstream(registry_file(name), std::ios::trunc) << child << "\n";
  } // destructor unmaps but does not unlink

  REQUIRE(fs::exists(registry_file(name)));
  REQUIRE_NOTHROW(SharedSegment::open(name));

  // Another test process may sweep concurrently, so check the outcome rather
  // than this call's count.
  sweep_orphans();
  CHECK_FALSE(fs::exists(registry_file(name)));
  CHECK_THROWS_AS(SharedSegment::open(name), std::system_error);
}

TEST_CASE("sweep_orphans leaves unparseable entries alone", "[shm][segment]") {
  auto reg = registry_file("/mads_testgarbage" + std::to_string(::getpid()));
  fs::create_directories(reg.parent_path());
  std::ofstream(reg) << "not a pid\n";
  sweep_orphans();
  CHECK(fs::exists(reg));
  fs::remove(reg);
}

#endif
