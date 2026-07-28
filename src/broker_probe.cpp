#include "broker_probe.hpp"

#include "mads.hpp"

#include <zmqpp/zmqpp.hpp>

#include <algorithm>
#include <thread>

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

} // namespace

bool probe_broker(const std::string &uri, std::chrono::milliseconds timeout) {
  try {
    zmqpp::context context;
    zmqpp::socket socket(context, zmqpp::socket_type::req);
    socket.set(zmqpp::socket_option::linger, 0);
    const int timeout_ms = static_cast<int>(
        std::max<std::chrono::milliseconds::rep>(0, timeout.count()));
    socket.set(zmqpp::socket_option::receive_timeout, timeout_ms);
    socket.set(zmqpp::socket_option::send_timeout, timeout_ms);
    socket.connect(uri);

    zmqpp::message request;
    request << std::string(LIB_VERSION) << std::string("settings")
           << std::string("__mads_up_probe__");
    if (!socket.send(request)) {
      socket.close();
      return false;
    }

    zmqpp::message reply;
    const bool ok = socket.receive(reply);
    socket.close();
    return ok;
  } catch (const std::exception &) {
    return false;
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
