/*
  _____ __  __  ___  ____  ____     ____                            _
 |__  /|  \/  |/ _ \|  _ \|  _ \   / ___|___  _ __ ___  _ __   __ _| |_
   / / | |\/| | | | | |_) | |_) | | |   / _ \| '_ ` _ \| '_ \ / _` | __|
  / /_ | |  | | |_| |  __/|  __/  | |__| (_) | | | | | | |_) | (_| | |_
 /____||_|  |_|\__\_\_|   |_|      \____\___/|_| |_| |_| .__/ \__,_|\__|
                                                       |_|
DEPRECATED source-compatibility shim for pre-cppzmq downstream agents
Copyright (C) 2026 Paolo Bosetti
*/

/**
 * @file zmqpp_compat.hpp
 * @brief Opt-in `zmqpp`-shaped API over cppzmq, for downstream code only.
 *
 * MADS used to expose zmqpp types on `Mads::Agent`'s protected interface
 * (`_context`, `_publisher`, `_subscriber`, `receive_raw()`) and pulled
 * `<zmqpp/zmqpp.hpp>` in transitively through `agent.hpp`. Since the cppzmq
 * migration those members are `zmq::context_t` / `zmq::socket_t` /
 * `zmq::multipart_t` and no MADS header includes zmqpp at all.
 *
 * A downstream agent that only calls the public API (`publish()`,
 * `receive()`, `loop()`, settings, callbacks) — and **every** consumer of
 * `Mads::AgentApp`, whose interface never named a ZMQ type — needs nothing
 * from this header. It exists for the narrower case of a subclass that
 * touches the sockets directly, e.g.
 *
 * @code
 * #include <agent.hpp>
 * #include <zmqpp_compat.hpp>   // <-- add this one line
 *
 * class MyAgent : public Mads::Agent {
 *   void send_side_channel() {
 *     zmqpp::socket s(_context, zmqpp::socket_type::push);  // still compiles
 *     s.connect("tcp://localhost:9099");
 *     zmqpp::message m;
 *     m << "hello";
 *     s.send(m);
 *   }
 * };
 * @endcode
 *
 * The types here **derive from** their cppzmq counterparts rather than
 * wrapping them, so a `zmqpp::socket` still binds to a `zmq::socket_t&`
 * parameter (`Mads::CurveAuth::setup_curve_client()`, for one) and the
 * `Agent` members bind to `zmqpp::socket&` parameters in downstream code.
 *
 * @warning Deprecated on arrival: it covers only the API surface MADS itself
 * used, and it will be removed in the release after next. Port to the cppzmq
 * types directly. Note also that this is a **source** compatibility aid only:
 * `MadsCore` is a shared library whose class layout changed, so downstream
 * code must be recompiled regardless.
 *
 * Define `MADS_NO_ZMQPP_COMPAT_WARNING` before including to silence the
 * deprecation warnings while porting.
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

#include <cstring>
#include <string>

#include <zmq.hpp>
#include <zmq_addon.hpp>

#include "zap_auth.hpp"

#if defined(MADS_NO_ZMQPP_COMPAT_WARNING)
#define MADS_ZMQPP_DEPRECATED(msg)
#else
#define MADS_ZMQPP_DEPRECATED(msg) [[deprecated(msg)]]
#endif

/**
 * @brief Minimal stand-in for the parts of zmqpp that MADS used to expose.
 *
 * Not a general-purpose zmqpp replacement: `poller`, `reactor`, `actor`,
 * `auth` and the rest were never part of the MADS interface and are not
 * provided. For ZAP authentication use Mads::ZapAuth / Mads::CurveAuth.
 */
namespace zmqpp {

/** @brief Socket types, spelled as in zmqpp (`socket_type::pub`, ...). */
using socket_type = zmq::socket_type;

/** @brief zmqpp's single exception type maps onto cppzmq's. */
using exception = zmq::error_t;
/** @brief zmqpp raised this for libzmq-level failures; cppzmq has one type. */
using zmq_internal_exception = zmq::error_t;

/**
 * @brief Socket option names, mapped onto the type-checked `zmq::sockopt`.
 *
 * Only the options MADS ever set are provided. Because `zmq::sockopt` entries
 * carry their value type, a wrong-typed `set()` that zmqpp only caught at
 * runtime is now a compile error.
 */
namespace socket_option {
inline constexpr auto linger = zmq::sockopt::linger;
inline constexpr auto receive_timeout = zmq::sockopt::rcvtimeo;
inline constexpr auto send_timeout = zmq::sockopt::sndtimeo;
inline constexpr auto conflate = zmq::sockopt::conflate;
inline constexpr auto receive_high_water_mark = zmq::sockopt::rcvhwm;
inline constexpr auto send_high_water_mark = zmq::sockopt::sndhwm;
inline constexpr auto identity = zmq::sockopt::routing_id;
inline constexpr auto curve_server = zmq::sockopt::curve_server;
inline constexpr auto curve_public_key = zmq::sockopt::curve_publickey;
inline constexpr auto curve_secret_key = zmq::sockopt::curve_secretkey;
inline constexpr auto curve_server_key = zmq::sockopt::curve_serverkey;
} // namespace socket_option

/**
 * @brief zmqpp-shaped context: a `zmq::context_t` plus `terminate()`.
 */
class MADS_ZMQPP_DEPRECATED("use zmq::context_t") context
    : public zmq::context_t {
public:
  using zmq::context_t::context_t;

  /** @brief zmqpp's name for zmq_ctx_term(), which cppzmq calls close(). */
  void terminate() { close(); }
};

/**
 * @brief zmqpp-shaped multipart message over `zmq::multipart_t`.
 *
 * Provides the streaming operators and the part accessors MADS used:
 * `<<`, `>>`, `parts()`, `get()`, `get<T>()`, `raw_data()`, `size(part)`,
 * `add_raw()` and `copy()`.
 */
class MADS_ZMQPP_DEPRECATED("use zmq::multipart_t") message
    : public zmq::multipart_t {
public:
  using zmq::multipart_t::multipart_t;

  message() = default;
  message(zmq::multipart_t &&other) noexcept
      : zmq::multipart_t(std::move(other)) {}

  /** @brief Number of frames (cppzmq spells this `size()`). */
  size_t parts() const { return size(); }

  /** @brief Frame @p n as a string. */
  std::string get(size_t n) const { return at(n).to_string(); }

  /** @brief Frame @p n reinterpreted as a fixed-size type. */
  template <typename T> T get(size_t n) const {
    T value{};
    const auto &frame = at(n);
    std::memcpy(&value, frame.data(),
                frame.size() < sizeof(T) ? frame.size() : sizeof(T));
    return value;
  }

  /** @brief Raw pointer to frame @p n. */
  const void *raw_data(size_t n) const { return at(n).data(); }

  /** @brief Byte length of frame @p n. */
  size_t size(size_t n) const { return at(n).size(); }

  /** @brief Number of frames; kept so `size()` still means what it did. */
  size_t size() const { return zmq::multipart_t::size(); }

  /** @brief Append @p len raw bytes as a new frame. */
  void add_raw(const void *data, size_t len) { addmem(data, len); }

  /** @brief Deep copy (cppzmq spells this `clone()`). */
  message copy() const { return message(clone()); }

  /** @brief Append a string frame. */
  message &operator<<(const std::string &part) {
    addstr(part);
    return *this;
  }
  message &operator<<(const char *part) {
    addstr(std::string(part));
    return *this;
  }

  /** @brief Pop the front frame into @p part. */
  message &operator>>(std::string &part) {
    part = popstr();
    return *this;
  }
};

/**
 * @brief zmqpp-shaped socket: a `zmq::socket_t` with zmqpp's send/receive.
 *
 * Note the argument order zmqpp used: `socket.send(message)` rather than
 * cppzmq's `message.send(socket)`.
 */
class MADS_ZMQPP_DEPRECATED("use zmq::socket_t") socket
    : public zmq::socket_t {
public:
  using zmq::socket_t::socket_t;

  socket(zmq::context_t &ctx, socket_type type) : zmq::socket_t(ctx, type) {}

  /** @brief Subscribe a SUB socket to a topic prefix. */
  void subscribe(const std::string &topic) {
    set(zmq::sockopt::subscribe, topic);
  }

  /** @brief Unsubscribe a SUB socket from a topic prefix. */
  void unsubscribe(const std::string &topic) {
    set(zmq::sockopt::unsubscribe, topic);
  }

  /** @brief Send a multipart message; false on timeout/EAGAIN. */
  bool send(zmq::multipart_t &msg, bool dont_block = false) {
    return msg.send(*this, dont_block ? ZMQ_DONTWAIT : 0);
  }

  /** @brief Send a single-frame string message; false on timeout/EAGAIN. */
  bool send(const std::string &payload, bool dont_block = false) {
    return zmq::socket_t::send(zmq::buffer(payload),
                               dont_block ? zmq::send_flags::dontwait
                                          : zmq::send_flags::none)
        .has_value();
  }

  /** @brief Receive a multipart message; false on timeout/EAGAIN. */
  bool receive(zmq::multipart_t &msg, bool dont_block = false) {
    return msg.recv(*this, dont_block ? ZMQ_DONTWAIT : 0);
  }

  /** @brief zmqpp's out-parameter getter, over the type-checked cppzmq one. */
  template <typename Opt, typename T> void get(Opt opt, T &out) const {
    out = zmq::socket_t::get(opt);
  }

  using zmq::socket_t::get;
};

/** @brief zmqpp's CURVE helpers, over Mads::ZapAuth's libzmq-backed ones. */
namespace curve {

/** @brief zmqpp's keypair struct; the fields keep their names. */
using keypair = Mads::CurveKeypair;

/** @brief Generate a CURVE keypair. */
inline keypair generate_keypair() { return Mads::generate_keypair(); }

} // namespace curve

} // namespace zmqpp
