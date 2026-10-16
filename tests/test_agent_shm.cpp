// Tests for the shared-memory data plane as seen through Mads::Agent: the
// producer side (enable_shared_output(), publish()/publish_slot() sending
// ring references), the consumer side (receive() returning
// message_type::shared for opted-in agents and an ordinary blob otherwise),
// the failure paths (foreign host, overwritten slots, torn reads), the
// broker-locality rule, and receive_raw_message() materializing references
// for mads-record.
//
// Producer/consumer pairs use cross mode over loopback tcp, as in
// tests/test_agent_wire.cpp. Port range for this file: 44700-44749.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <zmq.hpp>
#include <zmq_addon.hpp>

#include "agent.hpp"
#include "detail/host_token.hpp"
#include "detail/transport.hpp"
#include "detail/wire_format.hpp"
#include "mads_test_helpers.hpp"

using namespace std::chrono_literals;
namespace fs = std::filesystem;
using Mads::message_type;

namespace {

std::unique_ptr<Mads::Agent> make_pub(std::string name, uint16_t port,
                                      uint32_t slots = 4,
                                      size_t slot_size = 64 * 1024) {
  auto a = std::make_unique<Mads::Agent>(name, "none");
  a->init(false, false);
  a->set_cross(true);
  a->set_sub_endpoint(mads_test::loopback(port));
  a->set_sub_topic({});
  if (slots > 0)
    a->enable_shared_output(slots, slot_size);
  a->connect(std::chrono::milliseconds(0));
  return a;
}

std::unique_ptr<Mads::Agent> make_sub(std::string name, uint16_t port,
                                      bool accept_views) {
  auto a = std::make_unique<Mads::Agent>(name, "none");
  a->init(false, false);
  a->set_sub_endpoint(mads_test::loopback(port));
  a->set_pub_topic("");
  a->set_sub_topic({""});
  a->accept_shared_views(accept_views);
  a->connect(std::chrono::milliseconds(0));
  return a;
}

// Publishes until `received` holds (slow-joiner: the first sends may be lost).
template <typename PublishFn, typename CheckFn>
bool retry_until(PublishFn publish_once, CheckFn received,
                 std::chrono::milliseconds total = 3000ms) {
  auto deadline = std::chrono::steady_clock::now() + total;
  while (std::chrono::steady_clock::now() < deadline) {
    publish_once();
    if (mads_test::wait_for(received, 150ms, 5ms))
      return true;
  }
  return false;
}

std::vector<unsigned char> pattern(size_t n, unsigned char seed) {
  std::vector<unsigned char> v(n);
  for (size_t i = 0; i < n; ++i)
    v[i] = static_cast<unsigned char>(seed + i * 7);
  return v;
}

// A raw PUB socket plus a subscriber agent, to inject hand-made frames.
struct RawInjector {
  zmq::context_t ctx;
  zmq::socket_t pub{ctx, zmq::socket_type::pub};
  std::unique_ptr<Mads::Agent> sub;
  RawInjector(uint16_t port, bool accept_views) {
    pub.bind(mads_test::loopback(port));
    sub = make_sub("rawsub", port, accept_views);
  }
  void send_shm(nlohmann::json meta) {
    using namespace Mads::detail;
    zmq::multipart_t m;
    m.addstr("inj");
    m.addstr(make_wire_header(Mads::WireFormat::MsgPack, Comp::None,
                              WIRE_FLAGS_SHM_REF));
    m.addstr(encode_payload(meta, Mads::WireFormat::MsgPack));
    m.addmem(nullptr, 0);
    m.send(pub);
  }
};

} // namespace

// ---------------------------------------------------------------------------
// Producer setup and the locality rule
// ---------------------------------------------------------------------------

TEST_CASE("shm_unusable_reason() decides whether the broker is local",
          "[agent_shm]") {
  using Mads::detail::shm_unusable_reason;
  const std::string me = "myhost/abc";
  CHECK(shm_unusable_reason("ipc", "ipc:///tmp/x", "", me).empty());
  CHECK(shm_unusable_reason("tcp", "tcp://localhost:9090", "", me).empty());
  CHECK(shm_unusable_reason("tcp", "tcp://127.0.0.1:9090", "", me).empty());
  CHECK(shm_unusable_reason("tcp", "tcp://[::1]:9090", "", me).empty());
  CHECK(shm_unusable_reason("tcp", "tcp://*:9090", "", me).empty());
  CHECK(shm_unusable_reason("tcp", "tcp://MyHost:9090", "", me).empty());
  CHECK(shm_unusable_reason("tcp", "tcp://myhost.lan:9090", "", me).empty());
  CHECK_FALSE(
      shm_unusable_reason("tcp", "tcp://192.0.2.7:9090", "", me).empty());
  CHECK_FALSE(shm_unusable_reason("tcp", "tcp://other:9090", "", me).empty());
  CHECK_FALSE(shm_unusable_reason("tcp", "bogus", "", me).empty());
  // The broker's own host token, when known, wins over the address.
  const std::string mine = R"({"host_token":"myhost/abc"})";
  const std::string theirs = R"({"host_token":"far/def"})";
  CHECK(shm_unusable_reason("tcp", "tcp://192.0.2.7:9090", mine, me).empty());
  CHECK_FALSE(
      shm_unusable_reason("tcp", "tcp://localhost:9090", theirs, me).empty());
  // A legacy (empty) reply falls back to the address.
  CHECK(shm_unusable_reason("tcp", "tcp://localhost:9090", "", me).empty());
}

TEST_CASE("enable_shared_output() keeps blobs inline for a remote broker",
          "[agent_shm]") {
  Mads::Agent a("shmremote", "none");
  CHECK_THROWS_AS(a.enable_shared_output(4, 1024), Mads::AgentError);
  a.init(false, false);
  CHECK_FALSE(a.shared_output_enabled());
  CHECK(a.shared_slot_size() == 0);
  CHECK_THROWS_AS(a.acquire_slot(), Mads::AgentError);

  a.set_pub_endpoint("tcp://192.0.2.1:9090"); // TEST-NET: never this host
  a.enable_shared_output(4, 1024);
  CHECK_FALSE(a.shared_output_enabled());

  a.set_pub_endpoint("tcp://localhost:9090");
  CHECK_THROWS_AS(a.enable_shared_output(1, 1024), Mads::AgentError);
  CHECK_THROWS_AS(a.enable_shared_output(4, 0), Mads::AgentError);
  a.enable_shared_output(4, 1024);
  REQUIRE(a.shared_output_enabled());
  CHECK(a.shared_slot_size() == 1024);
}

TEST_CASE("init() enables shared output from the settings file",
          "[agent_shm]") {
  const auto ini =
      fs::temp_directory_path() /
      ("mads_test_shm_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()) +
       ".ini");
  {
    std::ofstream f(ini);
    f << "[agents]\nshm_slots = 3\n"
         "[shmon]\nshm = true\nshm_slot_size = 8192\n"
         "[shmoff]\npub_topic = \"x\"\n"
         "[shmbad]\nshm = true\nshm_slots = 1\n";
  }
  {
    Mads::Agent on("shmon", ini.string());
    on.init(false, false);
    REQUIRE(on.shared_output_enabled());
    CHECK(on.shared_slot_size() == 8192);
    // [agents] shm_slots applies: slot indices wrap after 3 acquisitions.
    auto s0 = on.acquire_slot();
    on.abandon_slot(s0);
    std::vector<uint32_t> idx;
    for (int i = 0; i < 4; ++i) {
      auto s = on.acquire_slot();
      idx.push_back(s.index);
      on.publish_slot(s, 0); // never connected: dropped by ZMQ, fine
    }
    CHECK(idx == std::vector<uint32_t>{0, 1, 2, 0});
  }
  {
    Mads::Agent off("shmoff", ini.string());
    off.init(false, false);
    CHECK_FALSE(off.shared_output_enabled());
  }
  {
    Mads::Agent bad("shmbad", ini.string());
    CHECK_THROWS_AS(bad.init(false, false), Mads::AgentError);
  }
  std::error_code ec;
  fs::remove(ini, ec);
}

// ---------------------------------------------------------------------------
// Pub/sub
// ---------------------------------------------------------------------------

TEST_CASE("shm producer -> opted-in consumer gets a zero-copy shared view",
          "[agent_shm]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 44700;
  const uint32_t slots = 4;
  auto pub = make_pub("shmpub", port, slots, 4096);
  REQUIRE(pub->shared_output_enabled());
  auto sub = make_sub("shmsub", port, true);

  const auto payload = pattern(1000, 3);
  REQUIRE(retry_until(
      [&] { pub->publish(payload, {{"format", "test"}, {"k", 1}}, "t"); },
      [&] { return sub->receive(true) == message_type::shared; }));
  // Keep the newest reference (the retry loop may have sent several).
  while (sub->receive(true) != message_type::none) {
  }
  Mads::SharedView view = sub->last_shared();
  REQUIRE(view);
  CHECK(sub->last_shared_topic() == "t");
  REQUIRE(view.size() == payload.size());
  CHECK(std::memcmp(view.data(), payload.data(), payload.size()) == 0);
  CHECK(view.valid());
  CHECK(view.meta().at("format") == "test");
  CHECK(view.meta().at("k") == 1);
  CHECK(view.meta().contains("timestamp"));
  CHECK_FALSE(view.meta().contains("shm"));

  // Zero copy: the view reads the producer's ring in place. Take slots until
  // the producer is writing the viewed one again (an odd seq invalidates the
  // view at once), scribble on it, and observe the bytes through the view.
  Mads::WritableSlot slot;
  for (uint32_t i = 0; i < slots && view.valid(); ++i) {
    if (slot)
      pub->publish_slot(slot, 0);
    slot = pub->acquire_slot();
  }
  REQUIRE(slot);
  CHECK_FALSE(view.valid());
  std::memset(slot.data, 0xAB, 16);
  CHECK(view.data()[0] == 0xAB);
  CHECK(view.data()[15] == 0xAB);
  pub->abandon_slot(slot);
  CHECK(sub->shm_failed() == 0);
  CHECK(sub->shm_torn() == 0);
}

TEST_CASE("shm producer -> non-opted consumer gets an ordinary blob",
          "[agent_shm]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 44701;
  auto pub = make_pub("shmpub2", port);
  auto sub = make_sub("shmsub2", port, false);

  const auto payload = pattern(40000, 9);
  REQUIRE(
      retry_until([&] { pub->publish(payload, {{"format", "bytes"}}, "b"); },
                  [&] { return sub->receive(true) == message_type::blob; }));
  auto [topic, meta_text, bytes] = sub->last_blob();
  CHECK(topic == "b");
  CHECK(bytes == payload);
  auto meta = nlohmann::json::parse(meta_text);
  CHECK(meta.at("format") == "bytes");
  CHECK_FALSE(meta.contains("shm"));
  auto [vt, vf, vb] = sub->last_blob_view();
  CHECK(vb.size() == payload.size());
  CHECK_FALSE(sub->last_shared());
}

TEST_CASE("last_topic() follows every received message type", "[agent_shm]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 44710;
  auto pub = make_pub("shmpub11", port);
  auto views = make_sub("shmsub11v", port, true);
  auto copies = make_sub("shmsub11c", port, false);
  const auto payload = pattern(2000, 2);

  REQUIRE(retry_until([&] { pub->publish(nlohmann::json{{"x", 1}}, "j"); },
                      [&] {
                        return views->receive(true) == message_type::json;
                      }));
  CHECK(views->last_topic() == "j");
  REQUIRE(retry_until(
      [&] { pub->publish(payload, {{"format", "t"}}, "v"); },
      [&] { return views->receive(true) == message_type::shared; }));
  CHECK(views->last_topic() == "v");
  CHECK(views->last_shared_topic() == "v");

  REQUIRE(retry_until(
      [&] { pub->publish(payload, {{"format", "t"}}, "c"); },
      [&] {
        auto t = copies->receive(true);
        return t == message_type::blob && copies->last_topic() == "c";
      }));
  CHECK(std::get<0>(copies->last_blob()) == "c");
}

TEST_CASE("a reference frame reads as an empty blob to pre-shm receivers",
          "[agent_shm]") {
  using namespace Mads::detail;
  mads_test::RunningGuard guard;
  const uint16_t port = 44709;
  auto pub = make_pub("shmpub10", port);
  zmq::context_t ctx;
  zmq::socket_t raw(ctx, zmq::socket_type::sub);
  raw.set(zmq::sockopt::subscribe, "");
  raw.set(zmq::sockopt::rcvtimeo, 50);
  raw.connect(mads_test::loopback(port));

  const auto payload = pattern(3000, 4);
  zmq::multipart_t frame;
  REQUIRE(retry_until(
      [&] { pub->publish(payload, {{"format", "old"}}, "o"); },
      [&] {
        frame.clear();
        return frame.recv(raw) && frame.at(0).to_string() == "o";
      }));
  // [topic][header][meta][empty bytes]
  REQUIRE(frame.size() == 4);
  WireHeader hdr;
  REQUIRE(parse_wire_header(frame.at(1).to_string(), hdr));
  CHECK(hdr.has_shm);
  // What a receiver ignoring the shm flag acts on: a blob with zero bytes
  // and the producer's meta, not a data frame.
  CHECK(hdr.has_blob);
  CHECK(frame.at(3).size() == 0);
  std::string meta_text;
  REQUIRE(decode_to_json_text(frame.at(2).to_string(), hdr.format,
                              hdr.compression, meta_text));
  auto meta = nlohmann::json::parse(meta_text);
  CHECK(meta.at("format") == "old");
  CHECK(meta.contains("shm"));
}

TEST_CASE("publish_slot() publishes bytes written in place", "[agent_shm]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 44702;
  auto pub = make_pub("shmpub3", port);
  auto sub = make_sub("shmsub3", port, false);

  const std::string text = "written straight into the ring";
  REQUIRE(retry_until(
      [&] {
        auto slot = pub->acquire_slot();
        REQUIRE(slot.capacity >= text.size());
        std::memcpy(slot.data, text.data(), text.size());
        pub->publish_slot(slot, text.size(), {{"format", "text"}}, "s");
        CHECK_FALSE(slot); // consumed
      },
      [&] { return sub->receive(true) == message_type::blob; }));
  auto [topic, meta, bytes] = sub->last_blob();
  CHECK(topic == "s");
  CHECK(std::string(bytes.begin(), bytes.end()) == text);

  // Misuse: oversize (the slot is abandoned), and a consumed slot.
  auto slot = pub->acquire_slot();
  auto copy = slot;
  CHECK_THROWS_AS(pub->publish_slot(slot, slot.capacity + 1), Mads::AgentError);
  CHECK_THROWS_AS(pub->publish_slot(copy, 1), Mads::AgentError);
}

TEST_CASE("an oversize blob falls back to an inline publish", "[agent_shm]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 44703;
  auto pub = make_pub("shmpub4", port, 4, 4096);
  auto sub = make_sub("shmsub4", port, true);

  const auto big = pattern(10000, 1);
  REQUIRE(
      retry_until([&] { pub->publish(big, {{"format", "big"}}, "o"); },
                  [&] { return sub->receive(true) == message_type::blob; }));
  CHECK(std::get<2>(sub->last_blob()) == big);
  // A fitting blob from the same producer still goes through the ring.
  const auto fitting = pattern(100, 2);
  REQUIRE(
      retry_until([&] { pub->publish(fitting, {{"format", "s"}}, "o"); },
                  [&] { return sub->receive(true) == message_type::shared; }));
  CHECK(sub->last_shared().size() == fitting.size());
}

// ---------------------------------------------------------------------------
// Overwrites, torn reads, failures
// ---------------------------------------------------------------------------

TEST_CASE("a wrapping producer invalidates a held view; late references "
          "fail cleanly",
          "[agent_shm]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 44704;
  auto pub = make_pub("shmpub5", port, 2, 4096);
  auto sub = make_sub("shmsub5", port, true);

  REQUIRE(
      retry_until([&] { pub->publish(pattern(64, 0), {}, "w"); },
                  [&] { return sub->receive(true) == message_type::shared; }));
  Mads::SharedView held = sub->last_shared();
  REQUIRE(held.valid());
  // Drain whatever the retry loop left queued, then wrap the ring.
  while (sub->receive(true) != message_type::none) {
  }
  held = sub->last_shared();
  REQUIRE(held.valid());
  const size_t failed_before = sub->shm_failed();
  for (int i = 0; i < 4; ++i)
    pub->publish(pattern(64, static_cast<unsigned char>(i)), {}, "w");
  CHECK_FALSE(held.valid());
  // The first two of the four references point at slots overwritten by the
  // last two: they fail (counted); the last two resolve.
  int shared = 0;
  mads_test::wait_for([&] {
    if (sub->receive(true) == message_type::shared)
      ++shared;
    return shared == 2 && sub->shm_failed() == failed_before + 2;
  });
  CHECK(shared == 2);
  CHECK(sub->shm_failed() == failed_before + 2);

  // record_torn_view() is the counter the plugin loader feeds.
  const size_t torn = sub->shm_torn();
  sub->record_torn_view();
  CHECK(sub->shm_torn() == torn + 1);
}

TEST_CASE("non-opted consumers never deliver torn data", "[agent_shm]") {
  // A producer hammering a 2-slot ring with large payloads while a consumer
  // copies them: references are often overwritten before (shm_failed) or
  // while (shm_torn) they are copied. Whatever gets through must be intact:
  // every payload is filled with one byte value.
  mads_test::RunningGuard guard;
  const uint16_t port = 44705;
  const size_t n = 1 << 20;
  auto pub = make_pub("shmpub6", port, 2, n);
  auto sub = make_sub("shmsub6", port, false);

  REQUIRE(retry_until(
      [&] { pub->publish(std::vector<unsigned char>(n, 1), {}, "h"); },
      [&] { return sub->receive(true) == message_type::blob; }));
  std::atomic<bool> stop{false};
  std::thread producer([&] {
    unsigned char v = 0;
    while (!stop) {
      auto slot = pub->acquire_slot();
      std::memset(slot.data, ++v, n);
      pub->publish_slot(slot, n, {}, "h");
    }
  });
  size_t delivered = 0, corrupt = 0;
  const auto deadline = std::chrono::steady_clock::now() + 1500ms;
  while (std::chrono::steady_clock::now() < deadline) {
    if (sub->receive(true) != message_type::blob)
      continue;
    auto [vt, vf, bytes] = sub->last_blob_view();
    ++delivered;
    for (auto b : bytes)
      if (b != bytes[0]) {
        ++corrupt;
        break;
      }
  }
  stop = true;
  producer.join();
  CHECK(corrupt == 0);
  INFO("delivered " << delivered << ", failed " << sub->shm_failed()
                    << ", torn " << sub->shm_torn());
  CHECK(delivered + sub->shm_failed() + sub->shm_torn() > 0);
}

TEST_CASE("a reference from a foreign host is refused and counted",
          "[agent_shm]") {
  mads_test::RunningGuard guard;
  RawInjector inj(44706, true);
  Mads::ShmDescriptor d;
  d.segment = "/mads_nonexistent";
  d.slot = 0;
  d.seq = 2;
  d.size = 4;
  d.nonce = std::string(32, '0');
  d.host = "elsewhere/" + std::string(8, 'f');
  nlohmann::json meta = {{"format", "raw"}, {"shm", d}};
  REQUIRE(retry_until([&] { inj.send_shm(meta); },
                      [&] {
                        inj.sub->receive(true);
                        return inj.sub->shm_failed() > 0;
                      }));
  CHECK(inj.sub->shm_torn() == 0);
  CHECK_FALSE(inj.sub->last_shared());

  // Same host, but no such segment (producer gone): also a failure.
  const size_t failed = inj.sub->shm_failed();
  d.host = Mads::detail::host_token();
  meta["shm"] = d;
  REQUIRE(retry_until([&] { inj.send_shm(meta); },
                      [&] {
                        inj.sub->receive(true);
                        return inj.sub->shm_failed() > failed;
                      }));

  // A frame with no descriptor is malformed: dropped, not an shm failure.
  const size_t dropped = inj.sub->dropped_messages();
  const size_t failed2 = inj.sub->shm_failed();
  REQUIRE(retry_until([&] { inj.send_shm({{"format", "raw"}}); },
                      [&] {
                        inj.sub->receive(true);
                        return inj.sub->dropped_messages() > dropped;
                      }));
  CHECK(inj.sub->shm_failed() == failed2);
}

// ---------------------------------------------------------------------------
// Raw receive (mads-record) and replay (mads-play)
// ---------------------------------------------------------------------------

TEST_CASE("receive_raw_message() materializes references into blob frames",
          "[agent_shm]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 44707;
  const uint16_t replay_port = 44708;
  auto pub = make_pub("shmpub7", port);
  auto rec = make_sub("shmrec7", port, false);

  const auto payload = pattern(5000, 5);
  std::string topic;
  std::vector<std::string> parts;
  REQUIRE(retry_until(
      [&] { pub->publish(payload, {{"format", "rec"}}, "r"); },
      [&] { return rec->receive_raw_message(topic, parts, true); }));
  CHECK(topic == "r");
  REQUIRE(parts.size() == 3);
  Mads::detail::WireHeader hdr;
  REQUIRE(Mads::detail::parse_wire_header(parts[0], hdr));
  CHECK(hdr.has_blob);
  CHECK_FALSE(hdr.has_shm);
  CHECK(parts[2] == std::string(payload.begin(), payload.end()));
  auto meta = nlohmann::json::from_msgpack(parts[1]);
  CHECK(meta.at("format") == "rec");
  CHECK_FALSE(meta.contains("shm"));

  // Replaying the recorded frame delivers the real bytes, even with the
  // producer (and its ring) gone.
  pub.reset();
  auto player = make_pub("shmplay7", replay_port, 0);
  auto sink = make_sub("shmsink7", replay_port, true);
  REQUIRE(
      retry_until([&] { player->publish_raw_message(topic, parts); },
                  [&] { return sink->receive(true) == message_type::blob; }));
  CHECK(std::get<2>(sink->last_blob()) == payload);
}
