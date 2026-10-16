// Unit tests for the shared-memory ring and the consumer-side segment cache
// (src/shm/shared_ring.hpp).
#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <cstring>
#include <stdexcept>
#include <string>

#include "detail/host_token.hpp"
#include "shm/shared_ring.hpp"

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace Mads::shm;
using Mads::ShmDescriptor;
using Mads::SharedView;
using Mads::WritableSlot;

namespace {

ShmDescriptor publish(SharedRing &ring, std::string const &text) {
  WritableSlot s = ring.acquire();
  REQUIRE(s);
  REQUIRE(text.size() <= s.capacity);
  std::memcpy(s.data, text.data(), text.size());
  return ring.commit(s, text.size());
}

std::string as_string(SharedView const &v) {
  return std::string(reinterpret_cast<char const *>(v.data()), v.size());
}

} // namespace

TEST_CASE("ring_layout aligns and rejects overflow", "[shm][ring]") {
  auto l = ring_layout(4, 100);
  REQUIRE(l);
  CHECK(l->slot_stride == 4096);
  CHECK(l->data_offset == 4096);
  CHECK(l->total_size == 4096 + 4 * 4096);
  CHECK(ring_layout(4, 4097)->slot_stride == 8192);
  // 64 slots of table headers + header > 4096 bytes.
  CHECK(ring_layout(64, 1)->data_offset == 8192);
  CHECK_FALSE(ring_layout(1, 100));
  CHECK_FALSE(ring_layout(4, 0));
  CHECK_FALSE(ring_layout(UINT32_MAX, UINT64_MAX / 2));
  CHECK_FALSE(ring_layout(2, UINT64_MAX));
}

TEST_CASE("SharedRing validates geometry", "[shm][ring]") {
  CHECK_THROWS_AS(SharedRing::create(1, 100), std::invalid_argument);
  CHECK_THROWS_AS(SharedRing::create(4, 0), std::invalid_argument);
  CHECK_THROWS_AS(SharedRing::create(UINT32_MAX, SIZE_MAX / 2),
                  std::invalid_argument);
}

TEST_CASE("SharedRing acquire/commit/view round trip", "[shm][ring]") {
  auto ring = SharedRing::create(4, 1000);
  CHECK(ring->slot_count() == 4);
  CHECK(ring->slot_size() == 1000);
  CHECK(ring->segment_name().size() <= 31);

  WritableSlot s = ring->acquire();
  CHECK(s.capacity == 1000);
  CHECK(s.index == 0);
  CHECK((s.write_seq & 1) == 1);
  CHECK(reinterpret_cast<uintptr_t>(s.data) % 4096 == 0);
  std::memcpy(s.data, "payload", 7);
  ShmDescriptor d = ring->commit(s, 7);
  CHECK_FALSE(s); // consumed
  CHECK(d.segment == ring->segment_name());
  CHECK(d.slot == 0);
  CHECK(d.seq == 2);
  CHECK(d.size == 7);
  CHECK(d.nonce.size() == 32);
  CHECK(d.nonce == ring->nonce_hex());
  CHECK(d.host == Mads::detail::host_token());

  // The descriptor survives the JSON wire format.
  ShmDescriptor d2 = nlohmann::json(d).get<ShmDescriptor>();
  CHECK(d2.segment == d.segment);
  CHECK(d2.seq == d.seq);

  SegmentCache cache;
  std::string err;
  auto v = cache.view(d2, {{"k", 1}}, &err);
  REQUIRE(v);
  CHECK(err.empty());
  CHECK(as_string(*v) == "payload");
  CHECK(v->meta()["k"] == 1);
  CHECK(v->valid());
  CHECK(cache.size() == 1);

  // Second view of the same segment reuses the mapping.
  auto d3 = publish(*ring, "second");
  CHECK(d3.slot == 1);
  auto v3 = cache.view(d3, {});
  REQUIRE(v3);
  CHECK(as_string(*v3) == "second");
  CHECK(cache.size() == 1);
  CHECK(v->valid()); // slot 0 untouched
}

TEST_CASE("SharedView is invalidated when the producer wraps",
          "[shm][ring]") {
  auto ring = SharedRing::create(2, 64);
  SegmentCache cache;
  auto d0 = publish(*ring, "first");
  auto v0 = cache.view(d0, {});
  REQUIRE(v0);
  CHECK(v0->valid());

  publish(*ring, "slot1");
  CHECK(v0->valid());

  // Third publish wraps to slot 0.
  auto d2 = publish(*ring, "third");
  CHECK(d2.slot == 0);
  CHECK(d2.seq == 4);
  CHECK_FALSE(v0->valid());

  // The stale descriptor is now rejected outright.
  std::string err;
  CHECK_FALSE(cache.view(d0, {}, &err));
  CHECK(err.find("overwritten") != std::string::npos);

  auto v2 = cache.view(d2, {});
  REQUIRE(v2);
  CHECK(as_string(*v2) == "third");
}

TEST_CASE("SharedView is invalid while its slot is being rewritten",
          "[shm][ring]") {
  auto ring = SharedRing::create(2, 64);
  SegmentCache cache;
  auto d0 = publish(*ring, "a");
  publish(*ring, "b");
  auto v0 = cache.view(d0, {});
  REQUIRE(v0);

  WritableSlot s = ring->acquire(); // slot 0, odd seq
  CHECK(s.index == 0);
  CHECK_FALSE(v0->valid());
  std::string err;
  CHECK_FALSE(cache.view(d0, {}, &err));
  CHECK(err.find("overwritten") != std::string::npos);

  // A descriptor carrying an odd seq is never valid.
  ShmDescriptor odd = d0;
  odd.seq = s.write_seq;
  CHECK_FALSE(cache.view(odd, {}, &err));

  ring->commit(s, 1);
  CHECK_FALSE(v0->valid()); // new contents, new seq
}

TEST_CASE("SharedRing commit rejects misuse", "[shm][ring]") {
  auto ring = SharedRing::create(2, 100);
  WritableSlot s = ring->acquire();
  CHECK_THROWS_AS(ring->commit(s, 101), std::length_error);
  // Still committable after an oversize attempt.
  CHECK_NOTHROW(ring->commit(s, 100));

  // Committing a slot twice.
  WritableSlot t = ring->acquire();
  WritableSlot copy = t;
  ring->commit(t, 1);
  CHECK_THROWS_AS(ring->commit(copy, 1), std::logic_error);

  // A slot re-acquired after a full wrap invalidates the older handle.
  WritableSlot a = ring->acquire(); // slot 0
  ring->acquire();                  // slot 1
  WritableSlot again = ring->acquire(); // slot 0 again
  CHECK_THROWS_AS(ring->commit(a, 1), std::logic_error);
  CHECK_NOTHROW(ring->commit(again, 1));

  // A slot from another ring.
  auto other = SharedRing::create(2, 100);
  WritableSlot foreign = other->acquire();
  CHECK_THROWS_AS(ring->commit(foreign, 1), std::logic_error);
}

TEST_CASE("SegmentCache rejects foreign hosts, bad nonces and bounds",
          "[shm][ring]") {
  auto ring = SharedRing::create(3, 128);
  auto d = publish(*ring, "data");
  SegmentCache cache;
  std::string err;

  SECTION("foreign host") {
    auto f = d;
    f.host = "elsewhere/0000";
    CHECK_FALSE(cache.view(f, {}, &err));
    CHECK(err.find("foreign host") != std::string::npos);
    CHECK(cache.size() == 0); // never even opened
  }
  SECTION("nonce mismatch") {
    auto f = d;
    f.nonce = std::string(32, '0');
    CHECK_FALSE(cache.view(f, {}, &err));
    CHECK(err.find("nonce") != std::string::npos);
    // The genuine descriptor still works afterwards.
    CHECK(cache.view(d, {}));
  }
  SECTION("upper-case nonce is accepted") {
    auto f = d;
    for (auto &c : f.nonce)
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    CHECK(cache.view(f, {}));
  }
  SECTION("slot out of range") {
    auto f = d;
    f.slot = 3;
    CHECK_FALSE(cache.view(f, {}, &err));
    CHECK(err.find("out of range") != std::string::npos);
  }
  SECTION("size beyond slot") {
    auto f = d;
    f.size = 129;
    CHECK_FALSE(cache.view(f, {}, &err));
    CHECK(err.find("exceeds") != std::string::npos);
  }
  SECTION("unknown segment") {
    auto f = d;
    f.segment = make_segment_name();
    CHECK_FALSE(cache.view(f, {}, &err));
    CHECK(err.find("Cannot open") != std::string::npos);
  }
  SECTION("not a ring") {
    auto seg = SharedSegment::create(make_segment_name(), 8192);
    auto f = d;
    f.segment = seg->name();
    CHECK_FALSE(cache.view(f, {}, &err));
    CHECK(err.find("magic") != std::string::npos);
    seg->unlink();
  }
  SECTION("null error pointer is fine") {
    auto f = d;
    f.slot = 99;
    CHECK_FALSE(cache.view(f, {}));
  }
}

TEST_CASE("SegmentCache evicts least recently used segments", "[shm][ring]") {
  auto r1 = SharedRing::create(2, 16);
  auto r2 = SharedRing::create(2, 16);
  auto r3 = SharedRing::create(2, 16);
  SegmentCache cache(2);
  CHECK(cache.capacity() == 2);
  REQUIRE(cache.view(publish(*r1, "1"), {}));
  REQUIRE(cache.view(publish(*r2, "2"), {}));
  REQUIRE(cache.view(publish(*r1, "1b"), {})); // r1 most recent
  REQUIRE(cache.view(publish(*r3, "3"), {}));  // evicts r2
  CHECK(cache.size() == 2);
  // r2 reopens transparently.
  auto v = cache.view(publish(*r2, "2b"), {});
  REQUIRE(v);
  CHECK(as_string(*v) == "2b");
  cache.clear();
  CHECK(cache.size() == 0);
}

TEST_CASE("SharedRing unlinks its segment on destruction", "[shm][ring]") {
  std::string name;
  ShmDescriptor d;
  {
    auto ring = SharedRing::create(2, 16);
    name = ring->segment_name();
    d = publish(*ring, "x");
  }
  SegmentCache cache;
  std::string err;
  CHECK_FALSE(cache.view(d, {}, &err));
  CHECK_THROWS(SharedSegment::open(name));
}

#ifndef _WIN32

TEST_CASE("SharedRing payload is readable from another process",
          "[shm][ring]") {
  auto ring = SharedRing::create(4, 1 << 16);
  std::string text(50000, 'z');
  text[0] = 'A';
  text.back() = 'Z';
  auto d = publish(*ring, text);
  std::string wire = nlohmann::json(d).dump();

  pid_t child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    // No Catch2 macros in the child: report through the exit status.
    int rc = 0;
    try {
      auto desc = nlohmann::json::parse(wire).get<ShmDescriptor>();
      SegmentCache cache;
      std::string err;
      auto v = cache.view(desc, {}, &err);
      if (!v)
        rc = 2;
      else if (as_string(*v) != text)
        rc = 3;
      else if (!v->valid())
        rc = 4;
    } catch (...) {
      rc = 5;
    }
    ::_exit(rc);
  }
  int status = 0;
  REQUIRE(::waitpid(child, &status, 0) == child);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
}

#endif
