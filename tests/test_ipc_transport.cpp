// Tests for the ipc:// local transport between Mads::Agent and the broker:
//   - the broker-side helpers in src/detail/broker_ipc.hpp (endpoint
//     resolution from [broker] keys, socket-file probing/ownership, the
//     `transport` reply) that src/main/broker.cpp runs;
//   - Agent::init()'s transport selection (src/agent.cpp _select_transport())
//     against an in-process broker that binds exactly like mads-broker does:
//     XSUB/XPUB proxy and a settings REP socket, each on loopback tcp AND on
//     ipc via bind_broker_ipc();
//   - pub/sub, `-s ipc://...` settings and CURVE + ip_whitelist over ipc.
//
// Every TEST_CASE runs as its own ctest process (catch_discover_tests), so
// each one gets its own ports and its own ipc directory (keyed by the base
// port, kept short for macOS's 104-byte sun_path).
//
// Port range for this file: 44600-44699.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>
#include <zmq.hpp>
#include <zmq_addon.hpp>

#include "agent.hpp"
#include "curve.hpp"
#include "detail/broker_ipc.hpp"
#include "detail/endpoint.hpp"
#include "detail/host_token.hpp"
#include "detail/runtime_dir.hpp"
#include "detail/transport.hpp"
#include "mads_test_helpers.hpp"

#ifndef _WIN32
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

using namespace std::chrono_literals;
namespace fs = std::filesystem;
using namespace Mads::detail;

// ipc:// may be missing on Windows (AF_UNIX support is optional there).
#define REQUIRE_IPC()                                                          \
  do {                                                                         \
    if (!zmq_has_ipc())                                                        \
      SKIP("this libzmq build has no ipc transport");                          \
  } while (0)

namespace {

// The ipc endpoint for `file` in `dir`, joined with the native separator.
std::string ipc_in(fs::path const &dir, std::string const &file) {
  return "ipc://" + (dir / file).string();
}

// A short, per-test ipc directory under the runtime dir, removed on exit.
struct IpcDir {
  fs::path path;
  explicit IpcDir(uint16_t tag) {
    path = runtime_dir() / ("t" + std::to_string(tag));
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path);
  }
  ~IpcDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

fs::path make_key_dir(uint16_t tag) {
  const auto dir = fs::temp_directory_path() /
                   ("mads_test_ipc_keys_" + std::to_string(tag) + "_" +
                    std::to_string(std::chrono::steady_clock::now()
                                       .time_since_epoch()
                                       .count()));
  fs::create_directories(dir);
  for (const auto &name : {std::string("broker"), std::string("client")}) {
    const auto kp = Mads::generate_keypair();
    std::ofstream(dir / (name + ".key")) << kp.secret_key << "\n";
    std::ofstream(dir / (name + ".pub")) << kp.public_key << "\n";
  }
  return dir;
}

std::string tcp_bind(uint16_t port) {
  return "tcp://127.0.0.1:" + std::to_string(port);
}

// A broker that binds like src/main/broker.cpp: frontend = base,
// backend = base + 1, settings = base + 2, each also on ipc when enabled.
struct TestBroker {
  struct Options {
    bool ipc = true;
    bool legacy = false;            // answer `transport` like an old broker
    bool settings_ipc_only = false; // offer ipc for settings only
    std::string host_token;         // override the advertised host token
    fs::path key_dir;               // non-empty: CURVE on every socket
    std::vector<std::string> whitelist;
  };

  uint16_t base;
  IpcDir dir;
  BrokerIpcConfig ipc;
  IpcSocketFile front_file, back_file, settings_file;
  std::string reply;
  std::string settings_body;
  Options opts;

  zmq::context_t ctx;
  std::unique_ptr<Mads::CurveAuth> auth;
  zmq::socket_t frontend{ctx, zmq::socket_type::xsub};
  zmq::socket_t backend{ctx, zmq::socket_type::xpub};
  zmq::socket_t settings{ctx, zmq::socket_type::rep};
  zmq::socket_t controlled{ctx, zmq::socket_type::rep};
  zmq::socket_t controller{ctx, zmq::socket_type::req};
  std::thread proxy_thread, settings_thread;
  std::atomic<bool> running{true};

  TestBroker(uint16_t base_port, std::string body)
      : TestBroker(base_port, std::move(body), Options{}) {}

  TestBroker(uint16_t base_port, std::string body, Options o)
      : base(base_port), dir(base_port), settings_body(std::move(body)),
        opts(std::move(o)) {
    toml::table section;
    section.insert("ipc", opts.ipc);
    section.insert("ipc_dir", dir.path.string());
    toml::table cfg;
    cfg.insert("broker", section);
    ipc = resolve_broker_ipc(cfg["broker"], tcp_bind(base),
                             tcp_bind(base + 1), tcp_bind(base + 2));
    if (opts.settings_ipc_only) {
      ipc.frontend.clear();
      ipc.backend.clear();
    }

    if (!opts.key_dir.empty()) {
      auth = std::make_unique<Mads::CurveAuth>(ctx);
      auth->allowed_ips = opts.whitelist;
      auth->setup_auth(Mads::auth_verbose::off);
      auth->fetch_public_keys(opts.key_dir);
      auth->setup_curve_server(frontend, "broker");
      auth->setup_curve_server(backend, "broker");
      auth->setup_curve_server(settings, "broker");
    }

    std::vector<std::string> warnings;
    frontend.bind(tcp_bind(base));
    bind_broker_ipc(frontend, ipc.frontend, front_file, warnings);
    backend.bind(tcp_bind(base + 1));
    bind_broker_ipc(backend, ipc.backend, back_file, warnings);
    settings.set(zmq::sockopt::rcvtimeo, 100);
    settings.bind(tcp_bind(base + 2));
    bind_broker_ipc(settings, ipc.settings, settings_file, warnings);
    REQUIRE(warnings.empty());

    reply = transport_reply(ipc, auth != nullptr);
    if (!opts.host_token.empty()) {
      auto j = nlohmann::json::parse(reply);
      j["host_token"] = opts.host_token;
      reply = j.dump();
    }

    const std::string ctrl = "inproc://test-ipc-ctrl";
    controlled.bind(ctrl);
    controller.connect(ctrl);
    proxy_thread = std::thread([this] {
      zmq::proxy_steerable(frontend, backend, zmq::socket_ref(), controlled);
    });
    settings_thread = std::thread([this] { serve_settings(); });
  }

  ~TestBroker() {
    running = false;
    settings_thread.join();
    controller.send(zmq::str_buffer("TERMINATE"), zmq::send_flags::none);
    zmq::message_t ack;
    (void)controller.recv(ack);
    proxy_thread.join();
    for (auto *s : {&frontend, &backend, &settings, &controlled, &controller})
      s->close();
    auth.reset();
    ctx.close();
    front_file.remove();
    back_file.remove();
    settings_file.remove();
  }

  std::string tcp_settings_uri() const { return mads_test::loopback(base + 2); }

  // The settings worker of src/main/broker.cpp, minus attachments/clock.
  void serve_settings() {
    while (running) {
      zmq::multipart_t msg;
      if (!msg.recv(settings) || msg.size() < 2)
        continue;
      const std::string cmd = msg.at(1).to_string();
      zmq::multipart_t out;
      if (cmd == "settings") {
        out.addstr(LIB_VERSION);
        out.addstr(settings_body);
      } else if (cmd == "timecode") {
        out.addstr("0");
      } else if (cmd == TRANSPORT_CMD && !opts.legacy) {
        out.addstr(reply);
      } else {
        out.addstr(LIB_VERSION); // broker.cpp's "unexpected command" branch
      }
      out.send(settings);
    }
  }
};

std::string settings_toml(uint16_t base, std::string const &agents_extra = "",
                          std::string const &agent_extra = "") {
  std::ostringstream ss;
  ss << "[agents]\n"
     << "frontend_address = \"tcp://localhost:" << base << "\"\n"
     << "backend_address = \"tcp://localhost:" << base + 1 << "\"\n"
     << "clock_source = \"none\"\n"
     << agents_extra << "\n"
     << "[pubber]\n"
     << "pub_topic = \"ipc_test\"\n"
     << agent_extra << "\n"
     << "[subber]\n"
     << "sub_topic = [\"ipc_test\"]\n"
     << agent_extra << "\n";
  return ss.str();
}

std::unique_ptr<Mads::Agent> make_agent(std::string const &name,
                                        std::string const &uri,
                                        std::string const &mode = "",
                                        fs::path const &key_dir = {}) {
  auto a = std::make_unique<Mads::Agent>(name, uri);
  a->set_settings_timeout(3000);
  if (!mode.empty())
    a->set_transport_mode(mode);
  if (!key_dir.empty()) {
    a->set_key_dir(key_dir);
    a->client_key_name = "client";
    a->server_key_name = "broker";
  }
  a->init(!key_dir.empty(), false);
  return a;
}

// Publishes until the subscriber gets one message, or `budget` runs out.
bool round_trip(Mads::Agent &pub, Mads::Agent &sub,
                std::chrono::milliseconds budget = 4000ms) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    pub.publish(nlohmann::json{{"n", 1}}, "ipc_test");
    for (int i = 0; i < 10; ++i) {
      if (sub.receive(true) == Mads::message_type::json) {
        auto [topic, doc] = sub.last_json();
        return topic == "ipc_test" && doc.at("n") == 1;
      }
      std::this_thread::sleep_for(10ms);
    }
  }
  return false;
}

std::string transport_request(zmq::context_t &ctx, std::string const &uri) {
  zmq::socket_t req(ctx, zmq::socket_type::req);
  req.set(zmq::sockopt::linger, 0);
  req.set(zmq::sockopt::rcvtimeo, 2000);
  req.connect(uri);
  zmq::multipart_t out;
  out.addstr(LIB_VERSION);
  out.addstr(TRANSPORT_CMD);
  out.addstr("probe");
  out.send(req);
  zmq::multipart_t in;
  REQUIRE(in.recv(req));
  REQUIRE(in.size() == 1);
  return in.at(0).to_string();
}

} // namespace

// ---------------------------------------------------------------------------
// Broker-side helpers
// ---------------------------------------------------------------------------

TEST_CASE("resolve_broker_ipc derives per-role endpoints from the tcp ports",
          "[ipc_transport]") {
  REQUIRE_IPC();
  toml::table cfg = toml::parse("[broker]\nipc_dir = \"/tmp/x\"\n");
  auto r = resolve_broker_ipc(cfg["broker"], "tcp://*:9090", "tcp://*:9091",
                              "tcp://*:9092");
  CHECK(r.warnings.empty());
  CHECK(r.frontend == ipc_in("/tmp/x", "broker-9090-front.ipc"));
  CHECK(r.backend == ipc_in("/tmp/x", "broker-9091-back.ipc"));
  CHECK(r.settings == ipc_in("/tmp/x", "broker-9092-settings.ipc"));

  SECTION("ipc = false disables every endpoint") {
    toml::table off = toml::parse("[broker]\nipc = false\n");
    auto d = resolve_broker_ipc(off["broker"], "tcp://*:9090",
                                "tcp://*:9091", "tcp://*:9092");
    CHECK(d.frontend.empty());
    CHECK(d.backend.empty());
    CHECK(d.settings.empty());
    CHECK(d.warnings.empty());
  }
  SECTION("a libzmq without ipc offers nothing, with a warning") {
    auto d = resolve_broker_ipc(cfg["broker"], "tcp://*:9090",
                                "tcp://*:9091", "tcp://*:9092", false);
    CHECK(d.frontend.empty());
    CHECK(d.settings.empty());
    CHECK(d.warnings.size() == 1);
  }
  SECTION("explicit overrides win; a non-ipc override is ignored") {
    toml::table o = toml::parse(
        "[broker]\nipc_dir = \"/tmp/x\"\n"
        "frontend_ipc = \"ipc:///tmp/x/f.ipc\"\n"
        "backend_ipc = \"tcp://*:1\"\n");
    auto d = resolve_broker_ipc(o["broker"], "tcp://*:9090", "tcp://*:9091",
                                "tcp://*:9092");
    CHECK(d.frontend == "ipc:///tmp/x/f.ipc");
    CHECK(d.backend.empty());
    CHECK(d.settings == ipc_in("/tmp/x", "broker-9092-settings.ipc"));
    CHECK(d.warnings.size() == 1);
  }
  SECTION("a tcp address without a numeric port needs an override") {
    auto d = resolve_broker_ipc(cfg["broker"], "tcp://*:*", "tcp://*:9091",
                                "tcp://*:9092");
    CHECK(d.frontend.empty());
    CHECK(d.backend == ipc_in("/tmp/x", "broker-9091-back.ipc"));
    CHECK(d.warnings.size() == 1);
  }
}

#ifndef _WIN32
TEST_CASE("an explicit ipc_dir gets the runtime dir's ownership rules",
          "[ipc_transport]") {
  REQUIRE_IPC();
  SECTION("an own directory is created owner-only") {
    IpcDir parent(44690);
    const fs::path dir = parent.path / "sub";
    toml::table cfg;
    toml::table section;
    section.insert("ipc_dir", dir.string());
    cfg.insert("broker", section);
    auto r = resolve_broker_ipc(cfg["broker"], "tcp://*:9090", "tcp://*:9091",
                                "tcp://*:9092");
    CHECK(r.warnings.empty());
    CHECK(r.frontend == ipc_in(dir, "broker-9090-front.ipc"));
    const auto perms = fs::status(dir).permissions();
    CHECK((perms & (fs::perms::group_all | fs::perms::others_all)) ==
          fs::perms::none);
  }
  SECTION("a directory owned by someone else is refused") {
    if (::getuid() == 0)
      SKIP("running as root: every directory is ours");
    toml::table cfg = toml::parse("[broker]\nipc_dir = \"/\"\n");
    auto r = resolve_broker_ipc(cfg["broker"], "tcp://*:9090", "tcp://*:9091",
                                "tcp://*:9092");
    CHECK(r.frontend.empty());
    CHECK(r.backend.empty());
    CHECK(r.settings.empty());
    REQUIRE(r.warnings.size() == 3);
    CHECK(r.warnings[0].find("not owned by the current user") !=
          std::string::npos);
  }
}

TEST_CASE("bind_broker_ipc replaces a stale socket file but never a live one",
          "[ipc_transport]") {
  REQUIRE_IPC();
  IpcDir dir(44600);
  const std::string path = (dir.path / "s.ipc").string();
  const std::string endpoint = "ipc://" + path;

  // A stale socket file: bound by a raw AF_UNIX socket that is then closed
  // without unlinking, exactly what a crashed broker leaves behind.
  {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size());
    REQUIRE(::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) ==
            0);
    ::close(fd);
  }
  REQUIRE(probe_ipc_socket(path) == IpcSocketState::Stale);

  zmq::context_t ctx;
  zmq::socket_t first(ctx, zmq::socket_type::rep);
  std::string ep1 = endpoint;
  IpcSocketFile file1;
  std::vector<std::string> warnings;
  REQUIRE(bind_broker_ipc(first, ep1, file1, warnings));
  CHECK(ep1 == endpoint);
  CHECK(warnings.empty());
  CHECK(probe_ipc_socket(path) == IpcSocketState::Live);

  // A second broker configured with the same path must not steal it.
  zmq::socket_t second(ctx, zmq::socket_type::rep);
  std::string ep2 = endpoint;
  IpcSocketFile file2;
  CHECK_FALSE(bind_broker_ipc(second, ep2, file2, warnings));
  CHECK(ep2.empty());
  CHECK(warnings.size() == 1);

  // The first broker is still reachable over the path.
  zmq::socket_t req(ctx, zmq::socket_type::req);
  req.set(zmq::sockopt::rcvtimeo, 2000);
  req.connect(endpoint);
  req.send(zmq::str_buffer("ping"), zmq::send_flags::none);
  zmq::message_t m;
  REQUIRE(first.recv(m));
  first.send(zmq::str_buffer("pong"), zmq::send_flags::none);
  REQUIRE(req.recv(m));
  CHECK(m.to_string() == "pong");

  req.close();
  first.close();
  second.close();
  ctx.close();
  // libzmq 4.3.5 leaves the file behind on close: removing it is ours.
  CHECK(socket_file_exists(path));
  file2.remove(); // never claimed: a no-op
  CHECK(socket_file_exists(path));
  file1.remove();
  CHECK_FALSE(socket_file_exists(path));
}

TEST_CASE("bind_broker_ipc refuses to replace a file that is not a socket",
          "[ipc_transport]") {
  REQUIRE_IPC();
  IpcDir dir(44603);
  const std::string path = (dir.path / "f.ipc").string();
  std::ofstream(path) << "precious";
  zmq::context_t ctx;
  zmq::socket_t s(ctx, zmq::socket_type::rep);
  std::string ep = "ipc://" + path;
  IpcSocketFile file;
  std::vector<std::string> warnings;
  CHECK_FALSE(bind_broker_ipc(s, ep, file, warnings));
  CHECK(ep.empty());
  CHECK(fs::file_size(path) == 8);
  s.close();
}
#endif

TEST_CASE("the broker's transport reply offers its bound ipc endpoints",
          "[ipc_transport]") {
  REQUIRE_IPC();
  TestBroker broker(44606, settings_toml(44606));
  zmq::context_t ctx;
  auto info = parse_transport_reply(
      transport_request(ctx, broker.tcp_settings_uri()));
  REQUIRE(info);
  CHECK(info->host_token == host_token());
  CHECK(info->offers_ipc());
  CHECK(info->frontend_ipc == broker.ipc.frontend);
  CHECK(info->backend_ipc == broker.ipc.backend);
  CHECK(info->settings_ipc == broker.ipc.settings);
  CHECK_FALSE(info->curve);
  CHECK(socket_file_exists(parse_endpoint(info->frontend_ipc)->path));

  // The same command answered over the ipc settings endpoint itself.
  auto again =
      parse_transport_reply(transport_request(ctx, broker.ipc.settings));
  REQUIRE(again);
  CHECK(again->frontend_ipc == info->frontend_ipc);
}

TEST_CASE("with [broker] ipc = false the transport reply offers no ipc",
          "[ipc_transport]") {
  REQUIRE_IPC();
  TestBroker::Options o;
  o.ipc = false;
  TestBroker broker(44609, settings_toml(44609), o);
  zmq::context_t ctx;
  auto info = parse_transport_reply(
      transport_request(ctx, broker.tcp_settings_uri()));
  REQUIRE(info);
  CHECK(info->host_token == host_token());
  CHECK_FALSE(info->offers_ipc());
  CHECK(info->settings_ipc.empty());
}

// ---------------------------------------------------------------------------
// Agent transport selection
// ---------------------------------------------------------------------------

TEST_CASE("an agent auto-selects ipc when the broker is on this host",
          "[ipc_transport]") {
  REQUIRE_IPC();
  mads_test::RunningGuard guard;
  TestBroker broker(44612, settings_toml(44612));
  auto a = make_agent("pubber", broker.tcp_settings_uri());
  CHECK(a->transport() == "ipc");
  CHECK(a->transport_mode() == "auto");
  CHECK(a->pub_endpoint() == broker.ipc.frontend);
  CHECK(a->sub_endpoint() == broker.ipc.backend);
}

TEST_CASE("transport = tcp keeps an agent on tcp; precedence is CLI > agent "
          "section > [agents]",
          "[ipc_transport]") {
  REQUIRE_IPC();
  mads_test::RunningGuard guard;
  SECTION("[agents] transport = tcp") {
    TestBroker broker(44615, settings_toml(44615, "transport = \"tcp\""));
    auto a = make_agent("pubber", broker.tcp_settings_uri());
    CHECK(a->transport() == "tcp");
    CHECK(a->transport_mode() == "tcp");
    CHECK(a->pub_endpoint() == "tcp://127.0.0.1:44615");
    CHECK(a->sub_endpoint() == "tcp://127.0.0.1:44616");
  }
  SECTION("the agent section wins over [agents]") {
    TestBroker broker(44618, settings_toml(44618, "transport = \"tcp\"",
                                           "transport = \"auto\""));
    auto a = make_agent("pubber", broker.tcp_settings_uri());
    CHECK(a->transport() == "ipc");
  }
  SECTION("set_transport_mode() wins over the settings") {
    TestBroker broker(44621, settings_toml(44621, "", "transport = \"ipc\""));
    auto a = make_agent("pubber", broker.tcp_settings_uri(), "tcp");
    CHECK(a->transport() == "tcp");
    CHECK(a->pub_endpoint() == "tcp://127.0.0.1:44621");
  }
  SECTION("an invalid mode is rejected") {
    Mads::Agent a("pubber", "none");
    CHECK_THROWS_AS(a.set_transport_mode("udp"), Mads::AgentError);
  }
}

TEST_CASE("transport = ipc throws when the broker offers no ipc",
          "[ipc_transport]") {
  REQUIRE_IPC();
  mads_test::RunningGuard guard;
  TestBroker::Options o;
  o.ipc = false;
  TestBroker broker(44624, settings_toml(44624), o);
  CHECK_THROWS_AS(make_agent("pubber", broker.tcp_settings_uri(), "ipc"),
                  Mads::AgentError);
  // auto mode quietly stays on tcp
  auto a = make_agent("pubber", broker.tcp_settings_uri());
  CHECK(a->transport() == "tcp");
}

TEST_CASE("a legacy broker (empty transport reply) leaves agents on tcp",
          "[ipc_transport]") {
  REQUIRE_IPC();
  mads_test::RunningGuard guard;
  TestBroker::Options o;
  o.legacy = true;
  TestBroker broker(44627, settings_toml(44627), o);
  auto a = make_agent("pubber", broker.tcp_settings_uri());
  CHECK(a->transport() == "tcp");
  CHECK(a->pub_endpoint() == "tcp://127.0.0.1:44627");
  CHECK_THROWS_AS(make_agent("subber", broker.tcp_settings_uri(), "ipc"),
                  Mads::AgentError);
}

TEST_CASE("a broker with another host token is reached over tcp",
          "[ipc_transport]") {
  REQUIRE_IPC();
  mads_test::RunningGuard guard;
  TestBroker::Options o;
  o.host_token = "elsewhere/0000";
  TestBroker broker(44630, settings_toml(44630), o);
  auto a = make_agent("pubber", broker.tcp_settings_uri());
  CHECK(a->transport() == "tcp");
}

TEST_CASE("pub/sub round trip over ipc through the broker",
          "[ipc_transport]") {
  REQUIRE_IPC();
  mads_test::RunningGuard guard;
  TestBroker broker(44633, settings_toml(44633));
  auto sub = make_agent("subber", broker.tcp_settings_uri());
  auto pub = make_agent("pubber", broker.tcp_settings_uri());
  REQUIRE(sub->transport() == "ipc");
  REQUIRE(pub->transport() == "ipc");
  sub->connect();
  // The publisher's handshake wait (SocketMonitor) must see the ipc
  // handshake: well under the 2 s worst case of a blind wait.
  const auto t0 = std::chrono::steady_clock::now();
  pub->connect(2000ms);
  CHECK(std::chrono::steady_clock::now() - t0 < 1500ms);
  CHECK(pub->link_state().status == Mads::LinkStatus::Up);
  CHECK(round_trip(*pub, *sub));
  const bool sub_up = mads_test::wait_for(
      [&] { return sub->link_state().status == Mads::LinkStatus::Up; });
  CHECK(sub_up);
}

TEST_CASE("settings fetched from an ipc:// settings URI",
          "[ipc_transport]") {
  REQUIRE_IPC();
  mads_test::RunningGuard guard;
  SECTION("data endpoints come from the transport reply") {
    TestBroker broker(44636, settings_toml(44636));
    auto a = make_agent("pubber", broker.ipc.settings);
    CHECK_FALSE(a->settings_are_local());
    CHECK(a->transport() == "ipc");
    CHECK(a->pub_endpoint() == broker.ipc.frontend);
    CHECK(a->sub_endpoint() == broker.ipc.backend);
  }
  SECTION("transport = tcp uses the configured addresses verbatim") {
    TestBroker broker(44639, settings_toml(44639));
    auto a = make_agent("pubber", broker.ipc.settings, "tcp");
    CHECK(a->transport() == "tcp");
    CHECK(a->pub_endpoint() == "tcp://localhost:44639");
  }
  SECTION("no data ipc offered is an error") {
    TestBroker::Options o;
    o.settings_ipc_only = true;
    TestBroker broker(44642, settings_toml(44642), o);
    CHECK_THROWS_AS(make_agent("pubber", broker.ipc.settings),
                    Mads::AgentError);
  }
}

TEST_CASE("CURVE over ipc with an ip_whitelist configured",
          "[ipc_transport][curve]") {
  REQUIRE_IPC();
  mads_test::RunningGuard guard;
  const auto keys = make_key_dir(44645);
  SECTION("settings over whitelisted tcp, data over ipc") {
    TestBroker::Options o;
    o.key_dir = keys;
    o.whitelist = {"127.0.0.1"};
    TestBroker broker(44645, settings_toml(44645), o);
    auto sub = make_agent("subber", broker.tcp_settings_uri(), "", keys);
    auto pub = make_agent("pubber", broker.tcp_settings_uri(), "", keys);
    REQUIRE(pub->transport() == "ipc");
    sub->connect();
    pub->connect(1000ms);
    CHECK(round_trip(*pub, *sub));
  }
  SECTION("ipc peers pass a whitelist that excludes every local address") {
    TestBroker::Options o;
    o.key_dir = keys;
    o.whitelist = {"10.255.255.1"};
    TestBroker broker(44648, settings_toml(44648), o);
    auto sub = make_agent("subber", broker.ipc.settings, "", keys);
    auto pub = make_agent("pubber", broker.ipc.settings, "", keys);
    REQUIRE(pub->transport() == "ipc");
    sub->connect();
    pub->connect(1000ms);
    CHECK(round_trip(*pub, *sub));
    // ...while the same whitelist still refuses loopback tcp.
    Mads::Agent tcp_agent("pubber", broker.tcp_settings_uri());
    tcp_agent.set_settings_timeout(500);
    tcp_agent.set_key_dir(keys);
    tcp_agent.client_key_name = "client";
    tcp_agent.server_key_name = "broker";
    CHECK_THROWS_AS(tcp_agent.init(true, false), Mads::AgentError);
  }
  std::error_code ec;
  fs::remove_all(keys, ec);
}
