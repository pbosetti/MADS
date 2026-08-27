// Unit tests for src/doctor_checks.hpp/.cpp, the pure/self-contained check
// logic backing `mads doctor` (src/main/doctor.cpp).
//
// Table-driven throughout: each check function is exercised against a battery
// of injected fakes -- a temp settings file (reusing tests/fixtures/settings/
// valid.toml and malformed.toml, the same fixtures test_agent_settings.cpp
// already uses), a temp CURVE keypair (same TempKeyDir pattern as
// tests/test_curve.cpp), and the same fake-broker REP-socket-on-a-thread
// pattern as tests/test_broker_probe.cpp -- with no live broker, no pugg, and
// no real .plugin file required anywhere in this suite (see doctor_checks.hpp's
// file-level comment for why the pugg-based plugin *loading* itself is
// exercised in src/main/doctor.cpp instead, not here).
//
// Port range for this file: 42700-42799 (fresh, disjoint from every other
// suite's documented range in mads_test_helpers.hpp).
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <zmq.hpp>
#include <zmq_addon.hpp>

#include "curve.hpp"
#include "zap_auth.hpp"

#include "doctor_checks.hpp"
#include "mads_test_helpers.hpp"

using namespace std::chrono_literals;
namespace fs = std::filesystem;
using Mads::Doctor::CheckResult;
using Mads::Doctor::Status;

namespace {

fs::path fixtures_dir() { return fs::path(MADS_TEST_FIXTURES_DIR) / "settings"; }

// Same fake-broker pattern as tests/test_broker_probe.cpp: a REP socket on a
// background thread that replies to any request, since probe_broker() only
// needs proof that something is there and speaking back.
class FakeBroker {
public:
  explicit FakeBroker(uint16_t port) : _ctx(), _sock(_ctx, zmq::socket_type::rep) {
    _sock.set(zmq::sockopt::rcvtimeo, 100);
    _sock.bind(mads_test::loopback(port));
  }
  ~FakeBroker() { stop(); }

  void start() { _thread = std::thread([this] { run(); }); }
  void stop() {
    if (_stopped.exchange(true)) return;
    if (_thread.joinable()) _thread.join();
  }

private:
  void run() {
    while (!_stopped) {
      zmq::multipart_t msg;
      if (!msg.recv(_sock)) continue;
      zmq::multipart_t reply;
      reply.addstr(std::string("v0.0"));
      reply.addstr(std::string("{}"));
      reply.send(_sock);
    }
  }
  zmq::context_t _ctx;
  zmq::socket_t _sock;
  std::thread _thread;
  std::atomic<bool> _stopped{false};
};

// The same fake broker behind a real CURVE server socket, with a live ZAP
// authenticator accepting the client key it is handed -- i.e. exactly what a
// `mads broker --crypto` settings endpoint looks like from outside. Needed
// because an unencrypted probe against one of these is dropped during the
// ZMTP handshake, which is indistinguishable from a broker that is down
// unless the probe carries CURVE credentials of its own.
class FakeCurveBroker {
public:
  FakeCurveBroker(uint16_t port, const fs::path &key_dir)
      : _ctx(), _sock(_ctx, zmq::socket_type::rep), _auth(_ctx) {
    _auth.setup_auth(Mads::auth_verbose::off);
    _auth.fetch_public_keys(key_dir);
    _auth.setup_curve_server(_sock, "broker");
    _sock.set(zmq::sockopt::rcvtimeo, 100);
    _sock.bind(mads_test::loopback(port));
  }
  ~FakeCurveBroker() { stop(); }

  void start() { _thread = std::thread([this] { run(); }); }
  void stop() {
    if (_stopped.exchange(true)) return;
    if (_thread.joinable()) _thread.join();
  }

private:
  void run() {
    while (!_stopped) {
      zmq::multipart_t msg;
      if (!msg.recv(_sock)) continue;
      zmq::multipart_t reply;
      reply.addstr(std::string("v0.0"));
      reply.addstr(std::string("{}"));
      reply.send(_sock);
    }
  }
  zmq::context_t _ctx;
  zmq::socket_t _sock;
  Mads::CurveAuth _auth;
  std::thread _thread;
  std::atomic<bool> _stopped{false};
};

// A plain TCP listener (no ZMQ protocol needed): used by the port-available
// tests, where probe_tcp_port() only cares that *something* accepts a
// connection.
class FakeListener {
public:
  explicit FakeListener(uint16_t port) : _ctx(), _sock(_ctx, zmq::socket_type::rep) {
    _sock.bind(mads_test::loopback(port));
  }

private:
  zmq::context_t _ctx;
  zmq::socket_t _sock;
};

// RAII temp directory, same shape as tests/test_curve.cpp's TempKeyDir.
struct TempDir {
  fs::path path;
  TempDir(const std::string &tag) {
    path = fs::temp_directory_path() /
           ("mads_test_doctor_" + tag + "_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

void write_file(const fs::path &p, const std::string &content) {
  std::ofstream f(p, std::ios::binary);
  f << content;
}

void write_keypair(const fs::path &dir, const std::string &name,
                   const Mads::CurveKeypair &kp) {
  write_file(dir / (name + ".key"), kp.secret_key + "\n");
  write_file(dir / (name + ".pub"), kp.public_key + "\n");
}

} // namespace

// ---------------------------------------------------------------------------
// 1. check_settings_file()
// ---------------------------------------------------------------------------

TEST_CASE("check_settings_file passes on a valid local TOML file", "[doctor][settings]") {
  auto r = Mads::Doctor::check_settings_file(fixtures_dir() / "valid.toml");
  REQUIRE(r.status == Status::Pass);
  REQUIRE(r.message.find("parses as valid TOML") != std::string::npos);
  REQUIRE_FALSE(r.fixable);
}

TEST_CASE("check_settings_file fails with a fixable hint when the file is missing",
         "[doctor][settings]") {
  TempDir dir("settings_missing");
  auto r = Mads::Doctor::check_settings_file(dir.path / "does_not_exist.ini");
  REQUIRE(r.status == Status::Fail);
  REQUIRE(r.fixable);
  REQUIRE(r.message.find("not found") != std::string::npos);
  REQUIRE(r.fix_hint.find("--fix") != std::string::npos);
}

TEST_CASE("check_settings_file fails with the parse error for malformed TOML",
         "[doctor][settings]") {
  auto r = Mads::Doctor::check_settings_file(fixtures_dir() / "malformed.toml");
  REQUIRE(r.status == Status::Fail);
  REQUIRE_FALSE(r.fixable); // the file exists; --fix must never overwrite it
  REQUIRE(r.message.find("TOML parse error") != std::string::npos);
}

TEST_CASE("check_settings_file treats a tcp:// URI as a remote broker, not a local file",
         "[doctor][settings]") {
  auto r = Mads::Doctor::check_settings_file("tcp://localhost:9092");
  REQUIRE(r.status == Status::Pass);
  REQUIRE(r.message.find("remote broker") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 2. evaluate_broker_reachable() / check_broker_reachable()
// ---------------------------------------------------------------------------

TEST_CASE("evaluate_broker_reachable reports pass/fail for known reachability",
         "[doctor][broker]") {
  auto pass = Mads::Doctor::evaluate_broker_reachable("tcp://localhost:9092", true);
  REQUIRE(pass.status == Status::Pass);
  REQUIRE(pass.message.find("reachable") != std::string::npos);

  auto fail = Mads::Doctor::evaluate_broker_reachable("tcp://localhost:9092", false);
  REQUIRE(fail.status == Status::Fail);
  REQUIRE_FALSE(fail.fix_hint.empty());
}

TEST_CASE("check_broker_reachable passes against a reachable fake broker",
         "[doctor][broker]") {
  const uint16_t port = 42700;
  FakeBroker broker(port);
  broker.start();

  auto r = Mads::Doctor::check_broker_reachable(mads_test::loopback(port), 2000ms);
  REQUIRE(r.status == Status::Pass);
}

TEST_CASE("check_broker_reachable fails against an unreachable broker",
         "[doctor][broker]") {
  const uint16_t port = 42701; // intentionally nothing bound here
  auto r = Mads::Doctor::check_broker_reachable(mads_test::loopback(port), 300ms);
  REQUIRE(r.status == Status::Fail);
}

// Regression: `mads doctor --crypto` used to probe a CURVE-secured broker
// with a plain REQ socket, which libzmq drops during the ZMTP handshake. A
// perfectly healthy encrypted broker was therefore reported as "did not
// respond" -- two lines above a passing CURVE handshake check against the
// very same URI.
TEST_CASE("check_broker_reachable reaches a CURVE broker only when given keys",
         "[doctor][broker][curve]") {
  const uint16_t port = 42704;
  TempDir dir("broker_curve");
  auto client_kp = Mads::generate_keypair();
  auto server_kp = Mads::generate_keypair();
  write_keypair(dir.path, "client", client_kp);
  write_keypair(dir.path, "broker", server_kp);

  FakeCurveBroker broker(port, dir.path);
  broker.start();
  const std::string uri = mads_test::loopback(port);

  Mads::Doctor::CurveKeyCheck cfg;
  cfg.key_dir = dir.path;
  auto encrypted = Mads::Doctor::check_broker_reachable(uri, 2000ms, cfg);
  REQUIRE(encrypted.status == Status::Pass);
  REQUIRE(encrypted.message.find("(CURVE)") != std::string::npos);

  auto plain = Mads::Doctor::check_broker_reachable(uri, 500ms);
  REQUIRE(plain.status == Status::Fail);
  // The hint has to name the encryption mismatch: the address it would
  // otherwise send the user off to check is not what is wrong here.
  REQUIRE(plain.fix_hint.find("--crypto") != std::string::npos);
}

// The mirror image: a CURVE-configured probe against an unencrypted broker
// must fail too, and say which way round the mismatch is.
TEST_CASE("check_broker_reachable with keys fails against a plain broker",
         "[doctor][broker][curve]") {
  const uint16_t port = 42705;
  TempDir dir("broker_plain_vs_curve");
  auto client_kp = Mads::generate_keypair();
  auto server_kp = Mads::generate_keypair();
  write_keypair(dir.path, "client", client_kp);
  write_keypair(dir.path, "broker", server_kp);

  FakeBroker broker(port);
  broker.start();

  Mads::Doctor::CurveKeyCheck cfg;
  cfg.key_dir = dir.path;
  auto r = Mads::Doctor::check_broker_reachable(mads_test::loopback(port), 500ms, cfg);
  REQUIRE(r.status == Status::Fail);
  REQUIRE(r.message.find("(CURVE)") != std::string::npos);
  REQUIRE(r.fix_hint.find("drop --crypto") != std::string::npos);
}

// Unreadable key files must not escape as an exception: a probe that could
// not even be configured is a probe that got no answer.
TEST_CASE("check_broker_reachable reports missing key files as no response",
         "[doctor][broker][curve]") {
  Mads::Doctor::CurveKeyCheck cfg;
  cfg.key_dir = "/no/such/curve/key/dir/at/all";
  auto r = Mads::Doctor::check_broker_reachable(mads_test::loopback(42706), 300ms, cfg);
  REQUIRE(r.status == Status::Fail);
}

// ---------------------------------------------------------------------------
// 3. evaluate_plugin_load()
// ---------------------------------------------------------------------------

TEST_CASE("evaluate_plugin_load fails when the plugin file does not exist",
         "[doctor][plugin]") {
  Mads::Doctor::PluginLoadFacts facts;
  facts.plugin_file = "/no/such/plugin.plugin";
  facts.file_exists = false;

  auto r = Mads::Doctor::evaluate_plugin_load(facts);
  REQUIRE(r.status == Status::Fail);
  REQUIRE(r.message.find("not found") != std::string::npos);
}

TEST_CASE("evaluate_plugin_load fails with the loader's error when loading fails",
         "[doctor][plugin]") {
  Mads::Doctor::PluginLoadFacts facts;
  facts.plugin_file = "broken.plugin";
  facts.file_exists = true;
  facts.loaded = false;
  facts.error = "no Source/Filter/Sink driver named 'broken' found";

  auto r = Mads::Doctor::evaluate_plugin_load(facts);
  REQUIRE(r.status == Status::Fail);
  REQUIRE(r.message.find("broken") != std::string::npos);
  REQUIRE(r.message.find("no Source/Filter/Sink driver") != std::string::npos);
}

TEST_CASE("evaluate_plugin_load passes and reports kind/name/protocol when loaded",
         "[doctor][plugin]") {
  Mads::Doctor::PluginLoadFacts facts;
  facts.plugin_file = "publish.plugin";
  facts.file_exists = true;
  facts.loaded = true;
  facts.driver_kind = "source";
  facts.driver_name = "publish";
  facts.protocol_version = 8;

  auto r = Mads::Doctor::evaluate_plugin_load(facts);
  REQUIRE(r.status == Status::Pass);
  REQUIRE(r.message.find("source/publish") != std::string::npos);
  REQUIRE(r.message.find("v8") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 4. parse_pinned_plugin_protocol() / evaluate_plugin_protocol()
// ---------------------------------------------------------------------------

TEST_CASE("parse_pinned_plugin_protocol reads the plugin_protocol field",
         "[doctor][plugin]") {
  auto v = Mads::Doctor::parse_pinned_plugin_protocol(
      R"({"plugin_protocol": 8, "plugin_git_tag": "v2.4-P8"})");
  REQUIRE(v.has_value());
  REQUIRE(*v == 8);
}

TEST_CASE("parse_pinned_plugin_protocol returns nullopt for malformed/missing input",
         "[doctor][plugin]") {
  REQUIRE_FALSE(Mads::Doctor::parse_pinned_plugin_protocol("not json").has_value());
  REQUIRE_FALSE(Mads::Doctor::parse_pinned_plugin_protocol("{}").has_value());
  REQUIRE_FALSE(
      Mads::Doctor::parse_pinned_plugin_protocol(R"({"plugin_protocol": "eight"})")
          .has_value());
}

TEST_CASE("evaluate_plugin_protocol warns when the plugin never loaded",
         "[doctor][plugin]") {
  auto r = Mads::Doctor::evaluate_plugin_protocol(-1, 8);
  REQUIRE(r.status == Status::Warn);
}

TEST_CASE("evaluate_plugin_protocol fails below the minimum supported protocol",
         "[doctor][plugin]") {
  auto r = Mads::Doctor::evaluate_plugin_protocol(5, 8, /*min_supported=*/7);
  REQUIRE(r.status == Status::Fail);
  REQUIRE(r.message.find("older than the minimum") != std::string::npos);
}

TEST_CASE("evaluate_plugin_protocol warns when the pinned protocol is unknown",
         "[doctor][plugin]") {
  auto r = Mads::Doctor::evaluate_plugin_protocol(8, std::nullopt);
  REQUIRE(r.status == Status::Warn);
  REQUIRE(r.message.find("could not read the pinned protocol") != std::string::npos);
}

TEST_CASE("evaluate_plugin_protocol warns on a mismatch above the minimum",
         "[doctor][plugin]") {
  auto r = Mads::Doctor::evaluate_plugin_protocol(9, 8);
  REQUIRE(r.status == Status::Warn);
  REQUIRE(r.message.find("differs from") != std::string::npos);
}

TEST_CASE("evaluate_plugin_protocol passes on an exact match", "[doctor][plugin]") {
  auto r = Mads::Doctor::evaluate_plugin_protocol(8, 8);
  REQUIRE(r.status == Status::Pass);
  REQUIRE(r.message.find("matches") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 5. is_well_formed_curve_key() / check_curve_keys()
// ---------------------------------------------------------------------------

TEST_CASE("is_well_formed_curve_key accepts a real generated Z85 key and rejects junk",
         "[doctor][curve]") {
  auto kp = Mads::generate_keypair();
  REQUIRE(Mads::Doctor::is_well_formed_curve_key(kp.public_key));
  REQUIRE(Mads::Doctor::is_well_formed_curve_key(kp.secret_key));

  REQUIRE_FALSE(Mads::Doctor::is_well_formed_curve_key(""));
  REQUIRE_FALSE(Mads::Doctor::is_well_formed_curve_key("too-short"));
  // Right length (40), but '"' and '\' are outside the Z85 alphabet.
  REQUIRE_FALSE(Mads::Doctor::is_well_formed_curve_key(std::string(40, '"')));
}

TEST_CASE("check_curve_keys fails when the key directory does not exist",
         "[doctor][curve]") {
  Mads::Doctor::CurveKeyCheck cfg;
  cfg.key_dir = "/no/such/curve/key/dir/at/all";
  auto r = Mads::Doctor::check_curve_keys(cfg);
  REQUIRE(r.status == Status::Fail);
  REQUIRE(r.message.find("does not exist") != std::string::npos);
}

TEST_CASE("check_curve_keys fails listing every missing file", "[doctor][curve]") {
  TempDir dir("curve_missing");
  Mads::Doctor::CurveKeyCheck cfg;
  cfg.key_dir = dir.path;
  cfg.client_key_name = "client";
  cfg.server_key_name = "broker";

  auto r = Mads::Doctor::check_curve_keys(cfg);
  REQUIRE(r.status == Status::Fail);
  REQUIRE(r.message.find("client secret key") != std::string::npos);
  REQUIRE(r.message.find("client public key") != std::string::npos);
  REQUIRE(r.message.find("server/broker public key") != std::string::npos);
}

TEST_CASE("check_curve_keys fails when a key file exists but is not well-formed",
         "[doctor][curve]") {
  TempDir dir("curve_malformed");
  auto client_kp = Mads::generate_keypair();
  auto server_kp = Mads::generate_keypair();
  write_keypair(dir.path, "client", client_kp);
  write_file(dir.path / "broker.pub", "not-a-real-z85-key-value\n");

  Mads::Doctor::CurveKeyCheck cfg;
  cfg.key_dir = dir.path;
  cfg.client_key_name = "client";
  cfg.server_key_name = "broker";

  auto r = Mads::Doctor::check_curve_keys(cfg);
  REQUIRE(r.status == Status::Fail);
  REQUIRE(r.message.find("server/broker public key") != std::string::npos);
  REQUIRE(r.message.find("not a well-formed") != std::string::npos);
}

TEST_CASE("check_curve_keys passes when client + server key files are all well-formed",
         "[doctor][curve]") {
  TempDir dir("curve_ok");
  auto client_kp = Mads::generate_keypair();
  auto server_kp = Mads::generate_keypair();
  write_keypair(dir.path, "client", client_kp);
  write_file(dir.path / "broker.pub", server_kp.public_key + "\n");

  Mads::Doctor::CurveKeyCheck cfg;
  cfg.key_dir = dir.path;
  cfg.client_key_name = "client";
  cfg.server_key_name = "broker";

  auto r = Mads::Doctor::check_curve_keys(cfg);
  REQUIRE(r.status == Status::Pass);
}

TEST_CASE("check_curve_keys tolerates CRLF line endings in key files", "[doctor][curve]") {
  TempDir dir("curve_crlf");
  auto client_kp = Mads::generate_keypair();
  auto server_kp = Mads::generate_keypair();
  write_file(dir.path / "client.key", client_kp.secret_key + "\r\n");
  write_file(dir.path / "client.pub", client_kp.public_key + "\r\n");
  write_file(dir.path / "broker.pub", server_kp.public_key + "\r\n");

  Mads::Doctor::CurveKeyCheck cfg;
  cfg.key_dir = dir.path;
  auto r = Mads::Doctor::check_curve_keys(cfg);
  REQUIRE(r.status == Status::Pass);
}

// ---------------------------------------------------------------------------
// 6. evaluate_port_available() / check_port_available()
// ---------------------------------------------------------------------------

TEST_CASE("evaluate_port_available inverts probe_tcp_port's pass/fail mapping",
         "[doctor][port]") {
  // probe_tcp_port()==true means "something answered" -- good news for
  // `ready = "port:<n>"`, bad news (a collision) for doctor.
  auto in_use = Mads::Doctor::evaluate_port_available("127.0.0.1", 9092, true);
  REQUIRE(in_use.status == Status::Fail);

  auto free = Mads::Doctor::evaluate_port_available("127.0.0.1", 9092, false);
  REQUIRE(free.status == Status::Pass);
}

TEST_CASE("check_port_available fails when something is already listening",
         "[doctor][port]") {
  const uint16_t port = 42702;
  FakeListener listener(port);
  auto r = Mads::Doctor::check_port_available("127.0.0.1", port, 300ms);
  REQUIRE(r.status == Status::Fail);
  REQUIRE(r.message.find("already in use") != std::string::npos);
}

TEST_CASE("check_port_available passes when nothing is listening", "[doctor][port]") {
  const uint16_t port = 42703; // intentionally nothing bound here
  auto r = Mads::Doctor::check_port_available("127.0.0.1", port, 300ms);
  REQUIRE(r.status == Status::Pass);
  REQUIRE(r.message.find("is free") != std::string::npos);
}
