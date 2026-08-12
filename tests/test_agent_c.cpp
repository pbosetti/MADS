// Unit tests for the pure-C ABI wrapper around Mads::Agent (src/agent_c.h,
// src/agent_c.cpp). Exercised from a C++ translation unit (agent_c.h wraps
// its declarations in `extern "C"`), which lets us also reach into
// agent.hpp/mads.hpp for two purposes:
//   - assembling settings fixtures / verifying results against the same
//     enums the C ABI mirrors (message_type, WireFormat, Compression,
//     event_type all have fixed numeric values matched by the C header).
//   - reaching Mads::Agent methods that have no C ABI equivalent (set_cross,
//     set_sub_endpoint, set_pub_endpoint) to pair up two C-created agents for
//     a loopback pub/sub round trip, exactly like test_agent_pubsub.cpp does
//     for the C++ API. agent_t is a type-erased Agent*, so
//     reinterpret_cast<Mads::Agent*>(handle) refers to the very same object
//     the C API is operating on.
//
// Port range for this file: 42300-42399 (WP-D).
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <chrono>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include "zap_auth.hpp"

#include "agent.hpp"
#include "agent_c.h"
#include "mads_test_helpers.hpp"

using namespace std::chrono_literals;

namespace {

std::string fixture(const std::string &rel) {
  return std::string(MADS_TEST_FIXTURES_DIR) + "/" + rel;
}

// Repeatedly (re-)publishes until check_received becomes true, absorbing the
// ZMQ "slow joiner" propagation delay. Mirrors the helper used in
// test_agent_pubsub.cpp, kept local since we may not edit that file.
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
// Free functions
// ---------------------------------------------------------------------------

TEST_CASE("mads_version/mads_default_settings_uri return stable, non-empty "
          "strings",
          "[agent_c]") {
  const char *v = mads_version();
  REQUIRE(v != nullptr);
  REQUIRE(std::string(v).size() > 0);

  const char *uri = mads_default_settings_uri();
  REQUIRE(uri != nullptr);
  // Just check it is a stable pointer to a sane string, content is whatever
  // was compiled in.
  REQUIRE(std::string(uri) == std::string(mads_default_settings_uri()));
}

TEST_CASE("mads_free tolerates a NULL pointer", "[agent_c]") {
  mads_free(nullptr); // free(NULL) is well-defined; must not crash
  SUCCEED();
}

// ---------------------------------------------------------------------------
// create / init / destroy lifecycle, accessor sweep
// ---------------------------------------------------------------------------

TEST_CASE("agent_create -> agent_init(\"none\") -> full accessor sweep -> "
          "agent_destroy",
          "[agent_c]") {
  mads_test::RunningGuard guard;
  agent_t a = agent_create("cabsweep", "none");
  REQUIRE(a != nullptr);
  REQUIRE(agent_init(a, false) == 0);

  // id
  agent_set_id(a, "the-id-1");
  REQUIRE(std::string(agent_id(a)) == "the-id-1");

  // pub topic
  agent_set_pub_topic(a, "mytopic");
  REQUIRE(std::string(agent_pub_topic(a)) == "mytopic");

  // sub topics: set two, read back via the malloc-on-demand convention.
  const char *topics_in[] = {"topicA", "topicB"};
  agent_set_sub_topics(a, topics_in, 2);
  char *topics_out = nullptr;
  size_t n_topics = 0;
  int rc = agent_sub_topics(a, &topics_out, &n_topics);
  REQUIRE(rc == 2);
  REQUIRE(n_topics == 2);
  REQUIRE(std::string(&topics_out[0 * 256]) == "topicA");
  REQUIRE(std::string(&topics_out[1 * 256]) == "topicB");
  mads_free(topics_out);

  // agent_topics() JSON dump
  char *topics_json = agent_topics(a, 0);
  REQUIRE(topics_json != nullptr);
  nlohmann::json tj = nlohmann::json::parse(std::string(topics_json));
  REQUIRE(tj.at("publish") == "mytopic");
  REQUIRE(tj.at("subscribe") == nlohmann::json::array({"topicA", "topicB"}));

  // receive timeout
  agent_set_receive_timeout(a, 321);
  REQUIRE(agent_receive_timeout(a) == 321);

  // high watermark
  REQUIRE(agent_set_high_watermark(a, 55) == 0);
  REQUIRE(agent_high_watermark(a) == 55);

  // wire format
  REQUIRE(agent_wire_format(a) == 0); // default Json
  REQUIRE(agent_set_wire_format(a, 1) == 0);
  REQUIRE(agent_wire_format(a) == 1);
  REQUIRE(agent_set_wire_format(a, 42) == -1); // invalid
  REQUIRE(std::string(agent_last_error()).size() > 0);

  // compression
  REQUIRE(agent_compression(a) == 2); // default Auto
  REQUIRE(agent_set_compression(a, 0) == 0);
  REQUIRE(agent_compression(a) == 0);
  REQUIRE(agent_set_compression(a, 99) == -1); // invalid
  REQUIRE(std::string(agent_last_error()).size() > 0);

  // settings uri / settings dump / print
  REQUIRE(std::string(agent_settings_uri(a)) == "none");
  const char *settings_json = agent_get_settings(a, 2);
  REQUIRE(settings_json != nullptr);
  nlohmann::json sj = nlohmann::json::parse(std::string(settings_json));
  REQUIRE(sj.contains("pub_topic"));
  agent_print_settings(a, 0); // just must not crash; output goes to stdout

  // loop watchdog: install and let destroy() join it cleanly.
  agent_install_loop_watchdog(a);

  agent_destroy(a);
  SUCCEED("agent_destroy completed without hanging or crashing");
}

// ---------------------------------------------------------------------------
// agent_set_sub_topics with zero topics (used to build a pure publisher).
// ---------------------------------------------------------------------------

TEST_CASE("agent_set_sub_topics(0 topics) clears the subscribe list",
          "[agent_c]") {
  mads_test::RunningGuard guard;
  agent_t a = agent_create("cabnosub", "none");
  REQUIRE(agent_init(a, false) == 0);
  agent_set_sub_topics(a, nullptr, 0);
  char *topics_out = nullptr;
  size_t n_topics = 0;
  int rc = agent_sub_topics(a, &topics_out, &n_topics);
  // sub_topic() defaults to [""] from init("none"); an explicit 0-count set
  // replaces it with an actually-empty vector.
  REQUIRE(rc == 0);
  REQUIRE(n_topics == 0);
  // malloc(0) is implementation-defined (NULL or a free()-able unique
  // pointer); mads_free() tolerates either.
  mads_free(topics_out);
  agent_destroy(a);
}

// ---------------------------------------------------------------------------
// settings timeout: succeeds before init(), guarded after init()
// ---------------------------------------------------------------------------

TEST_CASE("agent_set_settings_timeout succeeds pre-init, fails post-init",
          "[agent_c]") {
  mads_test::RunningGuard guard;
  agent_t a = agent_create("cabtimeout", "none");
  REQUIRE(agent_set_settings_timeout(a, 500) == 0);
  REQUIRE(agent_settings_timeout(a) == 500);

  REQUIRE(agent_init(a, false) == 0);
  REQUIRE(agent_set_settings_timeout(a, 999) == -1);
  REQUIRE(std::string(agent_last_error()).size() > 0);
  // Value is unchanged since the setter threw before mutating state.
  REQUIRE(agent_settings_timeout(a) == 500);

  agent_destroy(a);
}

// ---------------------------------------------------------------------------
// Settings getters against a TOML fixture (read-only, from WP-C).
// ---------------------------------------------------------------------------

TEST_CASE("agent_setting_bool/int/dbl/str read values from a loaded TOML "
          "file, defaulting on missing keys",
          "[agent_c]") {
  mads_test::RunningGuard guard;
  agent_t a = agent_create("settingstest", fixture("settings/valid.toml").c_str());
  REQUIRE(agent_init(a, false) == 0);

  REQUIRE(agent_setting_bool(a, "custom_bool") == true);
  REQUIRE(agent_setting_int(a, "custom_int") == 42);
  REQUIRE(agent_setting_dbl(a, "custom_double") == Catch::Approx(3.14));
  REQUIRE(std::string(agent_setting_str(a, "custom_string")) ==
          "custom_value");

  // Missing keys default per documented behavior.
  REQUIRE(agent_setting_bool(a, "does_not_exist") == false);
  REQUIRE(agent_setting_int(a, "does_not_exist") == 0);
  REQUIRE(agent_setting_dbl(a, "does_not_exist") == Catch::Approx(0.0));
  REQUIRE(std::string(agent_setting_str(a, "does_not_exist")) == "");

  REQUIRE(std::string(agent_settings_uri(a)) == fixture("settings/valid.toml"));

  agent_destroy(a);
}

// ---------------------------------------------------------------------------
// Error paths: agent_init failures and agent_last_error()
// ---------------------------------------------------------------------------

TEST_CASE("agent_init fails against a nonexistent settings file and sets "
          "agent_last_error()",
          "[agent_c]") {
  mads_test::RunningGuard guard;
  agent_t a = agent_create("cabmissing", "/no/such/path/settings.toml");
  int rc = agent_init(a, false);
  REQUIRE(rc == -1);
  std::string err = agent_last_error();
  REQUIRE(err.find("Error initializing agent") != std::string::npos);
  agent_destroy(a);
}

TEST_CASE("agent_init fails against a malformed TOML file", "[agent_c]") {
  mads_test::RunningGuard guard;
  agent_t a = agent_create("cabmalformed", fixture("settings/malformed.toml").c_str());
  REQUIRE(agent_init(a, false) == -1);
  REQUIRE(std::string(agent_last_error()).size() > 0);
  agent_destroy(a);
}

TEST_CASE("agent_init fails when the settings file lacks the agent's "
          "section",
          "[agent_c]") {
  mads_test::RunningGuard guard;
  agent_t a =
      agent_create("cabnosection", fixture("settings/missing_section.toml").c_str());
  REQUIRE(agent_init(a, false) == -1);
  REQUIRE(std::string(agent_last_error()).find("Error initializing agent") !=
          std::string::npos);
  agent_destroy(a);
}

TEST_CASE("agent_init fails when the agent is already connected",
          "[agent_c]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42300;
  agent_t a = agent_create("cabreinit", "none");
  REQUIRE(agent_init(a, false) == 0);
  Mads::Agent *ag = reinterpret_cast<Mads::Agent *>(a);
  ag->set_sub_endpoint(mads_test::loopback(port));
  agent_set_pub_topic(a, "");
  const char *reinit_topics[] = {""};
  agent_set_sub_topics(a, reinit_topics, 1); // non-empty -> connect_sub() runs
  REQUIRE(agent_connect(a, 0) == 0);

  REQUIRE(agent_init(a, false) == -1);
  REQUIRE(std::string(agent_last_error()).find("already connected") !=
          std::string::npos);

  agent_destroy(a);
}

// ---------------------------------------------------------------------------
// connect / disconnect / register_event guarded states
// ---------------------------------------------------------------------------

TEST_CASE("agent_connect fails on double-connect, agent_disconnect is "
          "idempotent",
          "[agent_c]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42301;
  agent_t a = agent_create("cabconn", "none");
  REQUIRE(agent_init(a, false) == 0);
  Mads::Agent *ag = reinterpret_cast<Mads::Agent *>(a);
  ag->set_sub_endpoint(mads_test::loopback(port));
  agent_set_pub_topic(a, "");
  const char *conn_topics[] = {""};
  agent_set_sub_topics(a, conn_topics, 1); // non-empty -> connect_sub() runs

  REQUIRE(agent_connect(a, 0) == 0);
  REQUIRE(agent_connect(a, 0) == -1);
  REQUIRE(std::string(agent_last_error()).find("already connected") !=
          std::string::npos);

  REQUIRE(agent_disconnect(a) == 0);
  REQUIRE(agent_disconnect(a) == 0); // no-op the second time

  agent_destroy(a);
}

TEST_CASE("agent_connect reports the underlying AgentError when the agent "
          "was never initialized",
          "[agent_c]") {
  mads_test::RunningGuard guard;
  agent_t a = agent_create("cabuninit", "none"); // agent_init() never called
  REQUIRE(agent_connect(a, 0) == -1);
  REQUIRE(std::string(agent_last_error()).find("Agent not initialized") !=
          std::string::npos);
  agent_destroy(a);
}

TEST_CASE("agent_register_event fails before connect, succeeds after",
          "[agent_c]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42302;
  agent_t a = agent_create("cabevent", "none");
  REQUIRE(agent_init(a, false) == 0);

  // Not connected yet: guarded failure. Uses mads_marker (synchronous
  // publish, src/agent.cpp:708-711) rather than mads_startup/mads_shutdown,
  // which spawn a detached/joined worker thread carrying a raw `this` --
  // safe only if the Agent outlives that thread's delay, which a short-lived
  // test case cannot guarantee.
  REQUIRE(agent_register_event(a, mads_marker, nullptr) == -1);
  REQUIRE(std::string(agent_last_error()).find("not connected") !=
          std::string::npos);

  Mads::Agent *ag = reinterpret_cast<Mads::Agent *>(a);
  ag->set_sub_endpoint(mads_test::loopback(port));
  agent_set_pub_topic(a, "");
  const char *event_topics[] = {""};
  agent_set_sub_topics(a, event_topics, 1); // non-empty -> connect_sub() runs
  REQUIRE(agent_connect(a, 0) == 0);

  REQUIRE(agent_register_event(a, mads_marker, nullptr) == 0);
  REQUIRE(agent_register_event(a, mads_marker, "{\"note\":\"hi\"}") == 0);
  // Malformed JSON payload -> parse error caught and reported.
  REQUIRE(agent_register_event(a, mads_marker, "{not json") == -1);
  REQUIRE(std::string(agent_last_error()).size() > 0);

  agent_destroy(a);
}

// ---------------------------------------------------------------------------
// discover_broker_settings: parameter guards only (no real network activity)
// ---------------------------------------------------------------------------

TEST_CASE("discover_broker_settings rejects invalid output buffer "
          "parameters without touching the network",
          "[agent_c]") {
  REQUIRE(discover_broker_settings("room", nullptr, 0) == -1);
  REQUIRE(std::string(agent_last_error()).find("Invalid output buffer") !=
          std::string::npos);

  char fixed[4];
  char *fixed_ptr = fixed;
  REQUIRE(discover_broker_settings("room", &fixed_ptr, 0) == -1);
  REQUIRE(std::string(agent_last_error()).find("Invalid output buffer") !=
          std::string::npos);
}

// ---------------------------------------------------------------------------
// Crypto setup via the C ABI, string-based keys (no key files needed).
// ---------------------------------------------------------------------------

TEST_CASE("agent_set_client_public_key/secret_key/server_public_key succeed "
          "once agent_setup_crypto has run",
          "[agent_c]") {
  mads_test::RunningGuard guard;
  agent_t a = agent_create("cabcrypto1", "none");

  // ZMQ validates CURVE key length/encoding (32 raw bytes or 40-char Z85) as
  // soon as the socket option is set, so real keypairs are needed even
  // though this test never opens a network connection.
  Mads::CurveKeypair client_kp = Mads::generate_keypair();
  Mads::CurveKeypair server_kp = Mads::generate_keypair();

  // Before agent_setup_crypto() the CurveAuth is not initialized yet, so the
  // key setters must fail cleanly with -1 and report an error.
  REQUIRE(agent_set_client_public_key(a, client_kp.public_key.c_str()) == -1);
  REQUIRE(agent_set_client_secret_key(a, client_kp.secret_key.c_str()) == -1);
  REQUIRE(agent_set_server_public_key(a, server_kp.public_key.c_str()) == -1);

  REQUIRE(agent_setup_crypto(a, false) == 0);

  REQUIRE(agent_set_client_public_key(a, client_kp.public_key.c_str()) == 0);
  REQUIRE(agent_set_client_secret_key(a, client_kp.secret_key.c_str()) == 0);
  REQUIRE(agent_set_server_public_key(a, server_kp.public_key.c_str()) == 0);

  // init(crypto=true) with keys already set via the string path (no files
  // touched): Agent::init() takes the "curve_auth already exists" branch.
  REQUIRE(agent_init(a, true) == 0);

  agent_destroy(a);
}

TEST_CASE("agent_init(crypto=true) with an incomplete key setup fails and "
          "reports an error",
          "[agent_c]") {
  mads_test::RunningGuard guard;
  agent_t a = agent_create("cabcrypto2", "none");
  REQUIRE(agent_setup_crypto(a, false) == 0);
  // Only the client public key is set; secret/server keys are missing.
  Mads::CurveKeypair client_kp = Mads::generate_keypair();
  REQUIRE(agent_set_client_public_key(a, client_kp.public_key.c_str()) == 0);
  REQUIRE(agent_init(a, true) == -1);
  REQUIRE(std::string(agent_last_error()).find("Error initializing agent") !=
          std::string::npos);
  agent_destroy(a);
}

TEST_CASE("agent_set_key_dir/agent_set_client_key_name/"
          "agent_set_server_key_name/agent_set_auth_verbose do not throw",
          "[agent_c]") {
  mads_test::RunningGuard guard;
  agent_t a = agent_create("cabkeydir", "none");
  agent_set_key_dir(a, "/some/dir/that/need/not/exist/yet");
  agent_set_client_key_name(a, "myclient");
  agent_set_server_key_name(a, "myserver");
  agent_set_auth_verbose(a, true);
  agent_set_auth_verbose(a, false);
  SUCCEED("setters completed without throwing");
  agent_destroy(a);
}

// ---------------------------------------------------------------------------
// Pub/sub round trip entirely through the C ABI's publish/receive/last_*
// functions. Endpoint/cross wiring (not exposed in the C header) is done by
// reaching through to the underlying Mads::Agent, exactly as init() requires
// it be done: after agent_init() and before agent_connect().
// ---------------------------------------------------------------------------

TEST_CASE("C ABI pub/sub round trip: publish(JSON) -> receive -> "
          "agent_last_message",
          "[agent_c]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42303;

  agent_t pub = agent_create("cpubA", "none");
  REQUIRE(agent_init(pub, false) == 0);
  Mads::Agent *pub_ag = reinterpret_cast<Mads::Agent *>(pub);
  pub_ag->set_cross(true);
  pub_ag->set_sub_endpoint(mads_test::loopback(port));
  // Leave pub_topic at its non-empty default (the agent name) so that
  // connect() actually runs connect_pub() (src/agent.cpp:641-644 gates on
  // !_pub_topic.empty()); clear sub_topic to skip connect_sub() for this
  // pure publisher, mirroring make_pub() in test_agent_pubsub.cpp.
  agent_set_sub_topics(pub, nullptr, 0);
  REQUIRE(agent_connect(pub, 0) == 0);

  agent_t sub = agent_create("csubA", "none");
  REQUIRE(agent_init(sub, false) == 0);
  Mads::Agent *sub_ag = reinterpret_cast<Mads::Agent *>(sub);
  sub_ag->set_sub_endpoint(mads_test::loopback(port));
  agent_set_pub_topic(sub, "");
  const char *sub_topics[] = {""};
  agent_set_sub_topics(sub, sub_topics, 1);
  REQUIRE(agent_connect(sub, 0) == 0);

  bool got = retry_until(
      [&] { REQUIRE(agent_publish(pub, "{\"hello\":\"world\",\"n\":7}",
                                  "topicX") == 0); },
      [&] { return agent_receive(sub, true) == mads_json; });
  REQUIRE(got);

  char *topic = nullptr;
  char *message = nullptr;
  agent_last_message(sub, &topic, &message);
  REQUIRE(std::string(topic) == "topicX");
  nlohmann::json doc = nlohmann::json::parse(std::string(message));
  REQUIRE(doc.at("hello") == "world");
  REQUIRE(doc.at("n") == 7);

  // Publishing invalid JSON is rejected before it ever reaches the wire.
  REQUIRE(agent_publish(pub, "{not json", "topicX") == -1);
  REQUIRE(std::string(agent_last_error()).find("Invalid JSON") !=
          std::string::npos);

  agent_disconnect(pub);
  agent_disconnect(sub);
  agent_destroy(pub);
  agent_destroy(sub);
}

TEST_CASE("agent_publish and agent_receive fail while not connected",
          "[agent_c]") {
  mads_test::RunningGuard guard;
  agent_t a = agent_create("cabnotconn", "none");
  REQUIRE(agent_init(a, false) == 0);

  REQUIRE(agent_publish(a, "{}", "t") == -1);
  REQUIRE(std::string(agent_last_error()).find("not connected") !=
          std::string::npos);

  REQUIRE(agent_receive(a, true) == mads_none);
  REQUIRE(std::string(agent_last_error()).find("not connected") !=
          std::string::npos);

  agent_destroy(a);
}

// ---------------------------------------------------------------------------
// agent_sub_topics: output-buffer parameter guards and the "too small"
// branch (src/agent_c.cpp:409-449).
// ---------------------------------------------------------------------------

TEST_CASE("agent_sub_topics validates output parameters, honors a "
          "caller-supplied buffer, and detects an oversized topic name",
          "[agent_c]") {
  mads_test::RunningGuard guard;
  agent_t a = agent_create("cabsubtopics", "none");
  REQUIRE(agent_init(a, false) == 0);
  const char *topics_in[] = {"short"};
  agent_set_sub_topics(a, topics_in, 1);

  // Both output pointers NULL.
  size_t n = 0;
  REQUIRE(agent_sub_topics(a, nullptr, &n) == -1);
  REQUIRE(std::string(agent_last_error())
              .find("Invalid output buffer pointer") != std::string::npos);
  char *buf = nullptr;
  REQUIRE(agent_sub_topics(a, &buf, nullptr) == -1);

  // Caller-supplied (non-null) buffer with a declared size of zero.
  char fixed[8] = {0};
  char *fixed_ptr = fixed;
  size_t zero_n = 0;
  REQUIRE(agent_sub_topics(a, &fixed_ptr, &zero_n) == -1);
  REQUIRE(std::string(agent_last_error()).find("Invalid output buffer size") !=
          std::string::npos);

  // Caller-supplied, correctly-sized buffer: pre-existing content is
  // cleared and then refilled.
  char caller_buf[256];
  std::memset(caller_buf, 'X', sizeof(caller_buf));
  char *caller_ptr = caller_buf;
  size_t caller_n = 1;
  int rc = agent_sub_topics(a, &caller_ptr, &caller_n);
  REQUIRE(rc == 1);
  REQUIRE(caller_n == 1);
  REQUIRE(std::string(caller_buf) == "short");

  // A topic name at/above the fixed 256-byte-per-topic slot size overflows
  // it; snprintf() reports the untruncated length, which the wrapper uses
  // to detect and report the overflow instead of returning a corrupt topic.
  std::string long_topic(300, 'a');
  const char *long_topics[] = {long_topic.c_str()};
  agent_set_sub_topics(a, long_topics, 1);
  char *auto_buf = nullptr;
  size_t auto_n = 0;
  REQUIRE(agent_sub_topics(a, &auto_buf, &auto_n) == -1);
  REQUIRE(std::string(agent_last_error()).find("Output buffer too small") !=
          std::string::npos);
  mads_free(auto_buf);

  agent_destroy(a);
}

// ---------------------------------------------------------------------------
// agent_receive: the exception path (mads_error), triggered the same way
// test_agent_pubsub.cpp triggers Agent::receive()'s AgentError -- threaded
// remote control owns the subscriber socket. enable_remote_control() has no
// C ABI equivalent, so it is called through the underlying Mads::Agent*.
// ---------------------------------------------------------------------------

TEST_CASE("agent_receive reports mads_error when the underlying receive() "
          "throws",
          "[agent_c]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42305;
  agent_t a = agent_create("cabrecverr", "none");
  REQUIRE(agent_init(a, false) == 0);
  Mads::Agent *ag = reinterpret_cast<Mads::Agent *>(a);
  ag->set_sub_endpoint(mads_test::loopback(port));
  agent_set_pub_topic(a, "");
  // Must be called before connect(): pushes "control" onto _sub_topic
  // (non-empty -> connect_sub() runs) and hands the subscriber socket to a
  // background thread.
  ag->enable_remote_control(true);
  REQUIRE(agent_connect(a, 0) == 0);

  REQUIRE(agent_receive(a, true) == mads_error);
  REQUIRE(std::string(agent_last_error()).find("Error receiving message") !=
          std::string::npos);

  agent_destroy(a);
}

// A raw C++ Agent (blob publisher) paired with a C-ABI-created subscriber:
// there is no blob-publish entry point in the C ABI, but agent_receive()
// must still report mads_blob correctly when a blob frame arrives, since the
// message_type_t <-> Mads::message_type mapping is part of this wrapper.
TEST_CASE("agent_receive reports mads_blob for a blob frame published by a "
          "C++ peer",
          "[agent_c]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42304;

  Mads::Agent pub_ag("cppblobpub", "none");
  pub_ag.init(false, false);
  pub_ag.set_cross(true);
  pub_ag.set_sub_endpoint(mads_test::loopback(port));
  pub_ag.set_sub_topic({});
  pub_ag.connect(std::chrono::milliseconds(0));

  agent_t sub = agent_create("cblobsub", "none");
  REQUIRE(agent_init(sub, false) == 0);
  Mads::Agent *sub_ag = reinterpret_cast<Mads::Agent *>(sub);
  sub_ag->set_sub_endpoint(mads_test::loopback(port));
  agent_set_pub_topic(sub, "");
  const char *sub_topics[] = {""};
  agent_set_sub_topics(sub, sub_topics, 1);
  REQUIRE(agent_connect(sub, 0) == 0);

  std::string data = "raw-bytes";
  nlohmann::json meta = {{"format", "raw"}};
  bool got = retry_until(
      [&] { pub_ag.publish(data.data(), data.size(), meta, "blobtopic"); },
      [&] { return agent_receive(sub, true) == mads_blob; });
  REQUIRE(got);

  agent_destroy(sub);
}

// ---------------------------------------------------------------------------
// message_type_t / event_type_t numeric values match Mads::message_type /
// Mads::event_type (the C ABI depends on the two enumerations staying in
// lock-step; this pins that contract).
// ---------------------------------------------------------------------------

TEST_CASE("message_type_t and event_type_t values mirror the C++ enums",
          "[agent_c]") {
  REQUIRE(static_cast<int>(mads_none) == static_cast<int>(Mads::message_type::none));
  REQUIRE(static_cast<int>(mads_json) == static_cast<int>(Mads::message_type::json));
  REQUIRE(static_cast<int>(mads_blob) == static_cast<int>(Mads::message_type::blob));
  REQUIRE(static_cast<int>(mads_error) == static_cast<int>(Mads::message_type::error));

  REQUIRE(static_cast<int>(mads_marker) == static_cast<int>(Mads::event_type::marker));
  REQUIRE(static_cast<int>(mads_marker_in) == static_cast<int>(Mads::event_type::marker_in));
  REQUIRE(static_cast<int>(mads_marker_out) == static_cast<int>(Mads::event_type::marker_out));
  REQUIRE(static_cast<int>(mads_startup) == static_cast<int>(Mads::event_type::startup));
  REQUIRE(static_cast<int>(mads_shutdown) == static_cast<int>(Mads::event_type::shutdown));
  REQUIRE(static_cast<int>(mads_message) == static_cast<int>(Mads::event_type::message));
}
