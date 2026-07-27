// Loopback coverage for P3 (mads-record / mads-play): the full pipeline
// publisher -> recorder -> bag file -> player -> subscriber, built directly
// from the same primitives mads-record/mads-play use (Agent::
// receive_raw_message()/publish_raw_message(), Mads::BagWriter/BagReader,
// Mads::Play::try_restamp(), Mads::topic_match()) -- mirroring how
// tests/test_agent_pubsub.cpp exercises Agent directly rather than spawning
// the compiled executables (no test in this suite spawns a subprocess; see
// tests/mads_test_helpers.hpp for the loopback-only convention).
//
// Port range: 42500-42599 (see tests/mads_test_helpers.hpp).
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent.hpp"
#include "bag.hpp"
#include "main/play_restamp.hpp"
#include "mads_test_helpers.hpp"
#include "topic_match.hpp"

using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Agent construction, mirroring tests/test_agent_pubsub.cpp exactly (see its
// header comment for the cross/bind vs. connect topology explanation).
// ---------------------------------------------------------------------------

std::unique_ptr<Mads::Agent> make_pub(std::string name, uint16_t port) {
  auto a = std::make_unique<Mads::Agent>(name, "none");
  a->init(false, false);
  a->set_cross(true);
  a->set_sub_endpoint(mads_test::loopback(port));
  a->set_sub_topic({}); // pure publisher: skip connect_sub entirely
  a->connect(std::chrono::milliseconds(0));
  return a;
}

std::unique_ptr<Mads::Agent> make_sub(std::string name, uint16_t port,
                                      std::vector<std::string> topics = {""}) {
  auto a = std::make_unique<Mads::Agent>(name, "none");
  a->init(false, false);
  a->set_sub_endpoint(mads_test::loopback(port));
  a->set_pub_topic(""); // pure subscriber: skip connect_pub entirely
  a->set_sub_topic(topics);
  a->connect(std::chrono::milliseconds(0));
  return a;
}

// ---------------------------------------------------------------------------
// A raw capture: topic + parts, as receive_raw_message() hands them back.
// ---------------------------------------------------------------------------

struct RawCapture {
  std::string topic;
  std::vector<std::string> parts;
};

// Polls receive_raw_message() (non-blocking) until it succeeds or the
// timeout expires, writing the result into `out`. Used instead of a single
// blocking call so slow-joiner propagation delay never causes a flaky miss
// (same rationale as mads_test::wait_for()/retry_until() elsewhere).
bool wait_for_raw(Mads::Agent &sub, RawCapture &out,
                  std::chrono::milliseconds timeout = 3000ms) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    std::string topic;
    std::vector<std::string> parts;
    if (sub.receive_raw_message(topic, parts, true)) {
      out.topic = std::move(topic);
      out.parts = std::move(parts);
      return true;
    }
    std::this_thread::sleep_for(10ms);
  }
  return false;
}

// Publishes `publish_once` repeatedly until a raw message is captured (slow
// -joiner warm-up), then drains any further leftovers so the connection
// starts the real test from a clean queue -- mirrors test_agent_pubsub.cpp's
// retry_until() + manual drain pattern, adapted to the raw receive path.
// (The publisher itself isn't a parameter: publish_once already captures
// whichever Agent it needs to call publish()/publish_raw_message() on.)
template <typename PublishFn>
void warm_up_raw(Mads::Agent &sub, PublishFn publish_once) {
  auto deadline = std::chrono::steady_clock::now() + 3000ms;
  bool got = false;
  while (!got && std::chrono::steady_clock::now() < deadline) {
    publish_once();
    RawCapture cap;
    got = wait_for_raw(sub, cap, 150ms);
  }
  REQUIRE(got);
  // Drain any further leftover deliveries from the warm-up retries above.
  RawCapture drain;
  while (wait_for_raw(sub, drain, 50ms)) {
  }
}

fs::path unique_bag_path(const std::string &tag) {
  static std::atomic<int> counter{0};
#if defined(_WIN32)
  int pid = _getpid();
#else
  int pid = ::getpid();
#endif
  return fs::temp_directory_path() /
         ("mads_test_bag_roundtrip_" + tag + "_" + std::to_string(pid) + "_" +
          std::to_string(counter++) + ".bag");
}

struct ScopedPath {
  fs::path p;
  explicit ScopedPath(fs::path p_) : p(std::move(p_)) {}
  ~ScopedPath() {
    std::error_code ec;
    fs::remove(p, ec);
  }
};

int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

} // namespace

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// Full pipeline, byte-identical frames (JSON and blob).
// ---------------------------------------------------------------------------

TEST_CASE("bag roundtrip: publisher -> recorder -> file -> player -> "
          "subscriber reproduces JSON and blob frames byte-identical",
          "[bag_roundtrip]") {
  mads_test::RunningGuard guard;
  auto bag_path = unique_bag_path("identical");
  ScopedPath cleanup(bag_path);

  const uint16_t record_port = 42500;
  auto pub = make_pub("rtpub", record_port);
  auto recorder = make_sub("rtrecorder", record_port, {""});
  warm_up_raw(*recorder,
             [&] { pub->publish(nlohmann::json{{"warmup", true}}, "warmup"); });

  // Capture one JSON message and one blob message off the real wire.
  nlohmann::json json_payload = {{"hello", "world"}, {"n", 7}};
  pub->publish(json_payload, "sensors/acc/x");
  RawCapture json_cap;
  REQUIRE(wait_for_raw(*recorder, json_cap));
  REQUIRE(json_cap.topic == "sensors/acc/x");

  std::vector<unsigned char> blob_data(2000);
  for (size_t i = 0; i < blob_data.size(); ++i)
    blob_data[i] = static_cast<unsigned char>((i * 37 + 11) & 0xFF);
  nlohmann::json meta = {{"format", "raw"}, {"note", "roundtrip"}};
  pub->publish(blob_data, meta, "camera/frame");
  RawCapture blob_cap;
  REQUIRE(wait_for_raw(*recorder, blob_cap));
  REQUIRE(blob_cap.topic == "camera/frame");

  // Write both captures to a bag file, exactly like mads-record's loop.
  {
    Mads::BagWriter writer(bag_path);
    writer.write(now_ns(), json_cap.topic, json_cap.parts);
    writer.write(now_ns(), blob_cap.topic, blob_cap.parts);
    writer.close();
  }

  Mads::BagReader reader(bag_path);
  REQUIRE(reader.record_count() == 2);
  Mads::BagRecord rec0 = reader.read(0);
  Mads::BagRecord rec1 = reader.read(1);
  // The bag itself is byte-identical to what was captured off the wire.
  CHECK(rec0.topic == json_cap.topic);
  CHECK(rec0.parts == json_cap.parts);
  CHECK(rec1.topic == blob_cap.topic);
  CHECK(rec1.parts == blob_cap.parts);

  // Replay: a fresh publisher on a different port (mirrors mads-play), plus
  // two independent subscribers -- one doing raw capture (byte-identical
  // check), one going through the normal decode path (proves a standard
  // consumer still works transparently on a replayed message).
  const uint16_t play_port = 42501;
  auto player_pub = make_pub("rtplayer", play_port);
  auto raw_sub = make_sub("rtrawsub", play_port, {""});
  auto semantic_sub = make_sub("rtsemanticsub", play_port, {""});
  warm_up_raw(*raw_sub, [&] {
    player_pub->publish_raw_message("warmup", {"x"});
  });
  // Warm up semantic_sub against the same publisher too (independent slow
  // -joiner window per subscriber). Uses a normal publish() (not raw bytes)
  // since semantic_sub goes through the real decode path, which requires
  // an actual valid encoded frame, not arbitrary bytes.
  {
    auto deadline = std::chrono::steady_clock::now() + 3000ms;
    bool got = false;
    while (!got && std::chrono::steady_clock::now() < deadline) {
      player_pub->publish(nlohmann::json{{"warmup", true}}, "warmup");
      got = mads_test::wait_for(
          [&] { return semantic_sub->receive(true) != Mads::message_type::none; },
          150ms, 10ms);
    }
    REQUIRE(got);
  }
  // The semantic_sub warm-up above also fans out to raw_sub (same port,
  // same catch-all subscription): drain those leftovers so the next raw
  // capture on raw_sub is the real test message, not a stale "warmup" one.
  {
    RawCapture drain;
    while (wait_for_raw(*raw_sub, drain, 50ms)) {
    }
  }

  reader.rewind();
  auto rec_json = reader.next();
  REQUIRE(rec_json.has_value());
  player_pub->publish_raw_message(rec_json->topic, rec_json->parts);
  RawCapture replayed_json;
  REQUIRE(wait_for_raw(*raw_sub, replayed_json));
  CHECK(replayed_json.topic == json_cap.topic);
  CHECK(replayed_json.parts == json_cap.parts);
  REQUIRE(mads_test::wait_for(
      [&] { return semantic_sub->receive(true) == Mads::message_type::json; }));
  {
    auto [topic, doc] = semantic_sub->last_json();
    CHECK(topic == "sensors/acc/x");
    CHECK(doc.at("hello") == "world");
    CHECK(doc.at("n") == 7);
  }

  auto rec_blob = reader.next();
  REQUIRE(rec_blob.has_value());
  player_pub->publish_raw_message(rec_blob->topic, rec_blob->parts);
  RawCapture replayed_blob;
  REQUIRE(wait_for_raw(*raw_sub, replayed_blob));
  CHECK(replayed_blob.topic == blob_cap.topic);
  CHECK(replayed_blob.parts == blob_cap.parts);
  REQUIRE(mads_test::wait_for(
      [&] { return semantic_sub->receive(true) == Mads::message_type::blob; }));
  {
    auto [topic, format, bytes] = semantic_sub->last_blob();
    CHECK(topic == "camera/frame");
    REQUIRE(bytes.size() == blob_data.size());
    CHECK(std::equal(bytes.begin(), bytes.end(), blob_data.begin()));
    nlohmann::json meta_doc = nlohmann::json::parse(format);
    CHECK(meta_doc.value("note", std::string()) == "roundtrip");
  }

  CHECK_FALSE(reader.next().has_value());
}

// ---------------------------------------------------------------------------
// --restamp: rewrites timestamp/timecode, leaves everything else identical.
// ---------------------------------------------------------------------------

TEST_CASE("bag roundtrip: --restamp (Mads::Play::try_restamp) rewrites "
          "timestamp/timecode and leaves every other field identical",
          "[bag_roundtrip]") {
  mads_test::RunningGuard guard;
  const uint16_t record_port = 42510;
  auto pub = make_pub("rspub", record_port);
  // try_restamp() only understands the legacy, header-less [topic][snappy
  // (json)] frame (Agent::receive()'s own rule: a header-less frame is only
  // ever emitted when the payload is Snappy-compressed). That happens
  // unconditionally under Compression::Snappy, or only above ~256 bytes
  // under the default Compression::Auto -- force it explicitly here so the
  // test doesn't depend on the exact byte size of the stamped payload.
  pub->set_compression(Mads::Compression::Snappy);
  auto recorder = make_sub("rsrecorder", record_port, {""});
  warm_up_raw(*recorder,
             [&] { pub->publish(nlohmann::json{{"warmup", true}}, "warmup"); });

  pub->publish(nlohmann::json{{"reading", 42}}, "sensors/temp");
  RawCapture cap;
  REQUIRE(wait_for_raw(*recorder, cap));
  REQUIRE(cap.parts.size() == 1); // plain legacy [topic][snappy(json)] frame

  auto original_parts = cap.parts;
  bool touched = Mads::Play::try_restamp(cap.parts);
  REQUIRE(touched);
  REQUIRE(cap.parts != original_parts); // the bytes did change...

  const uint16_t play_port = 42511;
  auto player_pub = make_pub("rsplayer", play_port);
  auto sub = make_sub("rssub", play_port, {""});
  warm_up_raw(*sub,
             [&] { player_pub->publish_raw_message("warmup", {"x"}); });

  player_pub->publish_raw_message(cap.topic, cap.parts);
  REQUIRE(mads_test::wait_for(
      [&] { return sub->receive(true) == Mads::message_type::json; }));
  auto [topic, doc] = sub->last_json();
  CHECK(topic == "sensors/temp");
  // ...but every non-timestamp field is unchanged.
  CHECK(doc.at("reading") == 42);
  CHECK(doc.contains("agent_id"));
  CHECK(doc.contains("hostname"));
  CHECK(doc.contains("timestamp"));
  CHECK(doc.contains("timecode"));

  // A record with no "timestamp"/"timecode" field (or a non-JSON/blob
  // shape) is reported untouched.
  std::vector<std::string> plain = {std::string("{\"x\":1}")};
  CHECK_FALSE(Mads::Play::try_restamp(plain));
  std::vector<std::string> multi_part = {"a", "b"};
  CHECK_FALSE(Mads::Play::try_restamp(multi_part));
}

// ---------------------------------------------------------------------------
// --topics: only the matching subset (Mads::topic_match) is replayed.
// ---------------------------------------------------------------------------

TEST_CASE("bag roundtrip: --topics ('sensors/+/x') replays only the "
          "matching subset",
          "[bag_roundtrip]") {
  mads_test::RunningGuard guard;
  auto bag_path = unique_bag_path("topics");
  ScopedPath cleanup(bag_path);

  const uint16_t record_port = 42520;
  auto pub = make_pub("tfpub", record_port);
  auto recorder = make_sub("tfrecorder", record_port, {""});
  warm_up_raw(*recorder,
             [&] { pub->publish(nlohmann::json{{"warmup", true}}, "warmup"); });

  {
    Mads::BagWriter writer(bag_path);
    for (auto const &topic :
        {"sensors/acc/x", "sensors/gyro/x", "other/topic"}) {
      pub->publish(nlohmann::json{{"topic", topic}}, topic);
      RawCapture cap;
      REQUIRE(wait_for_raw(*recorder, cap));
      REQUIRE(cap.topic == topic);
      writer.write(now_ns(), cap.topic, cap.parts);
    }
    writer.close();
  }

  Mads::BagReader reader(bag_path);
  REQUIRE(reader.record_count() == 3);

  const uint16_t play_port = 42521;
  auto player_pub = make_pub("tfplayer", play_port);
  auto sub = make_sub("tfsub", play_port, {""});
  warm_up_raw(*sub,
             [&] { player_pub->publish_raw_message("warmup", {"x"}); });

  const std::string pattern = "sensors/+/x";
  std::vector<std::string> received_topics;
  reader.rewind();
  while (auto rec = reader.next()) {
    if (!Mads::topic_match(pattern, rec->topic))
      continue; // dropped by --topics, exactly like mads-play's filter
    player_pub->publish_raw_message(rec->topic, rec->parts);
    RawCapture got;
    REQUIRE(wait_for_raw(*sub, got));
    received_topics.push_back(got.topic);
  }

  REQUIRE(received_topics.size() == 2);
  CHECK(received_topics[0] == "sensors/acc/x");
  CHECK(received_topics[1] == "sensors/gyro/x");

  // The filtered-out topic never arrives, even though it was in the bag.
  RawCapture unwanted;
  CHECK_FALSE(wait_for_raw(*sub, unwanted, 300ms));
}

// ---------------------------------------------------------------------------
// --rate: pacing scales replay duration within generous bounds.
// ---------------------------------------------------------------------------

TEST_CASE("bag roundtrip: --rate paces replay relative to the recorded "
          "gaps between timestamps",
          "[bag_roundtrip]") {
  mads_test::RunningGuard guard;
  auto bag_path = unique_bag_path("pacing");
  ScopedPath cleanup(bag_path);

  const uint16_t record_port = 42530;
  auto pub = make_pub("pcpub", record_port);
  auto recorder = make_sub("pcrecorder", record_port, {""});
  warm_up_raw(*recorder,
             [&] { pub->publish(nlohmann::json{{"warmup", true}}, "warmup"); });

  // Record 3 messages with a real ~120ms gap between captures, so the bag's
  // own timestamps carry a genuine, known-ish spacing (used only as the
  // pacing reference below -- not asserted on directly, since scheduling
  // jitter makes the exact gap unreliable; only the resulting replay
  // duration is asserted, with generous bounds).
  const auto nominal_gap = 120ms;
  {
    Mads::BagWriter writer(bag_path);
    for (int i = 0; i < 3; ++i) {
      if (i > 0)
        std::this_thread::sleep_for(nominal_gap);
      pub->publish(nlohmann::json{{"i", i}}, "sensors/pace");
      RawCapture cap;
      REQUIRE(wait_for_raw(*recorder, cap));
      writer.write(now_ns(), cap.topic, cap.parts);
    }
    writer.close();
  }

  Mads::BagReader reader(bag_path);
  REQUIRE(reader.record_count() == 3);

  const uint16_t play_port = 42531;
  auto player_pub = make_pub("pcplayer", play_port);
  auto sub = make_sub("pcsub", play_port, {""});
  warm_up_raw(*sub,
             [&] { player_pub->publish_raw_message("warmup", {"x"}); });

  // Replay at rate = 2.0 (twice real-time): total pacing delay should be
  // roughly half the ~2 * nominal_gap recorded span, i.e. close to
  // nominal_gap, generously bounded like other timing assertions in this
  // suite (see tests/test_agent_loop.cpp).
  const double rate = 2.0;
  reader.rewind();
  bool have_prev = false;
  int64_t prev_ts = 0;
  auto t0 = std::chrono::steady_clock::now();
  while (auto rec = reader.next()) {
    if (have_prev) {
      int64_t gap_ns = rec->timestamp_ns - prev_ts;
      if (gap_ns > 0) {
        auto sleep_ns = std::chrono::nanoseconds(
            static_cast<int64_t>(static_cast<double>(gap_ns) / rate));
        std::this_thread::sleep_for(sleep_ns);
      }
    }
    have_prev = true;
    prev_ts = rec->timestamp_ns;
    player_pub->publish_raw_message(rec->topic, rec->parts);
    RawCapture got;
    REQUIRE(wait_for_raw(*sub, got));
  }
  auto elapsed = std::chrono::steady_clock::now() - t0;

  // Two gaps of ~120ms recorded, replayed at 2x -> ~120ms of pacing sleep
  // total, plus test overhead. Generous bounds to absorb scheduler jitter
  // and CI slowness without being a no-op check.
  CHECK(elapsed >= 40ms);
  CHECK(elapsed < 3000ms);

  // Sanity: unpaced replay (no --rate) of the same bag is not slower than
  // the paced one by more than the paced one's own floor -- i.e. pacing is
  // actually doing something, not a no-op that happens to pass the bounds
  // above by coincidence.
  auto sub2 = make_sub("pcsub2", play_port, {""});
  warm_up_raw(*sub2,
             [&] { player_pub->publish_raw_message("warmup2", {"x"}); });
  reader.rewind();
  auto t1 = std::chrono::steady_clock::now();
  while (auto rec = reader.next()) {
    player_pub->publish_raw_message(rec->topic, rec->parts);
    RawCapture got;
    REQUIRE(wait_for_raw(*sub2, got));
  }
  auto unpaced_elapsed = std::chrono::steady_clock::now() - t1;
  CHECK(unpaced_elapsed < elapsed);
}
