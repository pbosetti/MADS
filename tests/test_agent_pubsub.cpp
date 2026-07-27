// Unit tests for Mads::Agent pub/sub messaging (src/agent.cpp, src/agent.hpp)
// over an in-process ZeroMQ loopback pair (no broker).
//
// Topology used throughout: a "publisher" agent is initialized with
// settings_uri "none" (test mode, src/agent.hpp:232) and set_cross(true),
// which makes connect_pub() BIND using the port carried by _sub_endpoint
// (src/agent.cpp Agent::connect_pub). A plain (non-cross) "subscriber" agent
// then CONNECTs its _sub_endpoint to that same tcp://127.0.0.1:<port>. Since
// init() overwrites _pub_endpoint/_sub_endpoint with defaults
// (src/agent.cpp:392-393), endpoint setters are always called AFTER init()
// and BEFORE connect().
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent.hpp"
#include "mads_test_helpers.hpp"

using namespace std::chrono_literals;

namespace {

// Publisher: binds (cross=true) at loopback(port), publishing only.
std::unique_ptr<Mads::Agent> make_pub(std::string name, uint16_t port) {
  auto a = std::make_unique<Mads::Agent>(name, "none");
  a->init(false, false); // crypto=false, install_watchdog=false
  a->set_cross(true);
  a->set_sub_endpoint(mads_test::loopback(port));
  a->set_sub_topic({}); // pure publisher: skip connect_sub entirely
  a->connect(std::chrono::milliseconds(0));
  return a;
}

// Subscriber: connects to loopback(port), subscribing only.
std::unique_ptr<Mads::Agent> make_sub(std::string name, uint16_t port,
                                      std::vector<std::string> topics = {""}) {
  auto a = std::make_unique<Mads::Agent>(name, "none");
  a->init(false, false);
  a->set_sub_endpoint(mads_test::loopback(port));
  a->set_pub_topic("");     // pure subscriber: skip connect_pub entirely
  a->set_sub_topic(topics);
  a->connect(std::chrono::milliseconds(0));
  return a;
}

// Repeatedly invokes `publish_once` (re-publishing) until `check_received`
// becomes true, absorbing the ZMQ "slow joiner" propagation delay without a
// single fixed sleep as the sole synchronization mechanism.
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

} // namespace

// ---------------------------------------------------------------------------
// publish(json) / receive()
// ---------------------------------------------------------------------------

TEST_CASE("publish(json) delivers message_type::json with matching topic, "
          "json and message content",
          "[agent_pubsub]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42100;
  auto pub = make_pub("pubA", port);
  auto sub = make_sub("subA", port);

  nlohmann::json payload = {{"hello", "world"}, {"n", 7}};
  bool got = retry_until(
      [&] { pub->publish(payload, "topicA"); },
      [&] { return sub->receive(true) == Mads::message_type::json; });
  REQUIRE(got);

  auto [topic, doc] = sub->last_json();
  REQUIRE(topic == "topicA");
  REQUIRE(doc.at("hello") == "world");
  REQUIRE(doc.at("n") == 7);
  // Agent::publish() auto-stamps these fields (src/agent.cpp:748-759).
  REQUIRE(doc.contains("timestamp"));
  REQUIRE(doc.contains("timecode"));
  REQUIRE(doc.contains("agent_id"));
  REQUIRE(doc.contains("hostname"));

  auto [topic2, text2] = sub->last_message();
  REQUIRE(topic2 == "topicA");
  REQUIRE(nlohmann::json::parse(text2).at("n") == 7);

  REQUIRE(sub->last_topic() == "topicA");
}

// ---------------------------------------------------------------------------
// publish(blob) overloads / receive()
// ---------------------------------------------------------------------------

TEST_CASE("publish(const char*, len, meta) delivers message_type::blob with "
          "matching bytes and metadata",
          "[agent_pubsub]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42101;
  auto pub = make_pub("pubB1", port);
  auto sub = make_sub("subB1", port);

  std::string data = "hello blob";
  nlohmann::json meta = {{"format", "raw"}, {"note", "test"}};

  bool got = retry_until(
      [&] { pub->publish(data.data(), data.size(), meta, "blobtopic"); },
      [&] { return sub->receive(true) == Mads::message_type::blob; });
  REQUIRE(got);

  auto [topic, meta_text, bytes] = sub->last_blob();
  REQUIRE(topic == "blobtopic");
  REQUIRE(std::string(bytes.begin(), bytes.end()) == data);
  nlohmann::json meta_doc = nlohmann::json::parse(meta_text);
  REQUIRE(meta_doc.value("note", std::string()) == "test");

  auto [tv, fv, bv] = sub->last_blob_view();
  REQUIRE(tv == "blobtopic");
  REQUIRE(std::string(bv.begin(), bv.end()) == data);
}

TEST_CASE("publish(vector<unsigned char>, meta) delivers message_type::blob "
          "with matching bytes",
          "[agent_pubsub]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42102;
  auto pub = make_pub("pubB2", port);
  auto sub = make_sub("subB2", port);

  std::vector<unsigned char> data = {1, 2, 3, 4, 5, 250, 251, 252};
  nlohmann::json meta = {{"format", "raw"}};

  bool got = retry_until(
      [&] { pub->publish(data, meta, "vectopic"); },
      [&] { return sub->receive(true) == Mads::message_type::blob; });
  REQUIRE(got);

  auto [topic, meta_text, bytes] = sub->last_blob();
  REQUIRE(topic == "vectopic");
  REQUIRE(bytes == data);
}

// ---------------------------------------------------------------------------
// receive() timeout path
// ---------------------------------------------------------------------------

TEST_CASE("receive() returns message_type::none when nothing arrives before "
          "the receive timeout",
          "[agent_pubsub]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42103;
  // A lone, connected subscriber: ZMQ connect() is async, so no publisher
  // needs to exist for the socket to reach the connected state.
  auto sub = make_sub("subC", port);
  sub->set_receive_timeout(150);
  REQUIRE(sub->receive_timeout() == 150);

  auto start = std::chrono::steady_clock::now();
  auto mt = sub->receive(); // blocking, bounded by the 150ms timeout
  auto elapsed = std::chrono::steady_clock::now() - start;

  REQUIRE(mt == Mads::message_type::none);
  REQUIRE(elapsed < 2000ms);
}

// ---------------------------------------------------------------------------
// Topic filtering
// ---------------------------------------------------------------------------

TEST_CASE("a subscriber subscribed to one topic never receives a different "
          "topic",
          "[agent_pubsub]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42104;
  auto pub = make_pub("pubE", port);
  auto sub = make_sub("subE", port, {"A"});

  bool got_a = retry_until(
      [&] { pub->publish(nlohmann::json{{"tag", "a"}}, "A"); },
      [&] { return sub->receive(true) == Mads::message_type::json; });
  REQUIRE(got_a);
  REQUIRE(std::get<0>(sub->last_message()) == "A");

  // Drain any leftover queued "A" messages from the retry warm-up above.
  while (sub->receive(true) == Mads::message_type::json) {
  }

  pub->publish(nlohmann::json{{"tag", "b"}}, "B");
  std::this_thread::sleep_for(300ms); // allow time for an (unwanted) delivery
  REQUIRE(sub->receive(true) == Mads::message_type::none);
  REQUIRE(std::get<0>(sub->last_message()) == "A"); // unchanged by topic B
}

// ---------------------------------------------------------------------------
// P2: MQTT-style wildcard topic filtering (src/topic_match.hpp/.cpp wired
// into src/agent.cpp's subscribe path). literal_prefix("sensors/+/x") ==
// "sensors/", so the raw ZMQ SUBSCRIBE is broader than the pattern: this
// proves the two-stage filtering -- not just ZMQ's own byte-prefix match --
// actually runs end to end over a real loopback socket.
// ---------------------------------------------------------------------------

TEST_CASE("a wildcard ('+') sub_topic passes matching topics through and "
          "silently suppresses non-matching ones under the same ZMQ prefix",
          "[agent_pubsub][topic_match]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42110;
  auto pub = make_pub("pubH", port);
  auto sub = make_sub("subH", port, {"sensors/+/x"});

  // Matching: single wildcard level, last level "x" matches literally.
  bool got = retry_until(
      [&] { pub->publish(nlohmann::json{{"v", 1}}, "sensors/acc/x"); },
      [&] { return sub->receive(true) == Mads::message_type::json; });
  REQUIRE(got);
  REQUIRE(std::get<0>(sub->last_message()) == "sensors/acc/x");

  // Drain any leftover queued matches from the retry warm-up above.
  while (sub->receive(true) == Mads::message_type::json) {
  }

  // Non-matching: same ZMQ-level prefix ("sensors/"), different last level,
  // so ZMQ itself would deliver it -- it must be dropped by topic_match().
  pub->publish(nlohmann::json{{"v", 2}}, "sensors/acc/y");
  std::this_thread::sleep_for(300ms); // allow time for an (unwanted) delivery
  REQUIRE(sub->receive(true) == Mads::message_type::none);
  REQUIRE(std::get<0>(sub->last_message()) ==
          "sensors/acc/x"); // unchanged by the suppressed message

  // Matching again, with a different value at the wildcard level.
  bool got2 = retry_until(
      [&] { pub->publish(nlohmann::json{{"v", 3}}, "sensors/gyro/x"); },
      [&] { return sub->receive(true) == Mads::message_type::json; });
  REQUIRE(got2);
  REQUIRE(std::get<0>(sub->last_message()) == "sensors/gyro/x");
}

TEST_CASE("a wildcard ('#') sub_topic matches the parent level itself and "
          "every level below it",
          "[agent_pubsub][topic_match]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42111;
  auto pub = make_pub("pubI", port);
  auto sub = make_sub("subI", port, {"sensors/#"});

  // "#" matches the bare parent topic "sensors" itself (MQTT quirk).
  bool got_parent = retry_until(
      [&] { pub->publish(nlohmann::json{{"v", 1}}, "sensors"); },
      [&] { return sub->receive(true) == Mads::message_type::json; });
  REQUIRE(got_parent);
  REQUIRE(std::get<0>(sub->last_message()) == "sensors");

  while (sub->receive(true) == Mads::message_type::json) {
  }

  // ...and everything nested below it, at any depth.
  bool got_deep = retry_until(
      [&] { pub->publish(nlohmann::json{{"v", 2}}, "sensors/acc/x/y"); },
      [&] { return sub->receive(true) == Mads::message_type::json; });
  REQUIRE(got_deep);
  REQUIRE(std::get<0>(sub->last_message()) == "sensors/acc/x/y");
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

TEST_CASE("accessors: name/status/agent_id/topics/endpoints/is_connected/"
          "info",
          "[agent_pubsub]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42105;
  auto pub = make_pub("pubF", port);
  auto sub = make_sub("subF", port);

  REQUIRE(pub->name() == "pubF");
  REQUIRE(sub->name() == "subF");
  REQUIRE(pub->is_connected());
  REQUIRE(sub->is_connected());

  sub->set_agent_id("agent-123");
  REQUIRE(sub->get_agent_id() == "agent-123");

  sub->set_sub_topic({"foo", "bar"});
  REQUIRE(sub->sub_topic() == std::vector<std::string>{"foo", "bar"});

  pub->set_pub_topic("mytopic");
  REQUIRE(pub->pub_topic() == "mytopic");

  REQUIRE(sub->sub_endpoint() == mads_test::loopback(port));
  // In cross mode, connect_pub() rewrites _sub_endpoint into its bind form.
  REQUIRE(pub->sub_endpoint().rfind("tcp://*:", 0) == 0);

  bool got = retry_until(
      [&] { pub->publish(nlohmann::json{{"v", 1}}, "status_topic"); },
      [&] { return sub->receive(true) == Mads::message_type::json; });
  REQUIRE(got);
  auto st = sub->status();
  REQUIRE(st.count("status_topic") == 1);
  REQUIRE(nlohmann::json::parse(st.at("status_topic")).at("v") == 1);

  std::ostringstream oss;
  sub->info(oss);
  REQUIRE_FALSE(oss.str().empty());
  REQUIRE(oss.str().find("subF") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Error paths
// ---------------------------------------------------------------------------

TEST_CASE("operations on an uninitialized agent throw AgentError",
          "[agent_pubsub]") {
  mads_test::RunningGuard guard;
  Mads::Agent a("uninit", "none");
  REQUIRE_THROWS_AS(a.connect(), Mads::AgentError);
  REQUIRE_THROWS_AS(a.publish(nlohmann::json{{"x", 1}}), Mads::AgentError);
  REQUIRE_THROWS_AS(a.receive(), Mads::AgentError);
  REQUIRE_THROWS_AS(a.disconnect(), Mads::AgentError);
  REQUIRE_THROWS_AS(a.enable_remote_control(), Mads::AgentError);
  REQUIRE_THROWS_AS(a.remote_control("{}"), Mads::AgentError);
  REQUIRE_THROWS_AS(a.register_event(), Mads::AgentError);
  REQUIRE_THROWS_AS(a.save_settings(), Mads::AgentError);
  std::ostringstream oss;
  REQUIRE_THROWS_AS(a.info(oss), Mads::AgentError);
}

TEST_CASE("set_settings_timeout() throws AgentError once the agent is "
          "initialized",
          "[agent_pubsub]") {
  mads_test::RunningGuard guard;
  Mads::Agent a("guardB", "none");
  a.init(false, false);
  REQUIRE_THROWS_AS(a.set_settings_timeout(100), Mads::AgentError);
  REQUIRE_THROWS_AS(a.set_settings_timeout(std::chrono::milliseconds(100)),
                    Mads::AgentError);
}

TEST_CASE("set_delivery/set_high_watermark/enable_remote_control throw "
          "AgentError once connected",
          "[agent_pubsub]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42106;
  auto a = std::make_unique<Mads::Agent>("guardA", "none");
  a->init(false, false);
  a->set_sub_endpoint(mads_test::loopback(port));
  a->set_pub_topic("");
  a->set_sub_topic({""});
  a->connect(std::chrono::milliseconds(0)); // lone connect, no peer required

  REQUIRE(a->is_connected());
  REQUIRE_THROWS_AS(a->set_delivery(Mads::Delivery::LastKnownValue),
                    Mads::AgentError);
  REQUIRE_THROWS_AS(a->set_high_watermark(10), Mads::AgentError);
  REQUIRE_THROWS_AS(a->enable_remote_control(), Mads::AgentError);
}

TEST_CASE("receive() throws AgentError while threaded remote control owns "
          "the subscriber socket",
          "[agent_pubsub]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42107;
  auto a = std::make_unique<Mads::Agent>("guardC", "none");
  a->init(false, false);
  a->set_sub_endpoint(mads_test::loopback(port));
  a->set_pub_topic("");
  // Must be called before connect(): it pushes "control" onto _sub_topic and
  // (threaded=true) hands the subscriber socket to a background thread.
  a->enable_remote_control(true);
  a->connect(std::chrono::milliseconds(0));
  REQUIRE(a->is_connected());
  REQUIRE_THROWS_AS(a->receive(), Mads::AgentError);
}

// ---------------------------------------------------------------------------
// disconnect() / shutdown() safety
// ---------------------------------------------------------------------------

TEST_CASE("disconnect() and shutdown() are safe to call repeatedly",
          "[agent_pubsub]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42108;
  auto a = std::make_unique<Mads::Agent>("guardD", "none");
  a->init(false, false);
  a->set_sub_endpoint(mads_test::loopback(port));
  a->set_pub_topic("");
  a->set_sub_topic({""});
  a->connect(std::chrono::milliseconds(0));

  REQUIRE(a->is_connected());
  a->disconnect();
  REQUIRE_FALSE(a->is_connected());
  a->disconnect(); // no-op the second time, must not throw
  REQUIRE_FALSE(a->is_connected());

  a->shutdown();
  a->shutdown(); // idempotent, guarded by _shutdown_done
  SUCCEED("disconnect()/shutdown() tolerated repeated calls");
}

// ---------------------------------------------------------------------------
// Delivery::LastKnownValue
// ---------------------------------------------------------------------------

TEST_CASE("Delivery::LastKnownValue drains a burst of messages down to the "
          "latest value",
          "[agent_pubsub]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42109;
  auto pub = make_pub("pubG", port);

  auto sub = std::make_unique<Mads::Agent>("subG", "none");
  sub->init(false, false);
  sub->set_sub_endpoint(mads_test::loopback(port));
  sub->set_pub_topic("");
  sub->set_sub_topic({""});
  sub->set_delivery(Mads::Delivery::LastKnownValue);
  REQUIRE(sub->delivery() == Mads::Delivery::LastKnownValue);
  sub->connect(std::chrono::milliseconds(0));

  // Warm up the pair first (slow-joiner absorption).
  bool warm = retry_until(
      [&] { pub->publish(nlohmann::json{{"idx", 0}}, "lkv"); },
      [&] { return sub->receive(true) == Mads::message_type::json; });
  REQUIRE(warm);

  // Publish a fast burst; the background drain thread (src/agent.cpp
  // Agent::connect_sub) should collapse it down to the last value.
  for (int i = 1; i <= 5; ++i) {
    pub->publish(nlohmann::json{{"idx", i}}, "lkv");
  }

  bool ok = mads_test::wait_for(
      [&] {
        auto mt = sub->receive(true);
        if (mt != Mads::message_type::json)
          return false;
        auto [topic, doc] = sub->last_json();
        return doc.value("idx", -1) == 5;
      },
      3000ms, 20ms);
  REQUIRE(ok);
}
