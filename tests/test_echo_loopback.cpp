// Thin loopback smoke test for `mads echo` (src/main/echo.cpp).
//
// mads-echo is a separate executable, not a library entry point, so it
// cannot be called directly from a test binary. Its actual logic is split
// into two independently-callable, already-tested-elsewhere pieces that this
// suite exercises together instead of spawning a subprocess:
//   - the Agent configuration echo.cpp builds (settings_uri "none", cleared
//     pub_topic, sub_topic set straight from the CLI's positional topic
//     args) -- proven here over a real loopback socket pair, following the
//     make_pub()/make_sub() convention from test_agent_pubsub.cpp;
//   - Mads::format_echo_json()/format_echo_blob() (src/echo_format.hpp),
//     the exact rendering functions echo.cpp's main loop calls on every
//     received message.
// Port range for this file: 42500-42599 (new range: the "(spare)
// 42400-42499" band documented in mads_test_helpers.hpp is no longer spare
// -- see test_agent_events.cpp/test_agent_app.cpp/test_logger_receive.cpp,
// which already claim parts of it).
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent.hpp"
#include "echo_format.hpp"
#include "mads_test_helpers.hpp"

using namespace std::chrono_literals;
using json = nlohmann::json;

namespace {

// Publisher: binds (cross=true) at loopback(port), publishing only. Mirrors
// test_agent_pubsub.cpp's make_pub().
std::unique_ptr<Mads::Agent> make_pub(std::string name, uint16_t port) {
  auto a = std::make_unique<Mads::Agent>(std::move(name), "none");
  a->init(false, false);
  a->set_cross(true);
  a->set_sub_endpoint(mads_test::loopback(port));
  a->set_sub_topic({}); // pure publisher: skip connect_sub entirely
  a->connect(std::chrono::milliseconds(0));
  return a;
}

// Builds the subscriber exactly the way src/main/echo.cpp's main() does for
// its zero-config path: settings_uri "none" (no mads.ini section needed),
// pub_topic cleared (echo never publishes, so connect() skips connect_pub()),
// sub_topic set verbatim from the CLI's positional topic arguments (raw
// pass-through -- Agent::set_sub_topic()/connect_sub() already implement the
// P2 MQTT-wildcard two-stage filter transparently, so echo.cpp adds no
// filtering logic of its own; this is what that claim is actually testing).
std::unique_ptr<Mads::Agent>
make_echo_agent(std::string name, uint16_t port,
                std::vector<std::string> topics = {""}) {
  auto a = std::make_unique<Mads::Agent>(std::move(name), "none");
  a->init(false, false);
  a->set_pub_topic("");
  a->set_sub_topic(std::move(topics));
  a->set_sub_endpoint(mads_test::loopback(port)); // mirrors mads-echo's -b/--broker
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

} // namespace

// ---------------------------------------------------------------------------
// mads-echo's zero-config Agent wiring, end to end, JSON messages.
// ---------------------------------------------------------------------------

TEST_CASE("mads-echo's zero-config Agent setup receives a JSON message and "
          "format_echo_json renders topic/size/payload",
          "[echo_loopback]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42500;
  auto pub = make_pub("pubEchoA", port);
  auto sub = make_echo_agent("echo", port); // sub_topic defaults to [""] (all)

  REQUIRE(sub->pub_topic().empty()); // never publishes

  json payload = {{"temp", 21.5}, {"unit", "C"}};
  bool got = retry_until(
      [&] { pub->publish(payload, "sensors/temp"); },
      [&] { return sub->receive(true) == Mads::message_type::json; });
  REQUIRE(got);

  auto [topic, doc] = sub->last_json();
  REQUIRE(topic == "sensors/temp");

  Mads::EchoRenderOptions opts;
  opts.color = false;
  opts.jsonl = false;
  const std::string dumped = doc.dump();
  std::string out = Mads::format_echo_json(topic, doc, dumped.size(), opts);

  CHECK(out.find("sensors/temp") != std::string::npos);
  CHECK(out.find("\"temp\": 21.5") != std::string::npos);
  CHECK(out.find("\"unit\": \"C\"") != std::string::npos);
  // No ANSI escapes leak through when color is disabled.
  CHECK(out.find("\x1b[") == std::string::npos);
}

TEST_CASE("format_echo_json --jsonl mode emits one parseable compact line "
          "with topic/size/type/payload",
          "[echo_loopback]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42501;
  auto pub = make_pub("pubEchoB", port);
  auto sub = make_echo_agent("echo", port, {""});

  json payload = {{"n", 7}};
  bool got = retry_until(
      [&] { pub->publish(payload, "counter"); },
      [&] { return sub->receive(true) == Mads::message_type::json; });
  REQUIRE(got);

  auto [topic, doc] = sub->last_json();
  Mads::EchoRenderOptions opts;
  opts.jsonl = true;
  opts.color = true; // must be ignored/irrelevant for jsonl: no ANSI codes
  const std::string dumped = doc.dump();
  std::string out = Mads::format_echo_json(topic, doc, dumped.size(), opts);

  // Exactly one line (trailing newline only).
  REQUIRE(out.size() > 0);
  CHECK(out.back() == '\n');
  CHECK(out.find('\n') == out.size() - 1);
  CHECK(out.find("\x1b[") == std::string::npos);

  json line = json::parse(out); // throws (fails the test) if not valid JSON
  CHECK(line.at("topic") == "counter");
  CHECK(line.at("type") == "json");
  CHECK(line.at("size") == dumped.size());
  CHECK(line.at("payload").at("n") == 7);
  REQUIRE(line.contains("timestamp"));
}

// ---------------------------------------------------------------------------
// Blob messages: default one-line summary vs. --raw base64.
// ---------------------------------------------------------------------------

TEST_CASE("format_echo_blob renders a one-line summary by default and exact "
          "base64 bytes with --raw",
          "[echo_loopback]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42502;
  auto pub = make_pub("pubEchoC", port);
  auto sub = make_echo_agent("echo", port, {""});

  const std::string data = "hello blob echo";
  json meta = {{"format", "raw"}};
  bool got = retry_until(
      [&] { pub->publish(data.data(), data.size(), meta, "blobtopic"); },
      [&] { return sub->receive(true) == Mads::message_type::blob; });
  REQUIRE(got);

  auto [topic, meta_text, bytes] = sub->last_blob_view();
  std::string format = json::parse(std::string(meta_text)).value("format", "raw");
  REQUIRE(format == "raw");

  Mads::EchoRenderOptions opts;
  opts.color = false;

  opts.raw = false;
  std::string summary = Mads::format_echo_blob(
      std::string(topic), format, bytes.data(), bytes.size(), opts);
  CHECK(summary.find("<blob " + std::to_string(bytes.size()) + " bytes") !=
        std::string::npos);
  CHECK(summary.find(data) == std::string::npos); // exact bytes NOT shown

  opts.raw = true;
  std::string raw_out = Mads::format_echo_blob(
      std::string(topic), format, bytes.data(), bytes.size(), opts);
  std::string expected_b64 =
      Mads::base64_encode(reinterpret_cast<const unsigned char *>(data.data()),
                          data.size());
  CHECK(raw_out.find(expected_b64) != std::string::npos);
}

TEST_CASE("format_echo_blob --jsonl base64-encodes the payload under --raw",
          "[echo_loopback]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42503;
  auto pub = make_pub("pubEchoD", port);
  auto sub = make_echo_agent("echo", port, {""});

  const std::vector<unsigned char> data = {0, 1, 2, 250, 251, 252, 255};
  json meta = {{"format", "raw"}};
  bool got = retry_until(
      [&] { pub->publish(data, meta, "vecblob"); },
      [&] { return sub->receive(true) == Mads::message_type::blob; });
  REQUIRE(got);

  auto [topic, meta_text, bytes] = sub->last_blob_view();

  Mads::EchoRenderOptions opts;
  opts.jsonl = true;
  opts.raw = true;
  std::string out = Mads::format_echo_blob(std::string(topic), "raw",
                                           bytes.data(), bytes.size(), opts);
  json line = json::parse(out);
  CHECK(line.at("topic") == "vecblob");
  CHECK(line.at("type") == "blob");
  CHECK(line.at("size") == bytes.size());
  std::string expected_b64 = Mads::base64_encode(bytes.data(), bytes.size());
  CHECK(line.at("payload") == expected_b64);
}

// ---------------------------------------------------------------------------
// MQTT-style wildcard topic filter passed straight through to
// set_sub_topic(), exactly as echo.cpp does with the CLI's positional
// topic arguments -- proves no extra filtering logic is needed in echo.cpp.
// ---------------------------------------------------------------------------

TEST_CASE("mads-echo's raw pass-through of a wildcard topic filter to "
          "set_sub_topic() suppresses non-matching topics end to end",
          "[echo_loopback][topic_match]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42504;
  auto pub = make_pub("pubEchoE", port);
  // As if the user ran: mads echo 'sensors/+/x'
  auto sub = make_echo_agent("echo", port, {"sensors/+/x"});

  bool got = retry_until(
      [&] { pub->publish(json{{"v", 1}}, "sensors/acc/x"); },
      [&] { return sub->receive(true) == Mads::message_type::json; });
  REQUIRE(got);
  REQUIRE(std::get<0>(sub->last_message()) == "sensors/acc/x");

  while (sub->receive(true) == Mads::message_type::json) {
  }

  // Same ZMQ-level literal_prefix ("sensors/"), non-matching last level: must
  // be suppressed by the in-process topic_match() filter, not delivered.
  pub->publish(json{{"v", 2}}, "sensors/acc/y");
  std::this_thread::sleep_for(300ms);
  REQUIRE(sub->receive(true) == Mads::message_type::none);
  REQUIRE(std::get<0>(sub->last_message()) == "sensors/acc/x");
}

TEST_CASE("mads-echo defaults to subscribing all topics when no topic "
          "argument is given",
          "[echo_loopback]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42505;
  auto pub = make_pub("pubEchoF", port);
  // As if the user ran: mads echo   (no positional topics)
  auto sub = make_echo_agent("echo", port); // default {""}
  REQUIRE(sub->sub_topic() == std::vector<std::string>{""});

  bool got_a = retry_until(
      [&] { pub->publish(json{{"v", 1}}, "any/topic/a"); },
      [&] { return sub->receive(true) == Mads::message_type::json; });
  REQUIRE(got_a);

  bool got_b = retry_until(
      [&] { pub->publish(json{{"v", 2}}, "completely/different/b"); },
      [&] { return sub->receive(true) == Mads::message_type::json &&
                  std::get<0>(sub->last_message()) == "completely/different/b"; });
  REQUIRE(got_b);
}
