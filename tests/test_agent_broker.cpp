// Unit tests for the Mads::Agent <-> broker settings protocol
// (src/agent.cpp: Agent::query_broker() ~230-307, Agent::init() broker branch
// ~375-380, Agent::save_settings(), Mads::start_agent() ~193-213), exercised
// against an in-process fake broker (a std::thread running a cppzmq REP socket
// on tcp://127.0.0.1:<port>, port range 42200-42299).
//
// Wire protocol transcribed from src/agent.cpp query_broker():
//   Settings round-trip (REQ->REP on one socket, reused for both exchanges):
//     -> [LIB_VERSION]["settings"][agent_name]                (3 parts)
//     <- [broker_version][raw_toml_settings]                  (2 parts), or
//     <- [broker_version][raw_toml_settings][attachment_bytes] (3 parts)
//        parts()==3 is the ONLY trigger for attachment handling; anything
//        else >= 2 parts is accepted with no attachment.
//   Timecode round-trip (same socket, immediately after):
//     -> ["v" + LIB_VERSION]["timecode"]                      (2 parts)
//     <- [broker_timecode_as_string]                          (>=1 part;
//        only get(0) is read, via std::stod)
//
// Failure modes read directly from the source:
//   - msg_in.parts() < 2               -> AgentError "Broker refuses..."
//   - !check_version(msg_in.get(0))    -> AgentError "...wrong version"
//   - send/receive timeout (both legs) -> AgentError "Timed out in ..."
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent.hpp"
#include "mads_test_helpers.hpp"

using namespace std::chrono_literals;

namespace {

// A minimal settings broker: binds a REP socket and replies to the agent's
// "settings" and "timecode" requests with a configurable, exact multipart
// layout. Polls with a receive timeout so the thread can always be stopped
// promptly and joined (never a blocking recv without a timeout).
class FakeBroker {
public:
  explicit FakeBroker(uint16_t port)
      : _ctx(), _sock(_ctx, zmq::socket_type::rep) {
    _sock.set(zmq::sockopt::rcvtimeo, 100);
    _sock.bind(mads_test::loopback(port));
  }

  ~FakeBroker() { stop(); }

  // --- configuration: set before start(), never mutated afterwards ---
  std::string settings_version = LIB_VERSION;
  std::string settings_body;
  bool with_attachment = false;
  std::string attachment_bytes;
  bool refuse_settings = false; // reply with only the version part (<2 total)
  double timecode_value = 0.0;
  int timecode_delay_ms = 0; // artificial delay before the timecode reply

  void start() {
    _thread = std::thread([this] { run(); });
  }

  void stop() {
    if (_stopped.exchange(true)) return;
    if (_thread.joinable()) _thread.join();
  }

private:
  void run() {
    while (!_stopped) {
      zmq::multipart_t msg;
      if (!msg.recv(_sock)) continue; // 100ms poll timeout, check _stopped
      if (msg.size() < 2) continue;     // malformed request, ignore
      std::string kind = msg.at(1).to_string();
      zmq::multipart_t reply;
      if (kind == "settings") {
        if (refuse_settings) {
          reply.addstr(settings_version);
        } else {
          reply.addstr(settings_version);
          reply.addstr(settings_body);
          if (with_attachment) reply.addstr(attachment_bytes);
        }
        reply.send(_sock);
      } else if (kind == "timecode") {
        if (timecode_delay_ms > 0)
          std::this_thread::sleep_for(
              std::chrono::milliseconds(timecode_delay_ms));
        reply.addstr(std::to_string(timecode_value));
        reply.send(_sock);
      } else {
        // Keep the REP socket's strict recv/send alternation intact even for
        // requests this fake broker does not otherwise understand.
        reply.addstr(std::string(LIB_VERSION));
        reply.addstr(std::string("{}"));
        reply.send(_sock);
      }
    }
  }

  zmq::context_t _ctx;
  zmq::socket_t _sock;
  std::thread _thread;
  std::atomic<bool> _stopped{false};
};

// Builds a settings TOML body mirroring the shape of the repo-root mads.ini:
// a fleet-wide [agents] section plus a per-agent section for `name`.
// Frontend/backend addresses are pinned into our port range so that
// Agent::connect() (which only *connects*, never binds, unless
// set_cross(true)) never touches a port outside 42200-42299.
std::string make_settings_toml(const std::string &name,
                               const std::string &extra_agent_lines = "") {
  std::ostringstream ss;
  ss << "[agents]\n"
     << "timecode_fps = 25\n"
     << "frontend_address = \"tcp://localhost:42290\"\n"
     << "backend_address = \"tcp://localhost:42291\"\n"
     << "\n[" << name << "]\n"
     << "pub_topic = \"" << name << "_pub\"\n"
     << "sub_topic = [\"in_a\", \"in_b\"]\n"
     << extra_agent_lines;
  return ss.str();
}

// Extracts the numeric value printed after "Timecode offset:" in
// Agent::info() (src/agent.cpp:617-618), stripping any ANSI styling
// (rang::style::bold/reset) that may surround it so a plain istream
// extraction lands on the actual number rather than an escape code digit.
double parse_timecode_offset(const std::string &info_text) {
  static const std::string ESC = "\x1b[";
  std::string clean;
  clean.reserve(info_text.size());
  for (size_t i = 0; i < info_text.size();) {
    if (info_text.compare(i, ESC.size(), ESC) == 0) {
      size_t end = info_text.find('m', i);
      if (end == std::string::npos) break;
      i = end + 1;
    } else {
      clean.push_back(info_text[i]);
      ++i;
    }
  }
  auto pos = clean.find("Timecode offset:");
  REQUIRE(pos != std::string::npos);
  std::istringstream iss(clean.substr(pos + std::string("Timecode offset:").size()));
  double val = 0.0;
  iss >> val;
  return val;
}

} // namespace

// ---------------------------------------------------------------------------
// init() over the broker protocol: happy path
// ---------------------------------------------------------------------------

TEST_CASE("init() against a fake broker loads settings via the REQ/REP "
          "protocol",
          "[agent_broker]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42200;
  FakeBroker broker(port);
  broker.settings_body = make_settings_toml("bhappy");
  broker.timecode_value = 100.0;
  broker.start();

  Mads::Agent a("bhappy", mads_test::loopback(port));
  a.init(false, false);

  REQUIRE_FALSE(a.settings_are_local());
  REQUIRE(a.settings_uri() == mads_test::loopback(port));
  REQUIRE(a.pub_topic() == "bhappy_pub");
  REQUIRE(a.sub_topic() == std::vector<std::string>{"in_a", "in_b"});

  nlohmann::json j = a.get_settings();
  REQUIRE(j.at("pub_topic") == "bhappy_pub");
  REQUIRE(j.at("sub_topic") == nlohmann::json::array({"in_a", "in_b"}));

  // No attachment was served.
  REQUIRE(a.attachment_path().empty());

  // Host substitution (src/agent.cpp:394-401): the broker's host replaces the
  // host portion of frontend_address/backend_address, keeping their ports.
  REQUIRE(a.pub_endpoint() == "tcp://127.0.0.1:42290");
  REQUIRE(a.sub_endpoint() == "tcp://127.0.0.1:42291");
}

// ---------------------------------------------------------------------------
// save_settings() after a broker-backed init()
// ---------------------------------------------------------------------------

TEST_CASE("save_settings() writes the exact raw settings text served by the "
          "broker",
          "[agent_broker]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42201;
  FakeBroker broker(port);
  broker.settings_body = make_settings_toml("bsave");
  broker.timecode_value = 0.0;
  broker.start();

  Mads::Agent a("bsave", mads_test::loopback(port));
  a.init(false, false);
  REQUIRE_FALSE(a.settings_are_local());

  auto tmp =
      std::filesystem::temp_directory_path() / "mads_test_broker_save.ini";
  a.save_settings(tmp.string());

  std::ifstream in(tmp);
  REQUIRE(in.good());
  std::ostringstream contents;
  contents << in.rdbuf();
  in.close(); // Windows locks open files against remove() below
  REQUIRE(contents.str() == broker.settings_body);
  std::filesystem::remove(tmp);
}

// ---------------------------------------------------------------------------
// Attachment handling
// ---------------------------------------------------------------------------

TEST_CASE("init() saves a served attachment under the default .plugin "
          "extension",
          "[agent_broker]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42202;
  FakeBroker broker(port);
  broker.settings_body = make_settings_toml("battach1");
  broker.with_attachment = true;
  broker.attachment_bytes = "BINARY-PLUGIN-DATA-1";
  broker.timecode_value = 0.0;
  broker.start();

  Mads::Agent a("battach1", mads_test::loopback(port));
  a.init(false, false);

  auto path = a.attachment_path();
  REQUIRE_FALSE(path.empty());
  REQUIRE(path.extension() == ".plugin");
  REQUIRE(std::filesystem::exists(path));

  std::ifstream in(path, std::ios::binary);
  std::ostringstream contents;
  contents << in.rdbuf();
  REQUIRE(contents.str() == broker.attachment_bytes);
}

TEST_CASE("init() saves a served attachment under a custom attachment_ext",
          "[agent_broker]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42203;
  FakeBroker broker(port);
  broker.settings_body =
      make_settings_toml("battach2", "attachment_ext = \"bin\"\n");
  broker.with_attachment = true;
  broker.attachment_bytes = "BINARY-PLUGIN-DATA-2";
  broker.timecode_value = 0.0;
  broker.start();

  Mads::Agent a("battach2", mads_test::loopback(port));
  a.init(false, false);

  auto path = a.attachment_path();
  REQUIRE_FALSE(path.empty());
  REQUIRE(path.extension() == ".bin");
  REQUIRE(std::filesystem::exists(path));

  std::ifstream in(path, std::ios::binary);
  std::ostringstream contents;
  contents << in.rdbuf();
  REQUIRE(contents.str() == broker.attachment_bytes);
}

// The plugin loaders fall back to the attachment file's stem for the driver
// name when the settings section has no `driver` key (plugin_loader.cpp,
// worker.cpp), so the content digest that keys the cache must stay out of the
// stem -- it is a directory component. See src/detail/plugin_cache.hpp.
TEST_CASE("a served attachment keeps the section name as its file stem",
          "[agent_broker]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42208;
  FakeBroker broker(port);
  broker.settings_body = make_settings_toml("battach3");
  broker.with_attachment = true;
  broker.attachment_bytes = "BINARY-PLUGIN-DATA-3";
  broker.timecode_value = 0.0;
  broker.start();

  Mads::Agent a("battach3", mads_test::loopback(port));
  a.init(false, false);

  REQUIRE(a.attachment_path().stem() == "battach3");
}

// Two instances of one scaled agent: both must resolve to the same cached file
// rather than each rewriting a shared path, which is what used to crash a
// sibling that had already dlopen()'d it.
TEST_CASE("identical attachment bytes resolve to one shared cached path",
          "[agent_broker]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42209;
  FakeBroker broker(port);
  broker.settings_body = make_settings_toml("battach4");
  broker.with_attachment = true;
  broker.attachment_bytes = "BINARY-PLUGIN-DATA-4";
  broker.timecode_value = 0.0;
  broker.start();

  Mads::Agent first("battach4", mads_test::loopback(port));
  first.init(false, false);
  Mads::Agent second("battach4", mads_test::loopback(port));
  second.init(false, false);

  REQUIRE(first.attachment_path() == second.attachment_path());
  REQUIRE(std::filesystem::exists(first.attachment_path()));

  std::ifstream in(first.attachment_path(), std::ios::binary);
  std::ostringstream contents;
  contents << in.rdbuf();
  REQUIRE(contents.str() == broker.attachment_bytes);
}

// The invalidation half: a different binary on the broker must not land on the
// path an already-running instance is using. FakeBroker's config is fixed
// before start(), so serving different bytes needs a second broker.
TEST_CASE("changed attachment bytes resolve to a different cached path",
          "[agent_broker]") {
  mads_test::RunningGuard guard;
  const uint16_t port_v1 = 42210;
  const uint16_t port_v2 = 42211;

  FakeBroker broker_v1(port_v1);
  broker_v1.settings_body = make_settings_toml("battach5");
  broker_v1.with_attachment = true;
  broker_v1.attachment_bytes = "BINARY-PLUGIN-DATA-5-V1";
  broker_v1.timecode_value = 0.0;
  broker_v1.start();

  Mads::Agent v1("battach5", mads_test::loopback(port_v1));
  v1.init(false, false);
  const auto v1_path = v1.attachment_path();
  REQUIRE(std::filesystem::exists(v1_path));

  FakeBroker broker_v2(port_v2);
  broker_v2.settings_body = make_settings_toml("battach5");
  broker_v2.with_attachment = true;
  broker_v2.attachment_bytes = "BINARY-PLUGIN-DATA-5-V2";
  broker_v2.timecode_value = 0.0;
  broker_v2.start();

  Mads::Agent v2("battach5", mads_test::loopback(port_v2));
  v2.init(false, false);

  REQUIRE(v2.attachment_path() != v1_path);
  REQUIRE(v2.attachment_path().stem() == "battach5");

  std::ifstream in(v2.attachment_path(), std::ios::binary);
  std::ostringstream contents;
  contents << in.rdbuf();
  REQUIRE(contents.str() == broker_v2.attachment_bytes);
}

// ---------------------------------------------------------------------------
// Timecode offset
// ---------------------------------------------------------------------------

TEST_CASE("init() applies the broker's timecode offset", "[agent_broker]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42204;

  auto now0 = std::chrono::system_clock::now();
  double local_tc0 = Mads::timecode(now0, 25);
  const double artificial_offset = 100.5;

  FakeBroker broker(port);
  broker.settings_body = make_settings_toml("btimecode");
  broker.timecode_value = local_tc0 + artificial_offset;
  broker.start();

  Mads::Agent a("btimecode", mads_test::loopback(port));
  a.init(false, false);

  std::ostringstream oss;
  a.info(oss);
  double measured_offset = parse_timecode_offset(oss.str());
  // Generous tolerance: only verifies the offset was applied at all, not
  // sub-second precision (query_broker's `now` capture happens after our
  // reference point, and Mads::timecode() buckets to ~40ms).
  REQUIRE(std::abs(measured_offset - artificial_offset) < 1.0);
}

// ---------------------------------------------------------------------------
// Error paths: broker response validation
// ---------------------------------------------------------------------------

TEST_CASE("init() throws AgentError when the broker reports a mismatched "
          "version",
          "[agent_broker]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42205;
  FakeBroker broker(port);
  broker.settings_version = "v9.9.9"; // != LIB_VERSION_CHECK ("v0.0")
  broker.settings_body = make_settings_toml("bversion");
  broker.start();

  Mads::Agent a("bversion", mads_test::loopback(port));
  bool threw = false;
  try {
    a.init(false, false);
  } catch (const Mads::AgentError &e) {
    threw = true;
    REQUIRE(std::string(e.what()).find("wrong version") != std::string::npos);
  }
  REQUIRE(threw);
}

TEST_CASE("init() throws AgentError when the broker refuses to provide "
          "settings",
          "[agent_broker]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42206;
  FakeBroker broker(port);
  broker.refuse_settings = true; // reply has < 2 parts
  broker.start();

  Mads::Agent a("brefuse", mads_test::loopback(port));
  bool threw = false;
  try {
    a.init(false, false);
  } catch (const Mads::AgentError &e) {
    threw = true;
    REQUIRE(std::string(e.what()).find("refuses") != std::string::npos);
  }
  REQUIRE(threw);
}

// ---------------------------------------------------------------------------
// Error paths: timeouts (deterministic via set_settings_timeout(), never a
// blocking wait without a bound)
// ---------------------------------------------------------------------------

TEST_CASE("init() throws AgentError when the settings request times out "
          "against an unbound port",
          "[agent_broker]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42207; // intentionally nothing bound here
  Mads::Agent a("btimeout1", mads_test::loopback(port));
  a.set_settings_timeout(200);
  REQUIRE(a.settings_timeout() == 200);

  bool threw = false;
  try {
    a.init(false, false);
  } catch (const Mads::AgentError &e) {
    threw = true;
    REQUIRE(std::string(e.what()).find("Timed out") != std::string::npos);
    REQUIRE(std::string(e.what()).find("settings") != std::string::npos);
  }
  REQUIRE(threw);
}

TEST_CASE("init() throws AgentError when the timecode request times out",
          "[agent_broker]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42208;
  FakeBroker broker(port);
  broker.settings_body = make_settings_toml("btimeout2");
  broker.timecode_delay_ms = 800; // longer than the agent's settings timeout
  broker.start();

  Mads::Agent a("btimeout2", mads_test::loopback(port));
  a.set_settings_timeout(200);

  bool threw = false;
  try {
    a.init(false, false);
  } catch (const Mads::AgentError &e) {
    threw = true;
    REQUIRE(std::string(e.what()).find("Timed out") != std::string::npos);
    REQUIRE(std::string(e.what()).find("timecode") != std::string::npos);
  }
  REQUIRE(threw);
}

// ---------------------------------------------------------------------------
// start_agent()
// ---------------------------------------------------------------------------

TEST_CASE("start_agent() against a fake broker returns a connected, "
          "broker-backed agent",
          "[agent_broker]") {
  mads_test::RunningGuard guard;
  const uint16_t port = 42209;
  FakeBroker broker(port);
  broker.settings_body = make_settings_toml("bstart");
  broker.start();

  auto agent = Mads::start_agent("bstart", mads_test::loopback(port));
  REQUIRE(agent->is_connected());
  REQUIRE_FALSE(agent->settings_are_local());
  REQUIRE(agent->name() == "bstart");
  REQUIRE(agent->pub_topic() == "bstart_pub");
}

TEST_CASE("start_agent() throws AgentError for incomplete crypto_settings",
          "[agent_broker]") {
  mads_test::RunningGuard guard;
  // No socket is ever touched: the crypto_settings keys are validated before
  // any Agent is constructed (src/agent.cpp:196-203).
  REQUIRE_THROWS_AS(
      Mads::start_agent("bcrypto", "none", {{"key_dir", "/tmp/keys"}}),
      Mads::AgentError);
  REQUIRE_THROWS_AS(
      Mads::start_agent("bcrypto", "none",
                        {{"key_dir", "/tmp/keys"}, {"key_client", "client"}}),
      Mads::AgentError);
}
