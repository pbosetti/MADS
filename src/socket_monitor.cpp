#include "socket_monitor.hpp"

#include <atomic>
#include <chrono>
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
  // Published separately from monitor_socket() so a driving thread can read
  // it without racing the attach() that sets it.
  std::atomic<void *> pollable{nullptr};

  // process_event() and monitor_socket() are protected in zmq::monitor_t;
  // this is the derived class, so it can hand them out. ZMQ_POLLIN is the
  // only thing the base looks at, and the caller has just seen it on the
  // monitor's own socket.
  void pump() { process_event(ZMQ_POLLIN); }
  void *pollable_handle() { return monitor_socket().handle(); }

  LinkEvent last_event() const {
    std::lock_guard<std::mutex> lock(_mtx);
    return _last_event;
  }

  std::string last_event_address() const {
    std::lock_guard<std::mutex> lock(_mtx);
    return _last_address;
  }

  LinkState state() const {
    std::lock_guard<std::mutex> lock(_mtx);
    return _state;
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
  LinkState _state;

  void record(const zmq_event_t &ev, const char *addr) {
    {
      std::lock_guard<std::mutex> lock(_mtx);
      _last_event = to_link_event(ev.event);
      _last_address = addr ? addr : "";
      _state.last_event = _last_event;
      _state.last_event_address = _last_address;
      apply_status(_last_event);
    }
    _cv.notify_all();
  }

  // Condenses the event stream into the two-state status callers act on.
  // Called with _mtx held.
  void apply_status(LinkEvent ev) {
    LinkStatus next = _state.status;
    switch (ev) {
    case LinkEvent::HandshakeSucceeded:
      // The only event that proves the link is *usable*. Connected is
      // deliberately absent: it fires as soon as TCP is up, before the ZMTP
      // mechanism has run, so treating it as Up would score a CURVE
      // rejection as a connection plus an immediate spurious drop.
      next = LinkStatus::Up;
      break;
    case LinkEvent::Disconnected:
    case LinkEvent::ConnectRetried:
    case LinkEvent::HandshakeFailedAuth:
    case LinkEvent::HandshakeFailedProtocol:
    case LinkEvent::HandshakeFailedNoDetail:
      // A refused handshake leaves libzmq retrying against a peer that will
      // not talk to us -- as unusable as a peer that went away, and worth
      // reporting as such rather than sitting at Unknown forever.
      next = LinkStatus::Down;
      break;
    case LinkEvent::Connected:
    case LinkEvent::ConnectDelayed:
    case LinkEvent::None:
      // In flight, neither up nor conclusively down. last_event still
      // records them for callers that want the finer detail.
      break;
    }
    if (next == _state.status)
      return; // repeated retries collapse into the one transition
    if (_state.status == LinkStatus::Up)
      ++_state.drops;
    else if (_state.status == LinkStatus::Down && next == LinkStatus::Up)
      ++_state.recoveries; // Unknown -> Up is a first connect, not a recovery
    _state.status = next;
    _state.changed_at = std::chrono::steady_clock::now();
  }
};

SocketMonitor::SocketMonitor() : _impl(std::make_unique<Impl>()) {}

SocketMonitor::~SocketMonitor() { stop(); }

void SocketMonitor::attach(zmq::socket_t &socket, int events) {
  if (_impl->pollable.load() != nullptr)
    return; // already attached

  // A private, process-unique inproc:// endpoint per instance -- two
  // monitors (e.g. an Agent's publisher and subscriber) must not share one.
  static std::atomic<uint64_t> counter{0};
  const std::string endpoint =
      "inproc://mads-socket-monitor-" + std::to_string(++counter);

  _impl->init(socket, endpoint, events);
  _impl->pollable.store(_impl->pollable_handle(), std::memory_order_release);
}

void SocketMonitor::start(zmq::socket_t &socket, int events) {
  if (_impl->thread.joinable())
    return; // already started

  attach(socket, events);
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

zmq::socket_ref SocketMonitor::pollable() const {
  void *handle = _impl->pollable.load(std::memory_order_acquire);
  return handle ? zmq::socket_ref(zmq::from_handle, handle) : zmq::socket_ref();
}

void SocketMonitor::process_pending() {
  if (_impl->pollable.load(std::memory_order_acquire) == nullptr)
    return;
  try {
    _impl->pump();
  } catch (const zmq::error_t &) {
    // Context torn down mid-drain; nothing left to observe.
  }
}

void SocketMonitor::stop() {
  // An attach()ed monitor has no thread of its own but still holds the PAIR
  // socket that must be released here, so joinability alone is not the test
  // for "there is nothing to stop".
  if (!_impl->thread.joinable() && _impl->pollable.load() == nullptr)
    return;
  if (_impl->thread.joinable()) {
    _impl->stop_requested.store(true);
    _impl->thread.join();
  }
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

LinkState SocketMonitor::state() const { return _impl->state(); }

std::string SocketMonitor::last_event_address() const {
  return _impl->last_event_address();
}

} // namespace Mads
