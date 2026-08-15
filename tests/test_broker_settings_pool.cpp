// Pins the libzmq mechanics ZMQ_DEVELOPMENT.md §3.6's ROUTER settings
// endpoint depends on: a ROUTER<->DEALER proxy fanning requests out to a
// pool of REP workers, so N concurrent REQ clients (the same socket type
// Agent::query_broker() uses) all get answered, and -- the actual point --
// a slow request occupying one worker does not delay a concurrent request
// answered by another.
//
// src/main/broker.cpp's settings pool lives in an executable target, not a
// linkable library, so (exactly like test_broker_steering.cpp and
// test_broker_subscriptions.cpp before it) this file mirrors the relevant
// mechanics locally rather than including broker.cpp.
//
// Port range for this file: 44200-44249.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <zmq.hpp>
#include <zmq_addon.hpp>

#include "mads_test_helpers.hpp"

using namespace std::chrono_literals;

namespace {

void run_proxy(zmq::socket_t &router, zmq::socket_t &dealer,
               zmq::socket_t &ctrl) {
  zmq::proxy_steerable(router, dealer, zmq::socket_ref(), ctrl);
}

zmq::multipart_t steer(zmq::socket_t &controller, std::string_view command) {
  controller.send(zmq::buffer(command), zmq::send_flags::none);
  zmq::multipart_t reply;
  reply.recv(controller);
  return reply;
}

// A ROUTER (external) <-proxy-> DEALER (inproc) <- N REP workers, mirroring
// src/main/broker.cpp's settings endpoint. Each worker echoes the request
// body back, except that a request body of "slow" sleeps first -- standing
// in for the real broker's disk read of a plugin attachment inside the
// handler loop, which is exactly what motivated moving off a single REP.
struct SettingsPoolHarness {
  zmq::context_t ctx;
  zmq::socket_t router{ctx, zmq::socket_type::router};
  zmq::socket_t dealer{ctx, zmq::socket_type::dealer};
  zmq::socket_t controlled{ctx, zmq::socket_type::rep};
  zmq::socket_t controller{ctx, zmq::socket_type::req};
  std::thread proxy_thread;
  std::vector<std::thread> workers;
  std::atomic<bool> running{true};

  SettingsPoolHarness(uint16_t router_port, int worker_count,
                      std::chrono::milliseconds slow_delay) {
    router.bind(mads_test::loopback(router_port));
    dealer.bind("inproc://test-settings-pool-workers");
    controlled.bind("inproc://test-settings-pool-ctrl");
    controller.connect("inproc://test-settings-pool-ctrl");
    proxy_thread = std::thread(run_proxy, std::ref(router), std::ref(dealer),
                               std::ref(controlled));

    for (int i = 0; i < worker_count; ++i) {
      workers.emplace_back([this, slow_delay] {
        zmq::socket_t worker(ctx, zmq::socket_type::rep);
        worker.set(zmq::sockopt::rcvtimeo, 200);
        worker.connect("inproc://test-settings-pool-workers");
        while (running.load()) {
          zmq::multipart_t msg;
          if (!msg.recv(worker)) continue;
          const std::string body = msg.at(0).to_string();
          if (body == "slow") std::this_thread::sleep_for(slow_delay);
          zmq::multipart_t reply;
          reply.addstr(body);
          reply.send(worker);
        }
        worker.close();
      });
    }
  }

  ~SettingsPoolHarness() {
    running.store(false);
    for (auto &t : workers) t.join();
    steer(controller, "TERMINATE");
    proxy_thread.join();
    router.close();
    dealer.close();
    controller.close();
    controlled.close();
  }
};

// One REQ round trip, matching Agent::query_broker()'s socket type. Returns
// the elapsed wall-clock time and whether a reply came back at all.
struct RoundTrip {
  bool ok;
  std::chrono::milliseconds elapsed;
};

RoundTrip request(zmq::context_t &ctx, uint16_t port, std::string const &body,
                  int timeout_ms = 5000) {
  zmq::socket_t req(ctx, zmq::socket_type::req);
  req.set(zmq::sockopt::linger, 0);
  req.set(zmq::sockopt::rcvtimeo, timeout_ms);
  req.set(zmq::sockopt::sndtimeo, timeout_ms);
  req.connect(mads_test::loopback(port));

  const auto start = std::chrono::steady_clock::now();
  zmq::multipart_t out;
  out.addstr(body);
  out.send(req);
  zmq::multipart_t in;
  const bool ok = in.recv(req);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
  req.close();
  return {ok, elapsed};
}

} // namespace

TEST_CASE("N concurrent REQ clients against the pool all get answered",
          "[broker_settings_pool]") {
  SettingsPoolHarness h(44201, 3, 0ms);
  std::this_thread::sleep_for(200ms); // let workers connect

  std::vector<std::thread> clients;
  std::vector<RoundTrip> results(6);
  for (int i = 0; i < 6; ++i) {
    clients.emplace_back([&, i] {
      results[i] = request(h.ctx, 44201, "req" + std::to_string(i));
    });
  }
  for (auto &t : clients) t.join();

  for (auto const &r : results) REQUIRE(r.ok);
}

TEST_CASE("a slow request does not delay a concurrent fast one",
          "[broker_settings_pool]") {
  // Two workers: one gets tied up by the slow request, the other is free to
  // answer the fast one immediately. This is the actual point of the pool --
  // on a single REP (the pre-§3.6 broker), the fast request would have
  // waited behind the slow one.
  SettingsPoolHarness h(44202, 2, 800ms);
  std::this_thread::sleep_for(200ms);

  RoundTrip slow_result{}, fast_result{};
  std::thread slow_thread([&] { slow_result = request(h.ctx, 44202, "slow"); });
  std::this_thread::sleep_for(100ms); // let the slow request claim its worker
  std::thread fast_thread([&] { fast_result = request(h.ctx, 44202, "fast"); });

  slow_thread.join();
  fast_thread.join();

  REQUIRE(slow_result.ok);
  REQUIRE(fast_result.ok);
  // Generous bound: the fast reply must come back well under the slow
  // worker's 800ms delay, not after it.
  REQUIRE(fast_result.elapsed < 500ms);
}

TEST_CASE("with a single worker, a slow request does delay a concurrent one",
          "[broker_settings_pool]") {
  // Control case: pins that the *pool*, not something else, is what removes
  // the head-of-line blocking above -- with worker_count=1 (the pre-§3.6
  // behaviour), the fast request must wait out the slow one.
  SettingsPoolHarness h(44203, 1, 500ms);
  std::this_thread::sleep_for(200ms);

  RoundTrip slow_result{}, fast_result{};
  std::thread slow_thread([&] { slow_result = request(h.ctx, 44203, "slow"); });
  std::this_thread::sleep_for(100ms);
  std::thread fast_thread([&] { fast_result = request(h.ctx, 44203, "fast"); });

  slow_thread.join();
  fast_thread.join();

  REQUIRE(slow_result.ok);
  REQUIRE(fast_result.ok);
  REQUIRE(fast_result.elapsed > 300ms);
}
