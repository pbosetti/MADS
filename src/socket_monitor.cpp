#include "socket_monitor.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace Mads {

namespace {
LinkEvent to_link_event(uint16_t zmq_event) {
  switch (zmq_event) {
  case ZMQ_EVENT_CONNECTED:
    return LinkEvent::Connected;
  case ZMQ_EVENT_CONNECT_DELAYED:
    return LinkEvent::ConnectDelayed;
  case ZMQ_EVENT_CONNECT_RETRIED:
    return LinkEvent::ConnectRetried;
  case ZMQ_EVENT_HANDSHAKE_SUCCEEDED:
    return LinkEvent::HandshakeSucceeded;
  case ZMQ_EVENT_HANDSHAKE_FAILED_AUTH:
    return LinkEvent::HandshakeFailedAuth;
  case ZMQ_EVENT_HANDSHAKE_FAILED_PROTOCOL:
    return LinkEvent::HandshakeFailedProtocol;
  case ZMQ_EVENT_HANDSHAKE_FAILED_NO_DETAIL:
    return LinkEvent::HandshakeFailedNoDetail;
  case ZMQ_EVENT_DISCONNECTED:
    return LinkEvent::Disconnected;
  default:
    return LinkEvent::None;
  }
}
} // namespace

// zmq::monitor_t requires subclassing to observe events (cppzmq has no
// callback-based alternative), so the subclass -- and its zmq_event_t
// handling -- stay out of the public header, pimpl-style.
class SocketMonitor::Impl : public zmq::monitor_t {
public:
  std::atomic<bool> stop_requested{false};
  std::thread thread;

  LinkEvent last_event() const {
    std::lock_guard<std::mutex> lock(_mtx);
    return _last_event;
  }

  std::string last_event_address() const {
    std::lock_guard<std::mutex> lock(_mtx);
    return _last_address;
  }

  bool wait_connected(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(_mtx);
    return _cv.wait_for(lock, timeout, [this] {
      return _last_event == LinkEvent::Connected ||
             _last_event == LinkEvent::HandshakeSucceeded;
    });
  }

  bool wait_handshake_succeeded(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(_mtx);
    // Wakes as soon as the handshake resolves either way -- success or one
    // of the failure events -- rather than always waiting out the full
    // timeout on a rejection the monitor already knows about.
    _cv.wait_for(lock, timeout, [this] {
      return _last_event == LinkEvent::HandshakeSucceeded ||
             _last_event == LinkEvent::HandshakeFailedAuth ||
             _last_event == LinkEvent::HandshakeFailedProtocol ||
             _last_event == LinkEvent::HandshakeFailedNoDetail;
    });
    return _last_event == LinkEvent::HandshakeSucceeded;
  }

protected:
  void on_event_connected(const zmq_event_t &ev, const char *addr) override {
    record(ev, addr);
  }
  void on_event_connect_delayed(const zmq_event_t &ev,
                                const char *addr) override {
    record(ev, addr);
  }
  void on_event_connect_retried(const zmq_event_t &ev,
                                const char *addr) override {
    record(ev, addr);
  }
  void on_event_handshake_succeeded(const zmq_event_t &ev,
                                    const char *addr) override {
    record(ev, addr);
  }
  void on_event_handshake_failed_auth(const zmq_event_t &ev,
                                      const char *addr) override {
    record(ev, addr);
  }
  void on_event_handshake_failed_protocol(const zmq_event_t &ev,
                                          const char *addr) override {
    record(ev, addr);
  }
  void on_event_handshake_failed_no_detail(const zmq_event_t &ev,
                                           const char *addr) override {
    record(ev, addr);
  }
  void on_event_disconnected(const zmq_event_t &ev,
                             const char *addr) override {
    record(ev, addr);
  }

private:
  mutable std::mutex _mtx;
  std::condition_variable _cv;
  LinkEvent _last_event = LinkEvent::None;
  std::string _last_address;

  void record(const zmq_event_t &ev, const char *addr) {
    {
      std::lock_guard<std::mutex> lock(_mtx);
      _last_event = to_link_event(ev.event);
      _last_address = addr ? addr : "";
    }
    _cv.notify_all();
  }
};

SocketMonitor::SocketMonitor() : _impl(std::make_unique<Impl>()) {}

SocketMonitor::~SocketMonitor() { stop(); }

void SocketMonitor::start(zmq::socket_t &socket, int events) {
  if (_impl->thread.joinable())
    return; // already started

  // A private, process-unique inproc:// endpoint per instance -- two
  // monitors (e.g. an Agent's publisher and subscriber) must not share one.
  static std::atomic<uint64_t> counter{0};
  const std::string endpoint =
      "inproc://mads-socket-monitor-" + std::to_string(++counter);

  _impl->init(socket, endpoint, events);
  _impl->stop_requested.store(false);
  _impl->thread = std::thread([impl = _impl.get()] {
    try {
      while (!impl->stop_requested.load()) {
        impl->check_event(100);
      }
    } catch (const zmq::error_t &) {
      // The context or socket was torn down from under us during shutdown;
      // nothing left to observe.
    }
  });
}

void SocketMonitor::stop() {
  if (!_impl->thread.joinable())
    return;
  _impl->stop_requested.store(true);
  _impl->thread.join();
  // Replacing _impl destroys the old one, running zmq::monitor_t's
  // destructor: it detaches from the monitored socket
  // (zmq_socket_monitor(socket, nullptr, 0), while the socket is still open
  // -- this must run before the caller closes it) and, critically, closes
  // the monitor's own PAIR socket. Leaving that PAIR socket open (e.g. by
  // calling only abort(), which does not touch it) would make the owning
  // zmq::context_t's close()/zmq_ctx_term() block forever waiting for it.
  // A fresh Impl also means a later start() (e.g. after a reconnect) works.
  _impl = std::make_unique<Impl>();
}

bool SocketMonitor::wait_connected(std::chrono::milliseconds timeout) {
  return _impl->wait_connected(timeout);
}

bool SocketMonitor::wait_handshake_succeeded(std::chrono::milliseconds timeout) {
  return _impl->wait_handshake_succeeded(timeout);
}

LinkEvent SocketMonitor::last_event() const { return _impl->last_event(); }

std::string SocketMonitor::last_event_address() const {
  return _impl->last_event_address();
}

} // namespace Mads
