// Pins the libzmq mechanics ZMQ_DEVELOPMENT.md §2.2's opt-in subscription
// table depends on: a PUB capture socket wired into zmq::proxy_steerable()
// alongside ZMQ_XPUB_VERBOSER on the backend delivers exactly the
// subscribe/unsubscribe notification frames (and nothing else) needed to
// build a topic -> subscriber-count table, without the broker ever
// inspecting ordinary data payloads. Plain VERBOSE is not enough: it only
// forwards every individual *subscribe*, still collapsing unsubscribes down
// to a topic's final 1->0 transition (see the second TEST_CASE below).
//
// src/main/broker.cpp's SubscriptionTable class itself lives in an
// executable target, not a linkable library, so (exactly like
// test_broker_steering.cpp does for PAUSE/RESUME/STATISTICS) this file
// mirrors the relevant mechanics locally rather than including broker.cpp.
//
// Port range for this file: 44150-44199.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <zmq.hpp>
#include <zmq_addon.hpp>

#include "mads_test_helpers.hpp"

using namespace std::chrono_literals;

namespace {

void run_proxy(zmq::socket_t &frontend, zmq::socket_t &backend,
               zmq::socket_t &ctrl, zmq::socket_ref capture) {
  zmq::proxy_steerable(frontend, backend, capture, ctrl);
}

zmq::multipart_t steer(zmq::socket_t &controller, std::string_view command) {
  controller.send(zmq::buffer(command), zmq::send_flags::none);
  zmq::multipart_t reply;
  reply.recv(controller);
  return reply;
}

// A steerable XSUB/XPUB proxy with subscription capture wired in exactly as
// src/main/broker.cpp's SubscriptionTable does: XPUB_VERBOSE on the backend,
// a bounded-HWM PUB capture socket passed to the proxy, and a reader thread
// decoding the 1-frame [flag][topic] notifications into a refcounted table.
struct SubscribingProxyHarness {
  zmq::context_t ctx;
  zmq::socket_t frontend{ctx, zmq::socket_type::xsub};
  zmq::socket_t backend{ctx, zmq::socket_type::xpub};
  zmq::socket_t controlled{ctx, zmq::socket_type::rep};
  zmq::socket_t controller{ctx, zmq::socket_type::req};
  zmq::socket_t capture{ctx, zmq::socket_type::pub};
  zmq::socket_t pub{ctx, zmq::socket_type::pub};
  zmq::socket_t sub{ctx, zmq::socket_type::sub};
  std::thread proxy_thread;
  std::thread reader_thread;
  std::atomic<bool> reader_running{true};
  std::mutex table_mutex;
  std::map<std::string, int> counts;

  SubscribingProxyHarness(uint16_t front_port, uint16_t back_port,
                          uint16_t capture_port) {
    backend.set(zmq::sockopt::xpub_verboser, true);
    capture.set(zmq::sockopt::sndhwm, 1000);
    capture.bind(mads_test::loopback(capture_port));

    frontend.bind(mads_test::loopback(front_port));
    backend.bind(mads_test::loopback(back_port));
    controlled.bind("inproc://test-sub-table-ctrl");
    controller.connect("inproc://test-sub-table-ctrl");
    proxy_thread = std::thread(run_proxy, std::ref(frontend), std::ref(backend),
                               std::ref(controlled), zmq::socket_ref(capture));

    reader_thread = std::thread([this, capture_port] {
      zmq::socket_t reader(ctx, zmq::socket_type::sub);
      reader.set(zmq::sockopt::subscribe, "");
      reader.set(zmq::sockopt::rcvtimeo, 100);
      reader.connect(mads_test::loopback(capture_port));
      while (reader_running.load()) {
        zmq::multipart_t msg;
        if (!msg.recv(reader)) continue;
        if (msg.size() != 1) continue; // ordinary data frame, not a notification
        const std::string part = msg.at(0).to_string();
        if (part.empty()) continue;
        const uint8_t flag = static_cast<uint8_t>(part[0]);
        const std::string topic = part.substr(1);
        std::lock_guard<std::mutex> lock(table_mutex);
        if (flag == 1) {
          counts[topic]++;
        } else if (flag == 0) {
          auto it = counts.find(topic);
          if (it != counts.end() && --it->second <= 0) counts.erase(it);
        }
      }
      reader.close();
    });

    pub.connect(mads_test::loopback(front_port));
    std::this_thread::sleep_for(300ms); // connect + subscription propagation
  }

  ~SubscribingProxyHarness() {
    steer(controller, "TERMINATE");
    if (proxy_thread.joinable()) proxy_thread.join();
    reader_running.store(false);
    if (reader_thread.joinable()) reader_thread.join();
    pub.close();
    sub.close();
    controller.close();
    controlled.close();
    frontend.close();
    backend.close();
    capture.close();
  }

  int count_of(std::string const &topic) {
    std::lock_guard<std::mutex> lock(table_mutex);
    auto it = counts.find(topic);
    return it == counts.end() ? 0 : it->second;
  }
};

} // namespace

TEST_CASE("subscribing to a topic makes it appear in the captured table",
          "[broker_subscriptions]") {
  SubscribingProxyHarness h(44151, 44152, 44153);

  REQUIRE(h.count_of("widgets") == 0);
  h.sub.set(zmq::sockopt::subscribe, "widgets");
  h.sub.connect(mads_test::loopback(44152));

  REQUIRE(mads_test::wait_for([&] { return h.count_of("widgets") == 1; }, 2000ms));
}

TEST_CASE("unsubscribing removes the topic once the last subscriber leaves",
          "[broker_subscriptions]") {
  SubscribingProxyHarness h(44154, 44155, 44156);

  h.sub.set(zmq::sockopt::subscribe, "gadgets");
  h.sub.connect(mads_test::loopback(44155));
  REQUIRE(mads_test::wait_for([&] { return h.count_of("gadgets") == 1; }, 2000ms));

  h.sub.set(zmq::sockopt::unsubscribe, "gadgets");
  REQUIRE(mads_test::wait_for([&] { return h.count_of("gadgets") == 0; }, 2000ms));
}

TEST_CASE("two subscribers to the same topic keep it present until both leave",
          "[broker_subscriptions]") {
  SubscribingProxyHarness h(44157, 44158, 44159);
  zmq::socket_t sub2(h.ctx, zmq::socket_type::sub);

  h.sub.set(zmq::sockopt::subscribe, "shared");
  h.sub.connect(mads_test::loopback(44158));
  sub2.set(zmq::sockopt::subscribe, "shared");
  sub2.connect(mads_test::loopback(44158));
  REQUIRE(mads_test::wait_for([&] { return h.count_of("shared") == 2; }, 2000ms));

  h.sub.set(zmq::sockopt::unsubscribe, "shared");
  REQUIRE(mads_test::wait_for([&] { return h.count_of("shared") == 1; }, 2000ms));

  sub2.close();
}

TEST_CASE("ordinary multi-frame data traffic never pollutes the table",
          "[broker_subscriptions]") {
  SubscribingProxyHarness h(44160, 44161, 44162);
  h.sub.set(zmq::sockopt::subscribe, "");
  h.sub.set(zmq::sockopt::rcvtimeo, 200);
  h.sub.connect(mads_test::loopback(44161));
  std::this_thread::sleep_for(300ms);

  for (int i = 0; i < 20; ++i) {
    zmq::multipart_t out;
    out.addstr("mytopic");
    out.addstr("some payload bytes");
    out.send(h.pub);
    zmq::multipart_t in;
    in.recv(h.sub);
  }

  // Data frames are two parts; the reader only ever tables 1-frame messages,
  // so no topic named after the data traffic's own topic string should
  // appear with a bogus positive count.
  REQUIRE(h.count_of("mytopic") == 0);
}
