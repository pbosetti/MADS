// Unit tests for the Mads::Agent on-the-wire codec: the WireFormat x
// Compression matrix implemented in the anonymous namespace of
// src/agent.cpp:62-191, exercised indirectly through publish()/receive()
// round trips over an in-process ZeroMQ loopback pair.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent.hpp"
#include "detail/wire_format.hpp"
#include "mads_test_helpers.hpp"

using namespace std::chrono_literals;

namespace {

std::unique_ptr<Mads::Agent> make_pub(std::string name, uint16_t port,
                                      Mads::WireFormat wf,
                                      Mads::Compression comp) {
  auto a = std::make_unique<Mads::Agent>(name, "none");
  a->init(false, false);
  a->set_cross(true);
  a->set_sub_endpoint(mads_test::loopback(port));
  a->set_sub_topic({});
  a->set_wire_format(wf);
  a->set_compression(comp);
  a->connect(std::chrono::milliseconds(0));
  return a;
}

std::unique_ptr<Mads::Agent> make_sub(std::string name, uint16_t port) {
  auto a = std::make_unique<Mads::Agent>(name, "none");
  a->init(false, false);
  a->set_sub_endpoint(mads_test::loopback(port));
  a->set_pub_topic("");
  a->set_sub_topic({""});
  a->connect(std::chrono::milliseconds(0));
  return a;
}

template <typename PublishFn, typename CheckFn>
bool retry_until(PublishFn publish_once, CheckFn check_received,
                 std::chrono::milliseconds total = 3000ms,
                 std::chrono::milliseconds settle = 150ms) {
  auto deadline = std::chrono::steady_clock::now() + total;
  while (std::chrono::steady_clock::now() < deadline) {
    publish_once();
    if (mads_test::wait_for(check_received, settle, 10ms))
      return true;
  }
  return false;
}

// Hand-crafts a MADS extended-frame header identical to
// src/agent.cpp make_wire_header()/WIRE_HEADER_SIZE, used only to inject
// synthetic frames via a raw cppzmq PUB socket (bypassing Agent::publish()).
std::string wire_header_bytes(uint8_t fmt, uint8_t comp, bool blob) {
  std::string h;
  h.append("MADS", 4);
  h.push_back(static_cast<char>(1)); // header version
  h.push_back(static_cast<char>(fmt));
  h.push_back(static_cast<char>(comp));
  h.push_back(static_cast<char>(blob ? 0x01 : 0x00));
  h.push_back(static_cast<char>(0)); // schema (4 bytes, unchecked by reader)
  h.push_back(static_cast<char>(0));
  h.push_back(static_cast<char>(0));
  h.push_back(static_cast<char>(0));
  return h;
}

void check_round_trip(Mads::WireFormat wf, Mads::Compression comp,
                      uint16_t port) {
  auto pub = make_pub("pubW", port, wf, comp);
  auto sub = make_sub("subW", port);

  nlohmann::json payload = {
      {"greeting", "hello wire"}, {"num", 123}, {"arr", {1, 2, 3}}};
  bool got = retry_until(
      [&] { pub->publish(payload, "t"); },
      [&] { return sub->receive(true) == Mads::message_type::json; });
  REQUIRE(got);
  auto [topic, doc] = sub->last_json();
  REQUIRE(topic == "t");
  REQUIRE(doc.at("greeting") == "hello wire");
  REQUIRE(doc.at("num") == 123);
  REQUIRE(doc.at("arr") == nlohmann::json({1, 2, 3}));
  REQUIRE(sub->dropped_messages() == 0);
}

} // namespace

// ---------------------------------------------------------------------------
// WireFormat x Compression matrix
// ---------------------------------------------------------------------------

TEST_CASE("wire codec: Json + Compression::None round-trips", "[agent_wire]") {
  mads_test::RunningGuard guard;
  check_round_trip(Mads::WireFormat::Json, Mads::Compression::None, 42120);
}

TEST_CASE("wire codec: Json + Compression::Snappy round-trips",
          "[agent_wire]") {
  mads_test::RunningGuard guard;
  check_round_trip(Mads::WireFormat::Json, Mads::Compression::Snappy, 42121);
}

TEST_CASE("wire codec: Json + Compression::Auto round-trips (small payload)",
          "[agent_wire]") {
  mads_test::RunningGuard guard;
  check_round_trip(Mads::WireFormat::Json, Mads::Compression::Auto, 42122);
}

TEST_CASE("wire codec: MsgPack + Compression::None round-trips",
          "[agent_wire]") {
  mads_test::RunningGuard guard;
  check_round_trip(Mads::WireFormat::MsgPack, Mads::Compression::None, 42123);
}

TEST_CASE("wire codec: MsgPack + Compression::Snappy round-trips",
          "[agent_wire]") {
  mads_test::RunningGuard guard;
  check_round_trip(Mads::WireFormat::MsgPack, Mads::Compression::Snappy,
                   42124);
}

TEST_CASE(
    "wire codec: MsgPack + Compression::Auto round-trips (small payload)",
    "[agent_wire]") {
  mads_test::RunningGuard guard;
  check_round_trip(Mads::WireFormat::MsgPack, Mads::Compression::Auto, 42125);
}

// ---------------------------------------------------------------------------
// Compression::Auto threshold behavior
// ---------------------------------------------------------------------------

TEST_CASE("Compression::Auto round-trips identically below and above the "
          "256-byte threshold",
          "[agent_wire]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42126;
  auto pub = make_pub("pubAuto", port, Mads::WireFormat::Json,
                      Mads::Compression::Auto);
  auto sub = make_sub("subAuto", port);

  // Not named "small"/"big": <rpcndr.h> (pulled in transitively on Windows)
  // #defines "small" to "char", silently mangling a variable with that name.
  nlohmann::json small_payload = {{"tag", "small"}, {"n", 1}};
  REQUIRE(small_payload.dump().size() < Mads::COMPRESSION_AUTO_THRESHOLD);

  std::string filler(400, 'x');
  nlohmann::json big_payload = {{"tag", "big"}, {"filler", filler}};
  REQUIRE(big_payload.dump().size() >= Mads::COMPRESSION_AUTO_THRESHOLD);

  bool got_small = retry_until(
      [&] { pub->publish(small_payload, "s"); },
      [&] { return sub->receive(true) == Mads::message_type::json; });
  REQUIRE(got_small);
  auto [t1, d1] = sub->last_json();
  REQUIRE(d1.at("tag") == "small");
  REQUIRE(d1.at("n") == 1);

  bool got_big = retry_until(
      [&] { pub->publish(big_payload, "b"); },
      [&] { return sub->receive(true) == Mads::message_type::json; });
  REQUIRE(got_big);
  auto [t2, d2] = sub->last_json();
  REQUIRE(d2.at("tag") == "big");
  REQUIRE(d2.at("filler") == filler);
}

// ---------------------------------------------------------------------------
// Large Compression::Snappy payload
// ---------------------------------------------------------------------------

TEST_CASE("Compression::Snappy round-trips a large payload byte-identical",
          "[agent_wire]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42127;
  auto pub = make_pub("pubBig", port, Mads::WireFormat::Json,
                      Mads::Compression::Snappy);
  auto sub = make_sub("subBig", port);

  std::string filler;
  filler.reserve(5000);
  for (int i = 0; i < 5000; ++i)
    filler.push_back(static_cast<char>('a' + (i % 26)));
  nlohmann::json payload = {{"filler", filler}};

  bool got = retry_until(
      [&] { pub->publish(payload, "big"); },
      [&] { return sub->receive(true) == Mads::message_type::json; });
  REQUIRE(got);
  auto [topic, doc] = sub->last_json();
  REQUIRE(doc.at("filler").get<std::string>() == filler);
}

// ---------------------------------------------------------------------------
// dropped_messages() on malformed frames, and continued usability afterwards
// ---------------------------------------------------------------------------

TEST_CASE("dropped_messages() counts malformed frames and the agent stays "
          "usable afterwards",
          "[agent_wire]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42128;
  zmq::context_t ctx;
  zmq::socket_t raw_pub(ctx, zmq::socket_type::pub);
  raw_pub.bind(mads_test::loopback(port));

  auto sub = make_sub("subMal", port);
  // Settle: subscriber connect + ZMQ subscription propagation to raw_pub.
  std::this_thread::sleep_for(300ms);

  size_t dropped_before = sub->dropped_messages();

  // Malformed legacy 2-part frame: [topic]["not actually snappy data"].
  // decode_to_payload() assumes 2-part legacy frames are snappy(json); this
  // payload fails to decompress, so it must be dropped, not delivered.
  bool saw_drop = false;
  for (int attempt = 0; attempt < 50 && !saw_drop; ++attempt) {
    zmq::multipart_t bad;
    bad.addstr(std::string("mal"));
    bad.addstr(std::string("this is definitely not snappy compressed data"));
    bad.send(raw_pub);
    // dropped_messages() only increments as a side effect of receive()
    // actually parsing (and rejecting) a frame, so the predicate must drive
    // receive() itself rather than passively poll the counter.
    saw_drop = mads_test::wait_for(
        [&] {
          sub->receive(true);
          return sub->dropped_messages() > dropped_before;
        },
        100ms, 10ms);
  }
  REQUIRE(saw_drop);
  REQUIRE(sub->dropped_messages() > dropped_before);
  // The bad frame must never surface as a normal message.
  REQUIRE(sub->receive(true) != Mads::message_type::json);

  // A well-formed extended-header frame sent right after must still arrive:
  // the agent is not wedged by the earlier malformed frame.
  size_t dropped_after_bad = sub->dropped_messages();
  bool got_good = false;
  for (int attempt = 0; attempt < 50 && !got_good; ++attempt) {
    zmq::multipart_t good;
    good.addstr(std::string("mal"));
    good.addstr(wire_header_bytes(0 /*Json*/, 0 /*None*/, false));
    good.addstr(std::string(R"({"ok":true})"));
    good.send(raw_pub);
    got_good = mads_test::wait_for(
        [&] { return sub->receive(true) == Mads::message_type::json; }, 150ms,
        10ms);
  }
  REQUIRE(got_good);
  auto [topic, doc] = sub->last_json();
  REQUIRE(topic == "mal");
  REQUIRE(doc.at("ok") == true);
  REQUIRE(sub->dropped_messages() == dropped_after_bad); // no further drops
}

// ---------------------------------------------------------------------------
// Shared-memory reference frames: [topic][header(shm)][meta + descriptor]
// (pub/sub coverage in tests/test_agent_shm.cpp)
// ---------------------------------------------------------------------------

TEST_CASE("wire header carries the shm flag independently of the blob flag",
          "[agent_wire][shm]") {
  using namespace Mads::detail;
  WireHeader h;
  REQUIRE(parse_wire_header(
      make_wire_header(Mads::WireFormat::MsgPack, Comp::None, WIRE_FLAG_SHM),
      h));
  CHECK(h.has_shm);
  CHECK_FALSE(h.has_blob);
  CHECK(h.format == static_cast<uint8_t>(Mads::WireFormat::MsgPack));
  REQUIRE(parse_wire_header(
      make_wire_header(Mads::WireFormat::Json, Comp::None, true), h));
  CHECK(h.has_blob);
  CHECK_FALSE(h.has_shm);
  // The legacy bool overload never sets the shm flag.
  REQUIRE(parse_wire_header(
      make_wire_header(Mads::WireFormat::Json, Comp::Snappy, false), h));
  CHECK_FALSE(h.has_shm);
  CHECK_FALSE(h.has_blob);
}

TEST_CASE("shm descriptor round-trips through the encoded meta",
          "[agent_wire][shm]") {
  using namespace Mads::detail;
  Mads::ShmDescriptor d;
  d.segment = "/mads_0000002a00000001";
  d.slot = 3;
  d.seq = 42;
  d.size = 1234567;
  d.nonce = "00112233445566778899aabbccddeeff";
  d.host = "host/id";
  nlohmann::json meta = {{"format", "raw"}, {"shm", d}};
  CHECK(meta["shm"].at("seg") == d.segment);
  for (auto fmt : {Mads::WireFormat::MsgPack, Mads::WireFormat::Json}) {
    std::string text;
    REQUIRE(decode_to_json_text(encode_payload(meta, fmt),
                                static_cast<uint8_t>(fmt),
                                static_cast<uint8_t>(Comp::None), text));
    auto back =
        nlohmann::json::parse(text).at("shm").get<Mads::ShmDescriptor>();
    CHECK(back.segment == d.segment);
    CHECK(back.slot == d.slot);
    CHECK(back.seq == d.seq);
    CHECK(back.size == d.size);
    CHECK(back.nonce == d.nonce);
    CHECK(back.host == d.host);
  }
  CHECK_THROWS(nlohmann::json{{"seg", "x"}}.get<Mads::ShmDescriptor>());
}
