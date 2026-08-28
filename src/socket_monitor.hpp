/*
  ____             _        _   __  __             _ _
 / ___|  ___   ___| | _____| |_|  \/  | ___  _ __ (_) |_ ___  _ __
 \___ \ / _ \ / __| |/ / _ \ __| |\/| |/ _ \| '_ \| | __/ _ \| '__|
  ___) | (_) | (__|   <  __/ |_| |  | | (_) | | | | | || (_) | |
 |____/ \___/ \___|_|\_\___|\__|_|  |_|\___/|_| |_|_|\__\___/|_|

Wraps zmq_socket_monitor()/zmq::monitor_t so callers can turn connection
lifecycle guesswork into fact: a real ZMQ_EVENT_CONNECTED instead of a blind
sleep, a real ZMQ_EVENT_HANDSHAKE_FAILED_AUTH instead of a bare receive
timeout (ZMQ_DEVELOPMENT.md §2.1). Purely local observation -- nothing on the
wire changes, so using this has no version impact.

Author(s): Paolo Bosetti
*/
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <zmq.hpp>

namespace Mads {

/// The connectivity events SocketMonitor exposes, condensed from libzmq's
/// full ZMQ_EVENT_* set (zmq_socket_monitor(3)) down to what a caller acts
/// on: is the link up, did the handshake fail, and if so why.
enum class LinkEvent {
  None,
  Connected,
  ConnectDelayed,
  ConnectRetried,
  HandshakeSucceeded,
  HandshakeFailedAuth,
  HandshakeFailedProtocol,
  HandshakeFailedNoDetail,
  Disconnected,
  /// A *bound* socket could not accept an inbound connection. Unlike every
  /// other event here this describes the listener, not a link: the peer it
  /// would have belonged to never got far enough to have one. The errno
  /// libzmq reported (EMFILE when the process is out of file descriptors)
  /// arrives with it, in LinkState::last_event_value.
  AcceptFailed,
};

/// Whether the link is usable *right now*. Where LinkEvent is a
/// point-in-time observation, this is the state those observations add up
/// to -- which is what a caller reporting "am I still talking to the
/// broker?" actually needs.
enum class LinkStatus {
  Unknown, ///< no event has settled the question yet (also: inproc://, which
           ///< libzmq does not report monitor events for at all)
  Up,      ///< the ZMTP handshake completed and has not been undone since
  Down,    ///< the peer went away, or the handshake was refused
};

/// Everything SocketMonitor knows about one link, taken atomically so the
/// status, the counters and the event that caused them can never disagree.
struct LinkState {
  LinkStatus status = LinkStatus::Unknown;
  /// The most recent event, whatever it was.
  LinkEvent last_event = LinkEvent::None;
  /// How the most recent ZMTP handshake ended: HandshakeSucceeded, one of
  /// the three HandshakeFailed* events, or None if none has completed yet.
  ///
  /// This -- not last_event -- is what carries the *reason* a link is down.
  /// libzmq follows a rejected handshake with ZMQ_EVENT_DISCONNECTED and
  /// then a stream of ZMQ_EVENT_CONNECT_RETRIED, so by the time anything
  /// asks, last_event has almost always moved on and "the broker rejected
  /// our key" would have been lost. Only the next handshake replaces it.
  LinkEvent last_handshake = LinkEvent::None;
  /// The peer address libzmq reported with that event ("" if none yet).
  std::string last_event_address;
  /// Up -> Down transitions. Counts transitions, not events, so a broker
  /// that stays down through a hundred CONNECT_RETRIED reads as one drop.
  uint64_t drops = 0;
  /// Down -> Up transitions. A first connection is not a recovery, so this
  /// stays 0 until a drop has actually been repaired.
  uint64_t recoveries = 0;
  /// When `status` last changed; empty while it is still Unknown. Callers
  /// report "down for 12s" from this.
  std::optional<std::chrono::steady_clock::time_point> changed_at;
  /// The integer libzmq attached to the most recent event. Its meaning is
  /// per-event and it is 0 for the ones that carry nothing useful; the case
  /// this exists for is AcceptFailed, where it is the errno accept(2)
  /// failed with.
  int last_event_value = 0;
  /// Inbound connections this listener has failed to accept. Monotonic, so a
  /// caller can report only what is new since it last looked -- which matters
  /// because a full descriptor table leaves the listening socket permanently
  /// readable, and libzmq re-fires the failure as fast as it can poll.
  uint64_t accept_failures = 0;
};

/**
 * @brief One monitor per monitored socket. start() must be called before the
 * socket's connect()/bind(): libzmq lets a connection through -- and may fire
 * its lifecycle events -- while no monitor is attached yet, exactly the same
 * race Mads::ZapAuth::start() avoids for the ZAP handler.
 *
 * The monitor runs its own background thread polling the inproc:// pair
 * zmq_socket_monitor() publishes to; nothing about it touches the monitored
 * socket itself, so it composes with any other use of that socket.
 *
 * state() assumes the monitored socket *connects* to a single peer. A bound
 * socket gets ZMQ_EVENT_ACCEPTED (not CONNECTED/HANDSHAKE_SUCCEEDED) per
 * arriving peer and ZMQ_EVENT_DISCONNECTED per departing one, so its events
 * would add up to "down" the moment any one of several peers left; use
 * last_event() there, or nothing at all.
 */
class SocketMonitor {
public:
  SocketMonitor();
  ~SocketMonitor();
  SocketMonitor(const SocketMonitor &) = delete;
  SocketMonitor &operator=(const SocketMonitor &) = delete;

  /**
   * @brief Starts monitoring `socket` on a private inproc:// endpoint and a
   * dedicated thread. A second call before stop() is a no-op.
   *
   * Equivalent to attach() plus a thread that does nothing but drive it. Use
   * attach() instead when there is already a poll loop to fold this into.
   */
  void start(zmq::socket_t &socket, int events = ZMQ_EVENT_ALL);

  /**
   * @brief Attaches to `socket` without starting any thread, leaving the
   * caller to drive the monitor: include pollable() in a `zmq::poll()` and
   * call process_pending() whenever it reports `ZMQ_POLLIN`.
   *
   * This is what lets several monitors -- or a monitor and the application's
   * own sockets -- share one poll loop instead of one thread each
   * (ZMQ_DEVELOPMENT.md §4.1). Same ordering contract as start(): call it
   * before the socket's connect()/bind().
   *
   * A second call before stop() is a no-op, so a caller that reconnects can
   * call it again without re-arming zmq_socket_monitor().
   */
  void attach(zmq::socket_t &socket, int events = ZMQ_EVENT_ALL);

  /**
   * @brief The monitor's own PAIR socket, for inclusion in the caller's
   * `zmq::poll()`. Its handle() is null until attach()/start() has run, and
   * null again after stop() -- so a driving loop can simply re-read it every
   * iteration and pick up a monitor that was attached later.
   */
  zmq::socket_ref pollable() const;

  /**
   * @brief Consumes the events pollable() has signalled, updating
   * last_event()/state() and waking the wait_*() calls.
   *
   * Call only from the thread that polls pollable(), and only when that poll
   * reported `ZMQ_POLLIN` -- like the socket it drains, this is not
   * thread-safe. A monitor driven this way must not be stop()ped until that
   * thread has been joined.
   */
  void process_pending();

  /**
   * @brief Stops the monitoring thread (if start() created one) and detaches
   * from the socket (`zmq_socket_monitor(socket, nullptr, 0)`). Must be
   * called -- directly or via the destructor -- before the monitored socket
   * is closed. Safe to call more than once and safe if neither start() nor
   * attach() was ever called.
   *
   * This resets what state() reports back to Unknown with zeroed counters: a
   * detached monitor has no link to describe, and a later start() begins a
   * fresh observation rather than resuming a stale one.
   */
  void stop();

  /**
   * @brief Blocks until a Connected or HandshakeSucceeded event is observed,
   * or `timeout` elapses. This is a fast "did the transport come up" signal
   * only -- ZMQ_EVENT_CONNECTED fires at the TCP level, before any ZMTP
   * security mechanism (NULL/PLAIN/CURVE) has been negotiated. Callers that
   * need to know the handshake itself succeeded (e.g. distinguishing a CURVE
   * rejection from an ordinary connect) must use wait_handshake_succeeded()
   * instead.
   * @return true if the connection was observed within the timeout.
   */
  bool wait_connected(std::chrono::milliseconds timeout);

  /**
   * @brief Blocks until a ZMTP handshake completes one way or the other, or
   * `timeout` elapses. Unlike wait_connected(), this does not accept a bare
   * Connected event, so it is not fooled by a CURVE/ZAP rejection: TCP
   * connects immediately regardless of the mechanism, but the handshake only
   * succeeds once the security layer has actually accepted the peer.
   *
   * Decided on state().last_handshake rather than the newest event, so the
   * DISCONNECTED that libzmq fires straight after a rejection cannot make
   * this report a timeout instead of a refusal.
   *
   * @return true if the handshake succeeded within the timeout.
   */
  bool wait_handshake_succeeded(std::chrono::milliseconds timeout);

  /// The most recent event observed (None if none yet).
  LinkEvent last_event() const;

  /**
   * @brief A consistent snapshot of the link's current status, the event
   * that produced it, and how often it has dropped and recovered.
   *
   * Only ZMQ_EVENT_HANDSHAKE_SUCCEEDED raises the status to Up. A bare
   * ZMQ_EVENT_CONNECTED deliberately does not: it fires as soon as TCP is
   * established, before the ZMTP security mechanism has run, so a CURVE
   * rejection would otherwise register as a connection immediately followed
   * by a spurious drop.
   */
  LinkState state() const;

  /// The libzmq-reported peer address the most recent event carried (empty
  /// if none yet).
  std::string last_event_address() const;

private:
  class Impl;
  std::unique_ptr<Impl> _impl;
};

} // namespace Mads
