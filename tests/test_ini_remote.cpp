// Tests for the remote settings editing service (src/ini/remote_edit.*):
// EditService on its own, and end to end over a CURVE ROUTER socket guarded
// by ZapAuth's per-domain allowlist.
//
// Port: 44750 (range 44750-44759, see mads_test_helpers.hpp).
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <exception>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include <zmq.hpp>

#include "mads_test_helpers.hpp"
#include "remote_edit.hpp"
#include "zap_auth.hpp"

namespace fs = std::filesystem;
using namespace Mads::Ini;
using namespace Mads::Ini::Remote;
using namespace std::chrono_literals;

namespace {

// A temp directory removed on destruction, holding mads.ini.
struct TempSettings {
  fs::path dir;
  fs::path file;
  explicit TempSettings(const std::string &content = "[broker]\nport = 1\n") {
    dir = fs::temp_directory_path() /
          ("mads_ini_remote_" +
           std::to_string(std::chrono::steady_clock::now()
                              .time_since_epoch()
                              .count()));
    fs::create_directories(dir);
    file = dir / "mads.ini";
    write(content);
  }
  ~TempSettings() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  void write(const std::string &content) const {
    std::ofstream(file, std::ios::binary) << content;
  }
  std::string read() const {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
  }
  size_t backups() const {
    size_t n = 0;
    for (const auto &e : fs::directory_iterator(dir)) {
      const auto name = e.path().filename().string();
      if (name.rfind("mads.ini.", 0) == 0 && name != "mads.ini.bak" &&
          name.size() > 4 && name.substr(name.size() - 4) == ".bak")
        ++n;
    }
    return n;
  }
};

std::vector<std::string> req(std::vector<std::string> frames) {
  frames.insert(frames.begin(), LIB_VERSION);
  return frames;
}

} // namespace

TEST_CASE("EditService get returns the text and its etag", "[ini][remote]") {
  TempSettings t;
  EditService svc(t.file);
  auto r = svc.handle(req({"get"}));
  REQUIRE(r.size() == 4);
  CHECK(r[0] == LIB_VERSION);
  CHECK(r[1] == "ok");
  CHECK(r[2] == content_etag("[broker]\nport = 1\n"));
  CHECK(r[3] == "[broker]\nport = 1\n");
}

TEST_CASE("EditService put writes, backs up and prunes", "[ini][remote]") {
  TempSettings t;
  EditService svc(t.file, 3);
  std::vector<std::string> lines;
  svc.log = [&](const std::string &l) { lines.push_back(l); };

  std::string etag = content_etag(t.read());
  for (int i = 0; i < 6; ++i) {
    const std::string text = "[broker]\nport = " + std::to_string(10 + i) + "\n";
    auto r = svc.handle(req({"put", etag, text}), "ABCDEFGHIJKL");
    REQUIRE(r.size() == 3);
    REQUIRE(r[1] == "ok");
    CHECK(r[2] == content_etag(text));
    CHECK(t.read() == text);
    etag = r[2];
  }
  // Six puts within a second or so: the counter suffix keeps them distinct and
  // pruning keeps the newest three.
  CHECK(t.backups() == 3);
  REQUIRE_FALSE(lines.empty());
  CHECK(lines.back().rfind("put by ABCDEFGH", 0) == 0);
  CHECK(lines.back().find(": ok, 19 bytes") != std::string::npos);
  CHECK_FALSE(fs::exists(t.dir / "mads.ini.tmp"));
}

TEST_CASE("EditService put conflict leaves the file untouched",
          "[ini][remote]") {
  TempSettings t;
  EditService svc(t.file);
  auto r = svc.handle(req({"put", "deadbeef", "[a]\nx = 1\n"}));
  REQUIRE(r.size() == 4);
  CHECK(r[1] == "conflict");
  CHECK(r[2] == content_etag("[broker]\nport = 1\n"));
  CHECK(r[3] == "[broker]\nport = 1\n");
  CHECK(t.read() == "[broker]\nport = 1\n");
  CHECK(t.backups() == 0);
}

TEST_CASE("EditService put with an empty base etag overwrites",
          "[ini][remote]") {
  TempSettings t;
  EditService svc(t.file);
  auto r = svc.handle(req({"put", "", "[a]\nx = 1\n"}));
  REQUIRE(r.size() == 3);
  CHECK(r[1] == "ok");
  CHECK(t.read() == "[a]\nx = 1\n");
  CHECK(t.backups() == 1);
}

TEST_CASE("EditService rejects invalid TOML and oversized files",
          "[ini][remote]") {
  TempSettings t;
  EditService svc(t.file);
  auto bad = svc.handle(req({"put", "", "[broken\n"}));
  REQUIRE(bad.size() == 3);
  CHECK(bad[1] == "error");
  CHECK(bad[2].find("not valid TOML") != std::string::npos);

  std::string big = "# " + std::string(kMaxFileSize, 'x') + "\n";
  auto large = svc.handle(req({"put", "", big}));
  REQUIRE(large.size() == 3);
  CHECK(large[1] == "error");
  CHECK(large[2].find("too large") != std::string::npos);

  CHECK(t.read() == "[broker]\nport = 1\n");
  CHECK(t.backups() == 0);
}

TEST_CASE("EditService rejects bad versions, commands and framing",
          "[ini][remote]") {
  TempSettings t;
  EditService svc(t.file);
  CHECK(svc.handle({"v1.0.0", "get"})[1] == "error");
  CHECK(svc.handle({"garbage", "get"})[1] == "error");
  CHECK(svc.handle({})[1] == "error");
  CHECK(svc.handle(req({"rm"}))[1] == "error");
  CHECK(svc.handle(req({"get", "extra"}))[1] == "error");
  CHECK(svc.handle(req({"put", "only-one-arg"}))[1] == "error");
  CHECK(t.read() == "[broker]\nport = 1\n");
}

TEST_CASE("EditService info reports the file", "[ini][remote]") {
  TempSettings t;
  EditService svc(t.file);
  auto r = svc.handle(req({"info"}));
  REQUIRE(r.size() == 3);
  REQUIRE(r[1] == "ok");
  auto j = nlohmann::json::parse(r[2]);
  CHECK(j["path"] == t.file.string());
  CHECK(j["size"] == 18);
  CHECK(j["etag"] == content_etag("[broker]\nport = 1\n"));
  CHECK(j["version"] == LIB_VERSION);
  const std::string mtime = j["mtime"];
  CHECK(mtime.size() == 20);
  CHECK(mtime.back() == 'Z');
}

// ---------------------------------------------------------------- end to end

namespace {

// Broker side of the tests: a ZAP handler with the edit domain restricted to
// the editor key, and serve() on a thread.
struct EditServer {
  zmq::context_t ctx;
  Mads::ZapAuth zap{ctx};
  Mads::CurveKeypair broker = Mads::generate_keypair();
  Mads::CurveKeypair editor = Mads::generate_keypair();
  Mads::CurveKeypair intruder = Mads::generate_keypair();
  TempSettings settings;
  EditService service{settings.file, 5};
  std::atomic<bool> running{true};
  std::thread thread;
  std::string endpoint = mads_test::loopback(44750);

  EditServer() {
    zap.allow("127.0.0.1");
    // The intruder is a valid *bus* key but not an editor key.
    zap.configure_curve(intruder.public_key);
    zap.configure_domain_curve(kZapDomain, {editor.public_key});
    zap.start();
    thread = std::thread([this] {
      serve(ctx,
            {Listener{endpoint, ServerKeys{broker.public_key,
                                           broker.secret_key}}},
            service, running);
    });
    // serve() binds on its own thread: give it a moment before clients
    // connect (ZMQ would retry anyway).
    std::this_thread::sleep_for(100ms);
  }
  ~EditServer() {
    running.store(false);
    if (thread.joinable())
      thread.join();
  }
  ClientKeys keys(const Mads::CurveKeypair &kp) const {
    return {kp.public_key, kp.secret_key, broker.public_key};
  }
};

} // namespace

TEST_CASE("RemoteSource edits the file through the CURVE edit socket",
          "[ini][remote]") {
  EditServer server;
  RemoteSource source(server.endpoint, server.keys(server.editor), 3000ms);

  CHECK(source.describe() == server.endpoint + " (remote)");

  Loaded loaded = source.load();
  CHECK(loaded.text == "[broker]\nport = 1\n");
  CHECK(loaded.etag == content_etag(loaded.text));

  SaveResult saved = source.save("[broker]\nport = 2\n", loaded.etag);
  REQUIRE(saved.status == SaveResult::Status::Ok);
  CHECK(saved.etag == content_etag("[broker]\nport = 2\n"));
  CHECK(server.settings.read() == "[broker]\nport = 2\n");
  CHECK(server.settings.backups() == 1);

  // The stale etag now conflicts, and nothing is written.
  SaveResult stale = source.save("[broker]\nport = 3\n", loaded.etag);
  CHECK(stale.status == SaveResult::Status::Conflict);
  CHECK(stale.etag == saved.etag);
  CHECK(server.settings.read() == "[broker]\nport = 2\n");

  SaveResult invalid = source.save("[oops\n", saved.etag);
  CHECK(invalid.status == SaveResult::Status::Error);

  auto j = nlohmann::json::parse(source.info());
  CHECK(j["etag"] == saved.etag);
  CHECK(j["version"] == LIB_VERSION);
}

TEST_CASE("RemoteSource with a non-editor key gets no answer",
          "[ini][remote]") {
  EditServer server;
  RemoteSource source(server.endpoint, server.keys(server.intruder), 500ms);
  REQUIRE_THROWS_AS(source.load(), std::runtime_error);
  SaveResult r = source.save("[a]\nx = 1\n", "");
  CHECK(r.status == SaveResult::Status::Error);
  CHECK(server.settings.read() == "[broker]\nport = 1\n");
}

// ------------------------------------------------------------- plaintext ipc

namespace {

bool have_ipc() { return zmq_has("ipc") != 0; }

// A short private directory for ipc socket files (sun_path is ~104 bytes).
struct TempIpcDir {
  fs::path dir;
  TempIpcDir() {
#ifdef _WIN32
    dir = fs::temp_directory_path();
#else
    dir = fs::path("/tmp");
#endif
    dir /= "mads_ir_" + std::to_string(std::chrono::steady_clock::now()
                                           .time_since_epoch()
                                           .count() %
                                       1000000000);
  }
  ~TempIpcDir() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};

// Runs serve() on one plaintext ipc listener and waits until it is bound.
struct IpcServer {
  TempIpcDir tmp;
  zmq::context_t ctx;
  std::unique_ptr<Mads::ZapAuth> zap;
  TempSettings settings;
  EditService service{settings.file};
  std::atomic<bool> running{true};
  std::thread thread;
  std::string endpoint;
  std::string path;
  std::exception_ptr error;

  explicit IpcServer(bool with_zap) {
    if (with_zap) {
      // As in a broker run with --crypto: a handler is bound, with a
      // restrictive default allowlist and an address whitelist.
      auto other = Mads::generate_keypair();
      zap = std::make_unique<Mads::ZapAuth>(ctx);
      zap->allow("127.0.0.1");
      zap->configure_curve(other.public_key);
      zap->configure_domain_curve(kZapDomain, {other.public_key});
      zap->start();
    }
    endpoint = local_endpoint(44751, tmp.dir);
    path = endpoint.substr(std::string("ipc://").size());
    thread = std::thread([this] {
      try {
        serve(ctx, {Listener{endpoint, std::nullopt}}, service, running);
      } catch (...) {
        error = std::current_exception();
      }
    });
    for (int i = 0; i < 100 && !fs::exists(path) && !error; ++i)
      std::this_thread::sleep_for(20ms);
  }
  ~IpcServer() {
    running.store(false);
    if (thread.joinable())
      thread.join();
  }
};

void exercise_plaintext(IpcServer &server) {
  REQUIRE_FALSE(server.error);
  RemoteSource source(server.endpoint, std::nullopt, 3000ms);
  Loaded loaded = source.load();
  CHECK(loaded.text == "[broker]\nport = 1\n");
  SaveResult saved = source.save("[broker]\nport = 2\n", loaded.etag);
  REQUIRE(saved.status == SaveResult::Status::Ok);
  CHECK(server.settings.read() == "[broker]\nport = 2\n");
  CHECK(source.save("[broker]\nport = 3\n", loaded.etag).status ==
        SaveResult::Status::Conflict);
  CHECK(nlohmann::json::parse(source.info())["etag"] == saved.etag);
}

} // namespace

TEST_CASE("plaintext ipc edit service works without a ZAP handler",
          "[ini][remote]") {
  if (!have_ipc())
    SKIP("no ipc transport");
  IpcServer server(false);
  exercise_plaintext(server);
}

TEST_CASE("plaintext ipc edit service works next to a ZAP handler",
          "[ini][remote]") {
  if (!have_ipc())
    SKIP("no ipc transport");
  IpcServer server(true);
  exercise_plaintext(server);
}

TEST_CASE("a second serve() cannot take over a live ipc socket",
          "[ini][remote]") {
  if (!have_ipc())
    SKIP("no ipc transport");
  IpcServer first(false);
  REQUIRE_FALSE(first.error);
  EditService other(first.settings.file);
  std::atomic<bool> running{true};
  zmq::context_t ctx2;
  REQUIRE_THROWS_AS(serve(ctx2, {Listener{first.endpoint, std::nullopt}},
                          other, running),
                    std::runtime_error);
  // The first one is unaffected, and its file is still there.
  RemoteSource source(first.endpoint, std::nullopt, 3000ms);
  CHECK(source.load().text == "[broker]\nport = 1\n");
}

TEST_CASE("serve() removes its ipc socket file on return", "[ini][remote]") {
  if (!have_ipc())
    SKIP("no ipc transport");
  std::string path;
  {
    IpcServer server(false);
    path = server.path;
    REQUIRE(fs::exists(path));
    server.running.store(false);
    server.thread.join();
    CHECK_FALSE(fs::exists(path));
  }
}

TEST_CASE("serve() refuses plaintext on anything but ipc", "[ini][remote]") {
  TempSettings t;
  EditService svc(t.file);
  zmq::context_t ctx;
  std::atomic<bool> running{true};
  CHECK_THROWS_AS(
      serve(ctx, {Listener{mads_test::loopback(44752), std::nullopt}}, svc,
            running),
      std::invalid_argument);
}

TEST_CASE("RemoteSource validates endpoint and keys", "[ini][remote]") {
  ClientKeys keys{"a", "b", "c"};
  CHECK_THROWS_AS(RemoteSource("tcp://127.0.0.1:44753", std::nullopt),
                  std::invalid_argument);
  CHECK_THROWS_AS(RemoteSource("ipc:///tmp/x.ipc", keys),
                  std::invalid_argument);
  CHECK_THROWS_AS(RemoteSource("nonsense", std::nullopt),
                  std::invalid_argument);
  CHECK_NOTHROW(RemoteSource("tcp://127.0.0.1:44753", keys));
  CHECK_NOTHROW(RemoteSource("ipc:///tmp/x.ipc", std::nullopt));
}
