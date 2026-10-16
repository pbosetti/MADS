// Kernel socket buffers for ipc:// endpoints: IpcBuffers::resolve() and
// with_ipc_buffers() (src/detail/socket_options.hpp), and -- end to end --
// the libzmq patch in vendors/patches/libzmq-ipc-socket-buffers.patch that
// makes ZMQ_SNDBUF/ZMQ_RCVBUF reach AF_UNIX sockets at all.
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <toml++/toml.hpp>
#include <zmq.hpp>

#include "detail/runtime_dir.hpp"
#include "detail/socket_options.hpp"

#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace Mads::detail;

namespace {
toml::table parse(const char *text) { return toml::parse(text); }
} // namespace

TEST_CASE("IpcBuffers::resolve: 1 MiB by default, agent section wins",
          "[ipc_buffers]") {
  auto none = parse("[agents]\n[me]\n");
  auto d = IpcBuffers::resolve(none["agents"], none["me"]);
  CHECK(d.sndbuf == IPC_SOCKBUF_DEFAULT);
  CHECK(d.rcvbuf == IPC_SOCKBUF_DEFAULT);

  auto cfg = parse("[agents]\nipc_sndbuf = 262144\nipc_rcvbuf = 0\n"
                   "[me]\nipc_sndbuf = 4194304\n");
  auto b = IpcBuffers::resolve(cfg["agents"], cfg["me"]);
  CHECK(b.sndbuf == 4194304); // agent section
  CHECK(b.rcvbuf == 0);       // fleet: keep the OS default
}

TEST_CASE("with_ipc_buffers restores the socket options afterwards",
          "[ipc_buffers]") {
  zmq::context_t ctx;
  zmq::socket_t s(ctx, zmq::socket_type::pub);
  s.set(zmq::sockopt::sndbuf, 65536); // a plain sndbuf setting
  bool called = false;
  with_ipc_buffers(s, {1 << 20, 0}, [&] {
    called = true;
    CHECK(s.get(zmq::sockopt::sndbuf) == (1 << 20));
    CHECK(s.get(zmq::sockopt::rcvbuf) == -1); // <= 0: untouched
  });
  CHECK(called);
  CHECK(s.get(zmq::sockopt::sndbuf) == 65536);
  CHECK(s.get(zmq::sockopt::rcvbuf) == -1);

  SECTION("also when the bind/connect throws") {
    CHECK_THROWS(with_ipc_buffers(s, {1 << 20, 1 << 20}, [] {
      throw zmq::error_t();
    }));
    CHECK(s.get(zmq::sockopt::sndbuf) == 65536);
    CHECK(s.get(zmq::sockopt::rcvbuf) == -1);
  }
}

#ifndef _WIN32
// The accepted connection's file descriptor comes with ZMQ_EVENT_ACCEPTED.
TEST_CASE("an ipc endpoint gets the ipc buffers, a tcp one on the same "
          "socket does not",
          "[ipc_buffers]") {
  if (!zmq_has("ipc"))
    SKIP("this libzmq has no ipc transport");
  constexpr int ipc_size = 1 << 20;
  zmq::context_t ctx;
  zmq::socket_t server(ctx, zmq::socket_type::pub);
  zmq_socket_monitor(server.handle(), "inproc://ipc-buffers-mon",
                     ZMQ_EVENT_ACCEPTED);
  zmq::socket_t mon(ctx, zmq::socket_type::pair);
  mon.connect("inproc://ipc-buffers-mon");

  const std::string ipc_ep =
      "ipc://" + (runtime_dir() / ("test-ipcbuf-" + std::to_string(::getpid()) +
                                   ".ipc"))
                     .string();
  with_ipc_buffers(server, {ipc_size, ipc_size}, [&] { server.bind(ipc_ep); });
  server.bind("tcp://127.0.0.1:*");
  const std::string tcp_ep = server.get(zmq::sockopt::last_endpoint);

  zmq::socket_t c1(ctx, zmq::socket_type::sub), c2(ctx, zmq::socket_type::sub);
  c1.connect(ipc_ep);
  c2.connect(tcp_ep);

  // endpoint -> accepted fd
  std::map<std::string, int> fds;
  mon.set(zmq::sockopt::rcvtimeo, 2000);
  while (fds.size() < 2) {
    zmq::message_t ev, addr;
    REQUIRE(mon.recv(ev));
    REQUIRE(mon.recv(addr));
    uint32_t value;
    std::memcpy(&value, static_cast<const char *>(ev.data()) + 2, 4);
    fds[addr.to_string()] = static_cast<int>(value);
  }
  auto sndbuf_of = [](int fd) {
    int v = 0;
    socklen_t len = sizeof v;
    REQUIRE(getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &v, &len) == 0);
    return v;
  };
  // Linux reports twice the requested size; macOS reports it as is.
  CHECK(sndbuf_of(fds.at(ipc_ep)) >= ipc_size);
  CHECK(sndbuf_of(fds.at(tcp_ep)) < ipc_size);
  CHECK(server.get(zmq::sockopt::sndbuf) == -1);

  c1.close();
  c2.close();
  mon.close();
  server.close();
  ctx.close();
  std::error_code ec;
  std::filesystem::remove(ipc_ep.substr(6), ec);
}
#endif
