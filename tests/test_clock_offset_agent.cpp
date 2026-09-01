// Integration tests for Mads::Agent's clock-offset measurement
// (src/agent.cpp: measure_clock_offset(), broadcast_clock_probe(),
// clock_offset(), the CLOCKSYNC_TOPIC ping/pong/announce protocol) and the
// broker's "clock" command (src/main/broker.cpp).
//
// Two fixtures, both in-process (no subprocess, no real mads-broker):
//   - FakeSettingsBroker: a REP socket answering "settings"/"clock"/"timecode"
//     exactly like broker.cpp's settings worker, following the same pattern
//     as test_agent_broker.cpp's own FakeBroker (that file's header comment
//     documents the wire protocol this mirrors for "settings"/"timecode";
//     "clock" is transcribed from src/main/broker.cpp's own branch).
//   - BusProxy: a steerable XSUB/XPUB proxy (zmq::proxy_steerable), the same
//     pattern as test_broker_steering.cpp's ProxyHarness, giving real Agent
//     instances a real N-to-N pub/sub fan-out for CLOCKSYNC_TOPIC without
//     needing the full mads-broker executable.
//
// Port range for this file: 44500-44599.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <zmq.hpp>
#include <zmq_addon.hpp>

#include "agent.hpp"
#include "mads_test_helpers.hpp"

using namespace std::chrono_literals;
using Mads::Agent;
using Mads::ClockOffsetResult;
using Mads::ClockSource;

namespace {

// ---------------------------------------------------------------------------
// FakeSettingsBroker: REP-only, "settings" + "clock" (+ a generic fallback
// mirroring broker.cpp's "unexpected command" branch, which is what makes
// clock_supported=false simulate a pre-"clock" broker).
// ---------------------------------------------------------------------------
class FakeSettingsBroker {
public:
  explicit FakeSettingsBroker(uint16_t port)
      : _ctx(), _sock(_ctx, zmq::socket_type::rep) {
    _sock.set(zmq::sockopt::rcvtimeo, 100);
    _sock.bind(mads_test::loopback(port));
  }
  ~FakeSettingsBroker() { stop(); }

  std::string settings_body;
  // false reproduces an old broker that does not know "clock": the reply
  // falls into the generic branch and is a bare [LIB_VERSION], exactly as
  // src/main/broker.cpp's own "else" branch does today.
  bool clock_supported = true;

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
      if (msg.size() < 2) continue;
      const std::string kind = msg.at(1).to_string();
      zmq::multipart_t reply;
      if (kind == "settings") {
        reply.addstr(std::string(LIB_VERSION));
        reply.addstr(settings_body);
        reply.send(_sock);
      } else if (kind == "timecode") {
        reply.addstr(std::string("0"));
        reply.send(_sock);
      } else if (kind == "clock" && clock_supported) {
        const int64_t t2 =
            Mads::epoch_us(std::chrono::system_clock::now());
        const int64_t t3 =
            Mads::epoch_us(std::chrono::system_clock::now());
        reply.addstr(std::string(LIB_VERSION));
        reply.addstr(std::to_string(t2));
        reply.addstr(std::to_string(t3));
        reply.send(_sock);
      } else {
        reply.addstr(std::string(LIB_VERSION));
        reply.send(_sock);
      }
    }
  }

  zmq::context_t _ctx;
  zmq::socket_t _sock;
  std::thread _thread;
  std::atomic<bool> _stopped{false};
};

std::string make_settings_toml(const std::string &name, uint16_t front_port,
                               uint16_t back_port,
                               const std::string &extra_agent_lines = "") {
  std::ostringstream ss;
  ss << "[agents]\n"
     << "timecode_fps = 25\n"
     << "frontend_address = \"tcp://localhost:" << front_port << "\"\n"
     << "backend_address = \"tcp://localhost:" << back_port << "\"\n"
     << "\n[" << name << "]\n"
     << "pub_topic = \"" << name << "\"\n"
     << "sub_topic = [\"\"]\n"
     << extra_agent_lines;
  return ss.str();
}

// ---------------------------------------------------------------------------
// BusProxy: real XSUB/XPUB fan-out, same shape as test_broker_steering.cpp's
// ProxyHarness (frontend = where publishers connect, backend = where
// subscribers connect), minus the steerable control channel this suite
// doesn't need.
// ---------------------------------------------------------------------------
struct BusProxy {
  zmq::context_t ctx;
  zmq::socket_t frontend{ctx, zmq::socket_type::xsub};
  zmq::socket_t backend{ctx, zmq::socket_type::xpub};
  // Steerable, not plain zmq::proxy(): a blocking zmq::proxy() only
  // unblocks once frontend/backend are *closed*, but those are BusProxy's
  // own members and would not close until this destructor's body already
  // returned -- a real deadlock, caught the hard way (see git history).
  // TERMINATE is the documented, working way to stop it instead (mirrors
  // test_broker_steering.cpp's ProxyHarness).
  zmq::socket_t controlled{ctx, zmq::socket_type::rep};
  zmq::socket_t controller{ctx, zmq::socket_type::req};
  std::thread thread;

  BusProxy(uint16_t front_port, uint16_t back_port) {
    frontend.bind(mads_test::loopback(front_port));
    backend.bind(mads_test::loopback(back_port));
    controlled.bind("inproc://clock-offset-test-proxy-ctrl");
    controller.connect("inproc://clock-offset-test-proxy-ctrl");
    thread = std::thread([this] {
      zmq::proxy_steerable(frontend, backend, zmq::socket_ref(), controlled);
    });
  }

  ~BusProxy() {
    controller.send(zmq::buffer(std::string("TERMINATE")),
                    zmq::send_flags::none);
    zmq::multipart_t reply;
    reply.recv(controller);
    if (thread.joinable()) thread.join();
  }
};

// An Agent (name, over `settings_uri`) whose clock config is set via TOML
// [agents] lines, connected and ready. Uses install_watchdog=false like
// every other Agent-heavy test in this suite (no CLI/process lifetime to
// guard here). connect()'s default slow-joiner grace (not 0ms) matters
// here: _start_clock_thread()'s very first announce goes out right after
// connect() returns, and a PUB socket silently drops anything sent before
// the XSUB/XPUB proxy has forwarded this agent's own CLOCKSYNC_TOPIC
// subscription back to it -- an announce lost to that window would not be
// retried until clock_announce_ms (5s default) later.
std::unique_ptr<Agent> make_agent(const std::string &name,
                                  const std::string &settings_uri) {
  auto a = std::make_unique<Agent>(name, settings_uri);
  a->init(false, false);
  a->connect();
  return a;
}

} // namespace

// ---------------------------------------------------------------------------
// measure_clock_offset() (source A) against FakeSettingsBroker
// ---------------------------------------------------------------------------

TEST_CASE("measure_clock_offset() against a live settings broker returns a "
          "valid, small-offset, positive-delay result",
          "[clock_offset_agent]") {
  mads_test::RunningGuard guard;
  const uint16_t settings_port = 44500;
  const uint16_t front_port = 44501, back_port = 44502;
  FakeSettingsBroker broker(settings_port);
  broker.settings_body = make_settings_toml("csrc", front_port, back_port);
  broker.start();

  Agent a("csrc", mads_test::loopback(settings_port));
  a.init(false, false);

  auto r = a.measure_clock_offset(/*samples=*/3, /*timeout_ms=*/1000);
  REQUIRE(r.valid);
  CHECK(r.samples >= 1);
  CHECK(r.delay_us >= 0);
  // Same process/machine on both ends of the loopback REQ/REP: the true
  // offset is exactly 0, so a generous bound catches only real breakage,
  // not ordinary measurement noise on a loaded CI runner.
  CHECK(std::abs(r.offset_us) < 5000);
  CHECK(r.source == ClockSource::Broker);
  CHECK(r.hops == 0);
  CHECK_FALSE(r.origin_agent_id.empty());

  // clock_offset() reflects the same measurement (no domain consensus peer
  // exists yet, so it falls back to this agent's own reading).
  auto adopted = a.clock_offset();
  REQUIRE(adopted.valid);
  CHECK(adopted.offset_us == r.offset_us);
}

TEST_CASE("measure_clock_offset() degrades gracefully against a broker that "
          "does not know \"clock\"",
          "[clock_offset_agent]") {
  mads_test::RunningGuard guard;
  const uint16_t settings_port = 44510;
  const uint16_t front_port = 44511, back_port = 44512;
  FakeSettingsBroker broker(settings_port);
  broker.settings_body = make_settings_toml("cold", front_port, back_port);
  broker.clock_supported = false; // bare [LIB_VERSION] reply, < 3 frames
  broker.start();

  Agent a("cold", mads_test::loopback(settings_port));
  a.init(false, false); // init() itself calls measure_clock_offset() once

  auto r = a.clock_offset();
  CHECK_FALSE(r.valid);

  // A direct call reports the same, and never throws.
  auto r2 = a.measure_clock_offset(3, 500);
  CHECK_FALSE(r2.valid);
}

TEST_CASE("measure_clock_offset() against an unreachable broker returns "
          "invalid without throwing",
          "[clock_offset_agent]") {
  mads_test::RunningGuard guard;
  // Nothing is listening on this port.
  Agent a("cunreach", mads_test::loopback(44520));
  // settings_are_local() is false ("tcp://" is in the URI) but there's no
  // fetch_settings() call here, so measure_clock_offset() is exercised
  // directly against a REQ socket that will time out.
  ClockOffsetResult r;
  REQUIRE_NOTHROW(r = a.measure_clock_offset(2, 200));
  CHECK_FALSE(r.valid);
}

TEST_CASE("measure_clock_offset() against local settings (no broker) "
          "returns invalid without throwing",
          "[clock_offset_agent]") {
  mads_test::RunningGuard guard;
  Agent a("clocal", "none");
  a.init(false, false);
  auto r = a.clock_offset();
  CHECK_FALSE(r.valid); // settings_are_local(): init() skips measurement
  ClockOffsetResult r2;
  REQUIRE_NOTHROW(r2 = a.measure_clock_offset());
  CHECK_FALSE(r2.valid);
}

// ---------------------------------------------------------------------------
// Bus round trip (source B) over a real XSUB/XPUB proxy
// ---------------------------------------------------------------------------

TEST_CASE("broadcast_clock_probe() receives a well-formed pong from a live "
          "responder, with clock_ref echoed back",
          "[clock_offset_agent]") {
  mads_test::RunningGuard guard;
  const uint16_t settings_port = 44530;
  const uint16_t front_port = 44531, back_port = 44532;
  FakeSettingsBroker broker(settings_port);
  // queue_size = 1 selects LKV delivery, which hands the subscriber socket
  // to the agent's own _io_thread (Agent::_start_io_thread()). That thread
  // is what actually answers a ping for this agent, exactly as it would
  // for any real LKV/threaded-remote-control agent -- a plain-delivery
  // agent only processes CLOCKSYNC_TOPIC messages when something calls its
  // receive(), which nothing does here since this responder is otherwise
  // idle. This also exercises the io_thread's own clocksync interception
  // (the "third site" in Agent::_start_io_thread()), not just receive()'s.
  broker.settings_body = make_settings_toml("responder", front_port,
                                            back_port, "queue_size = 1\n");
  broker.start();
  BusProxy proxy(front_port, back_port);

  auto responder = make_agent("responder", mads_test::loopback(settings_port));
  // Give the responder's own broker-source measurement (init()'s automatic
  // call) something to have adopted before it is probed.
  REQUIRE(responder->clock_offset().valid);

  FakeSettingsBroker broker2(44533);
  broker2.settings_body =
      make_settings_toml("initiator", front_port, back_port);
  broker2.start();
  auto initiator =
      make_agent("initiator", mads_test::loopback(44533));

  // Slow-joiner grace for the XPUB subscription (CLOCKSYNC_TOPIC) to
  // propagate back through the proxy before the probe is sent.
  std::this_thread::sleep_for(300ms);

  auto peers = initiator->broadcast_clock_probe(800ms);
  REQUIRE_FALSE(peers.empty());
  bool found = false;
  for (auto const &p : peers) {
    if (p.responder_agent_id.find("responder") != std::string::npos) {
      found = true;
      CHECK(p.responder_name == "responder");
      CHECK(p.sample.t1 > 0);
      CHECK(p.sample.t2 > 0);
      CHECK(p.sample.t3 > 0);
      CHECK(p.sample.t4 > 0);
      REQUIRE(p.responder_adopted.valid);
      CHECK(p.responder_adopted.source == ClockSource::Broker);
      CHECK(p.responder_adopted.hops == 0);
    }
  }
  CHECK(found);
}

// ---------------------------------------------------------------------------
// Same-host agreement (§1.2 of the design): the property the whole domain-
// consensus mechanism exists to provide.
// ---------------------------------------------------------------------------

TEST_CASE("agents sharing a clock domain converge on one identical adopted "
          "offset",
          "[clock_offset_agent]") {
  mads_test::RunningGuard guard;
  const uint16_t front_port = 44541, back_port = 44542;
  BusProxy proxy(front_port, back_port);

  // Three independent settings brokers (distinct ports; each Agent needs
  // its own settings URI) but ONE shared bus (front_port/back_port) and --
  // critically -- one shared process, so Mads::detail::clock_domain_id()
  // returns the exact same value for all three, exactly like several
  // agents on one real host.
  std::vector<std::unique_ptr<FakeSettingsBroker>> brokers;
  std::vector<std::unique_ptr<Agent>> agents;
  const std::vector<std::string> names = {"a1", "a2", "a3"};
  uint16_t settings_port = 44550;
  for (auto const &name : names) {
    auto b = std::make_unique<FakeSettingsBroker>(settings_port);
    // LKV (queue_size = 1) hands each agent's subscriber to its own
    // _io_thread, which is what actually processes the other two agents'
    // announcements while this test's main thread just polls
    // clock_offset() -- otherwise nothing on any of the three agents
    // would ever call receive() and consensus could never converge.
    // clock_announce_ms is lowered from its 5s default so that the one
    // announce most likely to be lost to the PUB/SUB slow-joiner window
    // (the very first, sent right after connect()) is retried quickly
    // rather than only every 5 seconds.
    b->settings_body = make_settings_toml(
        name, front_port, back_port,
        "queue_size = 1\nclock_announce_ms = 200\n");
    b->start();
    brokers.push_back(std::move(b));
    agents.push_back(make_agent(name, mads_test::loopback(settings_port)));
    ++settings_port;
  }

  // Every agent measured its own offset at init() and announced it; give
  // the announcements time to propagate and each agent's local
  // ClockConsensus time to converge (retried rather than one fixed sleep,
  // to absorb scheduler jitter without over-waiting on a fast machine).
  auto domain = agents.front()->clock_domain();
  for (auto const &a : agents) {
    CHECK(a->clock_domain() == domain);
  }

  std::string expected_ref;
  bool converged = false;
  auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    bool all_valid = true, all_match = true;
    std::string ref;
    for (size_t i = 0; i < agents.size(); ++i) {
      auto r = agents[i]->clock_offset();
      if (!r.valid) {
        all_valid = false;
        break;
      }
      if (i == 0) {
        ref = r.clock_ref();
      } else if (r.clock_ref() != ref) {
        all_match = false;
      }
    }
    if (all_valid && all_match) {
      expected_ref = ref;
      converged = true;
      break;
    }
    std::this_thread::sleep_for(100ms);
  }

  REQUIRE(converged);
  // Every agent's adopted offset_us -- not just its clock_ref -- must be
  // byte-identical: that is the actual guarantee (§1.2), clock_ref is
  // just how a consumer verifies it.
  int64_t expected_offset = agents.front()->clock_offset().offset_us;
  for (auto const &a : agents) {
    auto r = a->clock_offset();
    CHECK(r.clock_ref() == expected_ref);
    CHECK(r.offset_us == expected_offset);
  }
}
