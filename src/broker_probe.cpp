// Windows networking headers must be included before anything else in this
// translation unit (including "mads.hpp", which pulls in Logger and -- when
// MADS_HAS_MONGOCXX is defined -- the Mongo C++ driver's own transitive
// <windows.h>). Including <winsock2.h>/<ws2tcpip.h> after any of that risks
// them landing after a plain <windows.h> has already dragged in the legacy
// <winsock.h>, which corrupts later Windows-only ZMQ headers in ways that
// surface as unrelated parse errors deep inside the ZMQ headers themselves.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "broker_probe.hpp"

#include "curve.hpp"
#include "detail/wire_format.hpp"
#include "mads.hpp"
#include "socket_monitor.hpp"

#include <nlohmann/json.hpp>
#include <zmq.hpp>
#include <zmq_addon.hpp>

#include <algorithm>
#include <thread>

namespace Mads {

namespace {

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t invalid_socket = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t invalid_socket = -1;
#endif

void ensure_socket_runtime() {
#ifdef _WIN32
  static const int initialized = []() {
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
    return 0;
  }();
  (void)initialized;
#endif
}

void close_socket(socket_t fd) {
  if (fd == invalid_socket) {
    return;
  }
#ifdef _WIN32
  closesocket(fd);
#else
  ::close(fd);
#endif
}

// One bounded, non-blocking connect attempt. `budget` may be zero (a single
// immediate poll, still valid for select()/WSAPoll-style waits).
bool tcp_connect_once(const std::string &host, int port,
                      std::chrono::milliseconds budget) {
  ensure_socket_runtime();

  const socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd == invalid_socket) {
    return false;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  std::string resolved = host.empty() || host == "localhost" ? "127.0.0.1" : host;
  if (::inet_pton(AF_INET, resolved.c_str(), &addr.sin_addr) != 1) {
    close_socket(fd);
    return false;
  }

#ifdef _WIN32
  u_long nonblocking = 1;
  ioctlsocket(fd, FIONBIO, &nonblocking);
#else
  const int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif

  const int rc =
      ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
  bool connected = (rc == 0);

  if (!connected) {
#ifdef _WIN32
    const bool in_progress = (WSAGetLastError() == WSAEWOULDBLOCK);
#else
    const bool in_progress = (errno == EINPROGRESS);
#endif
    if (in_progress) {
      fd_set write_set;
      FD_ZERO(&write_set);
      FD_SET(fd, &write_set);
      timeval tv{};
      tv.tv_sec = static_cast<long>(budget.count() / 1000);
      tv.tv_usec = static_cast<long>((budget.count() % 1000) * 1000);
      const int selected =
          ::select(static_cast<int>(fd + 1), nullptr, &write_set, nullptr, &tv);
      if (selected > 0 && FD_ISSET(fd, &write_set)) {
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR,
                      reinterpret_cast<char *>(&so_error), &len) == 0 &&
            so_error == 0) {
          connected = true;
        }
      }
    }
  }

  close_socket(fd);
  return connected;
}

// Applies client-side CURVE credentials to a probe socket, if any were given.
// CurveAuth's constructor takes a context only to hold a ZapAuth member that
// is never started here (setup_auth() is not called), so nothing binds or
// listens -- this only reads the three key files off disk and sets the three
// socket options. Throws, like CurveAuth itself, when a key file is missing
// or unreadable; every caller below already treats an exception as "no
// answer", which is the honest result for a probe that could not even be
// configured.
void apply_curve(zmq::socket_t &socket, zmq::context_t &context,
                 const std::optional<ProbeCurveKeys> &curve) {
  if (!curve) {
    return;
  }
  CurveAuth curve_auth(context);
  curve_auth.set_key_dir(curve->key_dir);
  curve_auth.setup_curve_client(socket, curve->client_key_name,
                                curve->server_key_name);
}

} // namespace

bool probe_broker(const std::string &uri, std::chrono::milliseconds timeout,
                  const std::optional<ProbeCurveKeys> &curve) {
  try {
    zmq::context_t context;
    zmq::socket_t socket(context, zmq::socket_type::req);
    socket.set(zmq::sockopt::linger, 0);
    // Before connect(): CURVE options are read when the connection is set
    // up, so setting them afterwards would leave this peer talking NULL.
    apply_curve(socket, context, curve);
    const int timeout_ms = static_cast<int>(
        std::max<std::chrono::milliseconds::rep>(0, timeout.count()));
    socket.set(zmq::sockopt::rcvtimeo, timeout_ms);
    socket.set(zmq::sockopt::sndtimeo, timeout_ms);
    socket.connect(uri);

    zmq::multipart_t request;
    request.addstr(std::string(LIB_VERSION));
    request.addstr(std::string("settings"));
    request.addstr(std::string("__mads_up_probe__"));
    if (!request.send(socket)) {
      socket.close();
      return false;
    }

    zmq::multipart_t reply;
    const bool ok = reply.recv(socket);
    socket.close();
    return ok;
  } catch (const std::exception &) {
    return false;
  }
}

CurveProbeResult
probe_curve_handshake(const std::string &uri,
                      const std::filesystem::path &key_dir,
                      const std::string &client_key_name,
                      const std::string &server_key_name,
                      std::chrono::milliseconds timeout) {
  try {
    zmq::context_t context;
    zmq::socket_t socket(context, zmq::socket_type::req);
    socket.set(zmq::sockopt::linger, 0);

    apply_curve(socket, context,
                ProbeCurveKeys{key_dir, client_key_name, server_key_name});

    Mads::SocketMonitor monitor;
    monitor.start(socket);
    socket.connect(uri);

    // Deliberately NOT wait_connected(): ZMQ_EVENT_CONNECTED fires at the
    // TCP level before the CURVE/ZAP handshake is even attempted, so it
    // would report success on a rejection just as readily as on a real one.
    const bool succeeded = monitor.wait_handshake_succeeded(timeout);
    // last_handshake, not last_event: libzmq fires DISCONNECTED straight
    // after a rejection, so the newest event is usually that rather than the
    // refusal -- which would report a bare Timeout and lose the whole point
    // of this probe.
    const LinkEvent handshake = monitor.state().last_handshake;
    monitor.stop();
    socket.close();

    if (succeeded)
      return CurveProbeResult::Connected;
    if (handshake == LinkEvent::HandshakeFailedAuth)
      return CurveProbeResult::RejectedAuth;
    return CurveProbeResult::Timeout;
  } catch (const std::exception &) {
    return CurveProbeResult::Timeout;
  }
}

std::optional<std::map<std::string, int>>
fetch_subscription_table(const std::string &sub_uri,
                         std::chrono::milliseconds timeout,
                         const std::optional<ProbeCurveKeys> &curve) {
  try {
    zmq::context_t context;
    zmq::socket_t socket(context, zmq::socket_type::sub);
    socket.set(zmq::sockopt::linger, 0);
    apply_curve(socket, context, curve);
    socket.set(zmq::sockopt::subscribe, "subscriptions");
    // Bounded per-recv wait so the deadline below stays honest even if the
    // broker is up but silent (subscription_table switched off).
    socket.set(zmq::sockopt::rcvtimeo, 200);
    socket.connect(sub_uri);

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      zmq::multipart_t msg;
      if (!msg.recv(socket)) {
        continue; // rcvtimeo expiry; re-check the deadline
      }
      // The broker publishes the table as an ordinary MADS frame:
      // ["subscriptions"][header][json]. Anything else on this topic
      // is not ours to interpret.
      if (msg.size() < 3 || msg.at(0).to_string() != "subscriptions") {
        continue;
      }
      Mads::detail::WireHeader hdr;
      if (!Mads::detail::parse_wire_header(msg.at(1).to_string(), hdr) ||
          hdr.has_blob) {
        continue;
      }
      std::string json_text;
      if (!Mads::detail::decode_to_json_text(msg.at(2).to_string(), hdr.format,
                                             hdr.compression, json_text)) {
        continue;
      }
      std::map<std::string, int> table;
      const auto doc = nlohmann::json::parse(json_text);
      if (!doc.is_object()) {
        continue;
      }
      for (auto it = doc.begin(); it != doc.end(); ++it) {
        if (it.value().is_number_integer()) {
          table[it.key()] = it.value().get<int>();
        }
      }
      socket.close();
      return table;
    }
    socket.close();
    return std::nullopt;
  } catch (const std::exception &) {
    return std::nullopt;
  }
}

bool probe_tcp_port(const std::string &host, int port,
                    std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  while (true) {
    const auto now = std::chrono::steady_clock::now();
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    const auto attempt_budget = remaining.count() > 0
                                    ? std::min(remaining, std::chrono::milliseconds(200))
                                    : std::chrono::milliseconds(0);

    if (tcp_connect_once(host, port, attempt_budget)) {
      return true;
    }
    if (now >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(
        std::chrono::milliseconds(std::min<long long>(20, remaining.count())));
  }
}

} // namespace Mads
