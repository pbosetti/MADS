// Unit tests for Mads::CurveAuth (src/curve.hpp), the ZAP/CURVE helper used
// by Mads::Agent and by the C ABI's crypto entry points (agent_c.cpp).
//
// Keys are generated at runtime with zmqpp::curve::generate_keypair() and
// written as <name>.pub / <name>.key files into a fresh subdirectory of
// std::filesystem::temp_directory_path(), removed at the end of each test
// (RAII helper below) -- no fixture files are checked in.
//
// Port range for this file: 42300-42399 (WP-D), disjoint from test_agent_c.cpp
// (42300-42304 used there); this file uses 42350+.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "curve.hpp"
#include "mads_test_helpers.hpp"

using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

// Creates a unique temp directory for one test case and removes it (and its
// contents) on scope exit.
struct TempKeyDir {
  fs::path path;
  TempKeyDir() {
    path = fs::temp_directory_path() /
           ("mads_test_curve_" +
            std::to_string(std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count()));
    fs::create_directories(path);
  }
  ~TempKeyDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

void write_keypair(const fs::path &dir, const std::string &name,
                   const zmqpp::curve::keypair &kp) {
  {
    std::ofstream pub(dir / (name + ".pub"));
    pub << kp.public_key << "\n";
  }
  {
    std::ofstream key(dir / (name + ".key"));
    key << kp.secret_key << "\n";
  }
}

} // namespace

// ---------------------------------------------------------------------------
// fetch_public_keys()
// ---------------------------------------------------------------------------

TEST_CASE("fetch_public_keys throws when the directory does not exist",
          "[curve]") {
  zmqpp::context ctx;
  Mads::CurveAuth auth(ctx);
  bool threw = false;
  try {
    auth.fetch_public_keys("/no/such/curve/key/dir/at/all");
  } catch (const std::exception &e) {
    threw = true;
    REQUIRE(std::string(e.what()).find("does not exist") != std::string::npos);
  }
  REQUIRE(threw);
}

TEST_CASE("fetch_public_keys throws when the directory has no .pub files",
          "[curve]") {
  TempKeyDir dir;
  zmqpp::context ctx;
  Mads::CurveAuth auth(ctx);
  bool threw = false;
  try {
    auth.fetch_public_keys(dir.path);
  } catch (const std::exception &e) {
    threw = true;
    REQUIRE(std::string(e.what()).find("No client public keys found") !=
            std::string::npos);
  }
  REQUIRE(threw);
}

TEST_CASE("fetch_public_keys succeeds when .pub files are present",
          "[curve]") {
  TempKeyDir dir;
  auto kp1 = zmqpp::curve::generate_keypair();
  auto kp2 = zmqpp::curve::generate_keypair();
  write_keypair(dir.path, "clientA", kp1);
  write_keypair(dir.path, "clientB", kp2);
  // A stray non-.pub file must be ignored, not mistaken for a key.
  { std::ofstream junk(dir.path / "notes.txt"); junk << "hello"; }

  zmqpp::context ctx;
  Mads::CurveAuth auth(ctx);
  REQUIRE_NOTHROW(auth.fetch_public_keys(dir.path));
}

// ---------------------------------------------------------------------------
// set_key_dir() / getters
// ---------------------------------------------------------------------------

TEST_CASE("set_key_dir throws for a nonexistent directory and succeeds for "
          "a valid one",
          "[curve]") {
  zmqpp::context ctx;
  Mads::CurveAuth auth(ctx);
  REQUIRE_THROWS_AS(auth.set_key_dir("/still/not/a/real/dir"),
                    std::runtime_error);

  TempKeyDir dir;
  REQUIRE_NOTHROW(auth.set_key_dir(dir.path));
}

TEST_CASE("client/server key getters and setters round-trip and default to "
          "empty",
          "[curve]") {
  zmqpp::context ctx;
  Mads::CurveAuth auth(ctx);
  REQUIRE(auth.client_public_key().empty());
  REQUIRE(auth.client_secret_key().empty());
  REQUIRE(auth.server_public_key().empty());

  auth.set_client_public_key("pubkeyvalue");
  auth.set_client_secret_key("seckeyvalue");
  auth.set_server_public_key("srvkeyvalue");

  REQUIRE(auth.client_public_key() == "pubkeyvalue");
  REQUIRE(auth.client_secret_key() == "seckeyvalue");
  REQUIRE(auth.server_public_key() == "srvkeyvalue");
}

// ---------------------------------------------------------------------------
// setup_curve_server()
// ---------------------------------------------------------------------------

TEST_CASE("setup_curve_server throws when fetch_public_keys was never "
          "called (key dir unset)",
          "[curve]") {
  zmqpp::context ctx;
  Mads::CurveAuth auth(ctx);
  zmqpp::socket sock(ctx, zmqpp::socket_type::rep);
  bool threw = false;
  try {
    auth.setup_curve_server(sock, "server");
  } catch (const std::exception &e) {
    threw = true;
    REQUIRE(std::string(e.what()).find("Key directory not set") !=
            std::string::npos);
  }
  REQUIRE(threw);
}

TEST_CASE("setup_curve_server throws when the server key files are missing",
          "[curve]") {
  TempKeyDir dir;
  // Only a client key is present; no "server.pub"/"server.key".
  auto client_kp = zmqpp::curve::generate_keypair();
  write_keypair(dir.path, "client", client_kp);

  zmqpp::context ctx;
  Mads::CurveAuth auth(ctx);
  auth.fetch_public_keys(dir.path);
  zmqpp::socket sock(ctx, zmqpp::socket_type::rep);
  bool threw = false;
  try {
    auth.setup_curve_server(sock, "server");
  } catch (const std::exception &e) {
    threw = true;
    REQUIRE(std::string(e.what()).find("server public key file") !=
            std::string::npos);
  }
  REQUIRE(threw);
}

TEST_CASE("setup_curve_server succeeds and configures the socket when key "
          "files are present",
          "[curve]") {
  TempKeyDir dir;
  auto server_kp = zmqpp::curve::generate_keypair();
  auto client_kp = zmqpp::curve::generate_keypair();
  write_keypair(dir.path, "server", server_kp);
  write_keypair(dir.path, "client", client_kp);

  zmqpp::context ctx;
  Mads::CurveAuth auth(ctx);
  auth.fetch_public_keys(dir.path); // picks up both server.pub and client.pub
  zmqpp::socket sock(ctx, zmqpp::socket_type::rep);
  REQUIRE_NOTHROW(auth.setup_curve_server(sock, "server"));
}

// ---------------------------------------------------------------------------
// setup_curve_client() -- file-based overload
// ---------------------------------------------------------------------------

TEST_CASE("setup_curve_client(file-based) throws when key files are "
          "missing",
          "[curve]") {
  TempKeyDir dir;
  auto client_kp = zmqpp::curve::generate_keypair();
  write_keypair(dir.path, "client", client_kp);
  // No "server.pub" present.

  zmqpp::context ctx;
  Mads::CurveAuth auth(ctx);
  REQUIRE_NOTHROW(auth.set_key_dir(dir.path));
  zmqpp::socket sock(ctx, zmqpp::socket_type::req);
  bool threw = false;
  try {
    auth.setup_curve_client(sock, "client", "server");
  } catch (const std::exception &e) {
    threw = true;
    REQUIRE(std::string(e.what()).find("server public key file") !=
            std::string::npos);
  }
  REQUIRE(threw);
}

TEST_CASE("setup_curve_client(file-based) succeeds when all key files are "
          "present",
          "[curve]") {
  TempKeyDir dir;
  auto client_kp = zmqpp::curve::generate_keypair();
  auto server_kp = zmqpp::curve::generate_keypair();
  write_keypair(dir.path, "client", client_kp);
  write_keypair(dir.path, "server", server_kp);

  zmqpp::context ctx;
  Mads::CurveAuth auth(ctx);
  auth.set_key_dir(dir.path);
  zmqpp::socket sock(ctx, zmqpp::socket_type::req);
  REQUIRE_NOTHROW(auth.setup_curve_client(sock, "client", "server"));
  REQUIRE(auth.client_public_key() == client_kp.public_key);
  REQUIRE(auth.client_secret_key() == client_kp.secret_key);
  REQUIRE(auth.server_public_key() == server_kp.public_key);
}

// ---------------------------------------------------------------------------
// setup_curve_client() -- raw-key overload
// ---------------------------------------------------------------------------

TEST_CASE("setup_curve_client(no-arg) throws until all three keys are set, "
          "then succeeds",
          "[curve]") {
  zmqpp::context ctx;
  Mads::CurveAuth auth(ctx);
  zmqpp::socket sock(ctx, zmqpp::socket_type::req);

  REQUIRE_THROWS_AS(auth.setup_curve_client(sock), std::runtime_error);

  auto client_kp = zmqpp::curve::generate_keypair();
  auto server_kp = zmqpp::curve::generate_keypair();
  auth.set_client_public_key(client_kp.public_key);
  REQUIRE_THROWS_AS(auth.setup_curve_client(sock), std::runtime_error);
  auth.set_client_secret_key(client_kp.secret_key);
  REQUIRE_THROWS_AS(auth.setup_curve_client(sock), std::runtime_error);
  auth.set_server_public_key(server_kp.public_key);

  REQUIRE_NOTHROW(auth.setup_curve_client(sock));
}

// ---------------------------------------------------------------------------
// setup_auth(): whitelist configuration must not throw with a real context.
// ---------------------------------------------------------------------------

TEST_CASE("setup_auth configures the ZAP authenticator without throwing",
          "[curve]") {
  zmqpp::context ctx;
  Mads::CurveAuth auth(ctx);
  auth.allowed_ips.push_back("127.0.0.1");
  REQUIRE_NOTHROW(auth.setup_auth(Mads::auth_verbose::off));
}

// ---------------------------------------------------------------------------
// Stretch: end-to-end CURVE-encrypted REQ/REP round trip including the ZAP
// authenticator, over tcp://127.0.0.1:<port>.
// ---------------------------------------------------------------------------

TEST_CASE("full CURVE-encrypted REQ/REP round trip with ZAP authentication",
          "[curve]") {
  const uint16_t port = 42350;
  TempKeyDir dir;
  auto server_kp = zmqpp::curve::generate_keypair();
  auto client_kp = zmqpp::curve::generate_keypair();
  write_keypair(dir.path, "server", server_kp);
  write_keypair(dir.path, "client", client_kp);

  // Server side: the ZAP authenticator must live in the same context as the
  // server socket it protects.
  zmqpp::context server_ctx;
  Mads::CurveAuth server_auth(server_ctx);
  server_auth.allowed_ips.push_back("127.0.0.1");
  server_auth.setup_auth(Mads::auth_verbose::off);
  server_auth.fetch_public_keys(dir.path); // registers client.pub (and server.pub)

  zmqpp::socket server_sock(server_ctx, zmqpp::socket_type::rep);
  server_auth.setup_curve_server(server_sock, "server");
  server_sock.set(zmqpp::socket_option::receive_timeout, 200);
  server_sock.bind(mads_test::loopback(port));

  // Client side: separate context (as a different process would have).
  zmqpp::context client_ctx;
  Mads::CurveAuth client_auth(client_ctx);
  client_auth.set_key_dir(dir.path);
  zmqpp::socket client_sock(client_ctx, zmqpp::socket_type::req);
  client_auth.setup_curve_client(client_sock, "client", "server");
  client_sock.set(zmqpp::socket_option::receive_timeout, 3000);
  client_sock.connect(mads_test::loopback(port));

  zmqpp::message request;
  request << "ping";
  REQUIRE(client_sock.send(request));

  bool server_got = mads_test::wait_for(
      [&] {
        zmqpp::message in;
        if (!server_sock.receive(in)) return false;
        REQUIRE(in.get(0) == "ping");
        zmqpp::message reply;
        reply << "pong";
        server_sock.send(reply);
        return true;
      },
      3000ms, 50ms);
  REQUIRE(server_got);

  zmqpp::message response;
  bool client_got = client_sock.receive(response);
  REQUIRE(client_got);
  REQUIRE(response.get(0) == "pong");
}
