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
#include <memory>
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
   */
  void start(zmq::socket_t &socket, int events = ZMQ_EVENT_ALL);

  /**
   * @brief Stops the monitoring thread and detaches from the socket
   * (`zmq_socket_monitor(socket, nullptr, 0)`). Must be called -- directly or
   * via the destructor -- before the monitored socket is closed. Safe to call
   * more than once and safe if start() was never called.
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
   * @brief Blocks until ZMQ_EVENT_HANDSHAKE_SUCCEEDED specifically is
   * observed, or `timeout` elapses. Unlike wait_connected(), this does not
   * also accept a bare Connected event, so it is not fooled by a CURVE/ZAP
   * rejection: TCP connects immediately regardless of the mechanism, but the
   * handshake only succeeds once the security layer has actually accepted
   * the peer.
   * @return true if the handshake succeeded within the timeout.
   */
  bool wait_handshake_succeeded(std::chrono::milliseconds timeout);

  /// The most recent event observed (None if none yet).
  LinkEvent last_event() const;

  /// The libzmq-reported peer address the most recent event carried (empty
  /// if none yet).
  std::string last_event_address() const;

private:
  class Impl;
  std::unique_ptr<Impl> _impl;
};

} // namespace Mads
