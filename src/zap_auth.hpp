/*
  _____              _         _   _
 |__  /__ _ _ __    / \  _   _| |_| |__
   / // _` | '_ \  / _ \| | | | __| '_ \
  / /| (_| | |_) |/ ___ \ |_| | |_| | | |
 /____\__,_| .__//_/   \_\__,_|\__|_| |_|
           |_|

ZeroMQ Authentication Protocol (ZAP, RFC 27) handler
Copyright (C) 2026 Paolo Bosetti
*/

#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#endif

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <zmq.hpp>

namespace Mads {

/**
 * @brief In-process ZAP (ZeroMQ Authentication Protocol, RFC 27) handler.
 *
 * libzmq delegates every incoming connection on a socket that has a security
 * mechanism enabled to a REP socket bound to `inproc://zeromq.zap.01` inside
 * the same context. This class owns that socket and the thread servicing it.
 *
 * This replaces `zmqpp::auth`, which cppzmq has no equivalent for. It is
 * deliberately limited to what MADS actually needs: an address whitelist and a
 * CURVE client-key allowlist.
 *
 * Ordering matters for security: libzmq only enforces authentication while a
 * handler is bound, so `start()` completes the `bind()` **before** it returns
 * and before any protected socket is bound or connected. Configuration calls
 * are safe after `start()`; they are mutex-protected against the worker thread.
 *
 * Only one handler may exist per `zmq::context_t` (the ZAP endpoint is a
 * single well-known inproc address).
 */
class ZapAuth {
public:
  /**
   * @brief Construct a handler for the given context. Does not bind yet.
   *
   * @param context The context whose sockets this handler will authenticate.
   */
  explicit ZapAuth(zmq::context_t &context) : _context(context) {}

  ~ZapAuth() { stop(); }

  ZapAuth(const ZapAuth &) = delete;
  ZapAuth &operator=(const ZapAuth &) = delete;
  ZapAuth(ZapAuth &&) = delete;
  ZapAuth &operator=(ZapAuth &&) = delete;

  /**
   * @brief Bind the ZAP endpoint and start servicing requests.
   *
   * Blocks until the endpoint is bound, so that no unauthenticated connection
   * can slip through before the handler is listening. Calling it more than
   * once is a no-op.
   *
   * @throws zmq::error_t if the ZAP endpoint cannot be bound (typically
   *         because another handler already owns it in this context).
   */
  void start();

  /**
   * @brief Stop servicing requests and join the worker thread.
   *
   * Idempotent; also called by the destructor. Unlike zmqpp's actor-based
   * authenticator, teardown is deterministic and leaves no process-lifetime
   * state behind.
   */
  void stop();

  /** @brief Log every authentication decision to stdout. */
  void set_verbose(bool verbose) { _verbose.store(verbose); }

  /**
   * @brief Restrict the handler to one ZAP domain.
   *
   * @param domain The domain to serve; `"*"` (the default) serves every
   *        domain, matching how MADS configures it.
   */
  void configure_domain(const std::string &domain);

  /**
   * @brief Add an address to the whitelist.
   *
   * While the whitelist is empty every peer address is acceptable. As soon as
   * one address is added, any peer not on the list is rejected.
   *
   * @param address The peer IP address to allow, e.g. `"127.0.0.1"`.
   */
  void allow(const std::string &address);

  /**
   * @brief Add a client public key to the CURVE allowlist.
   *
   * While the allowlist is empty every CURVE client is acceptable. As soon as
   * one key is added, only those clients are accepted. The special value
   * `"*"` accepts any CURVE client regardless of the rest of the list.
   *
   * @param z85_public_key The client public key, Z85-encoded (40 characters).
   */
  void configure_curve(const std::string &z85_public_key);

  /** @brief Number of requests granted so far (test/diagnostic aid). */
  int granted() const { return _granted.load(); }

  /** @brief Number of requests denied so far (test/diagnostic aid). */
  int denied() const { return _denied.load(); }

  /** @brief The well-known ZAP endpoint mandated by RFC 27. */
  static constexpr const char *endpoint = "inproc://zeromq.zap.01";

private:
  /** @brief Worker loop: answers ZAP requests until stop() is called. */
  void _serve();

  /**
   * @brief Apply the configured policy to one parsed request.
   *
   * @param domain The ZAP domain of the request.
   * @param address The peer address.
   * @param mechanism The security mechanism ("NULL", "PLAIN", "CURVE", ...).
   * @param credentials The mechanism-specific credential frame; for CURVE this
   *        is the client's 32-byte binary public key.
   * @param[out] reason Human-readable denial reason, empty when granted.
   * @return true if the connection is authorised.
   */
  bool _authorise(const std::string &domain, const std::string &address,
                  const std::string &mechanism,
                  const std::string &credentials, std::string &reason) const;

  zmq::context_t &_context;
  std::unique_ptr<zmq::socket_t> _socket;
  std::thread _thread;
  std::atomic<bool> _stop{false};
  std::atomic<bool> _running{false};
  std::atomic<bool> _verbose{false};
  std::atomic<int> _granted{0};
  std::atomic<int> _denied{0};
  mutable std::mutex _config_mutex;
  std::string _domain = "*";
  std::set<std::string> _allowed_addresses;
  std::set<std::string> _allowed_curve_keys;
};

/** @brief A CURVE keypair, both keys Z85-encoded (40 characters each). */
struct CurveKeypair {
  std::string public_key;
  std::string secret_key;
};

/**
 * @brief Generate a fresh CURVE keypair.
 *
 * Replaces `zmqpp::curve::generate_keypair()`; cppzmq exposes no wrapper for
 * `zmq_curve_keypair()`, so the C API is called directly (as
 * `doctor_checks.cpp` already does for `zmq_z85_decode`).
 *
 * @return The new keypair.
 * @throws zmq::error_t if libzmq was built without CURVE support.
 */
CurveKeypair generate_keypair();

/**
 * @brief Z85-encode a binary buffer.
 *
 * @param data The bytes to encode; the length must be a multiple of 4.
 * @return The Z85 text, or an empty string if the input length is invalid.
 */
std::string z85_encode(const std::string &data);

} // namespace Mads
