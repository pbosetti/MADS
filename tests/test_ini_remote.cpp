// Tests for the remote settings editing service (src/ini/remote_edit.*):
// EditService on its own, and end to end over a CURVE ROUTER socket guarded
// by ZapAuth's per-domain allowlist.
//
// Port: 44750 (range 44750-44759, see mads_test_helpers.hpp).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include <sodium.h>
#include <sys/stat.h>
#include <zmq.hpp>
#include <zmq_addon.hpp>

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
           std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()));
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

// [LIB_VERSION, command, "" (no auth), args...] from command, args...
std::vector<std::string> req(std::vector<std::string> frames) {
  frames.insert(frames.begin() + 1, "");
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
    const std::string text =
        "[broker]\nport = " + std::to_string(10 + i) + "\n";
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

// ------------------------------------------------------------ password helpers

namespace {

std::string sha256_of(const std::string &data) {
  std::string out(crypto_hash_sha256_BYTES, '\0');
  crypto_hash_sha256(reinterpret_cast<unsigned char *>(out.data()),
                     reinterpret_cast<const unsigned char *>(data.data()),
                     data.size());
  return out;
}

std::string hmac_of(const std::string &key, const std::string &msg) {
  std::string out(crypto_auth_hmacsha256_BYTES, '\0');
  crypto_auth_hmacsha256(reinterpret_cast<unsigned char *>(out.data()),
                         reinterpret_cast<const unsigned char *>(msg.data()),
                         msg.size(),
                         reinterpret_cast<const unsigned char *>(key.data()));
  return out;
}

std::string b64d(const std::string &text, size_t n) {
  std::string out(n, '\0');
  size_t len = 0;
  REQUIRE(sodium_base642bin(reinterpret_cast<unsigned char *>(out.data()), n,
                            text.data(), text.size(), nullptr, &len, nullptr,
                            sodium_base64_VARIANT_ORIGINAL) == 0);
  REQUIRE(len == n);
  return out;
}

// A verifier with libsodium's cheapest Argon2id parameters, built through the
// public ClientSecret::derive (PasswordVerifier::create is deliberately slow).
PasswordVerifier make_verifier(const std::string &password) {
  REQUIRE(sodium_init() >= 0);
  std::string salt(16, '\0');
  randombytes_buf(salt.data(), salt.size());
  ClientSecret s = ClientSecret::derive(
      password, salt, crypto_pwhash_OPSLIMIT_MIN, crypto_pwhash_MEMLIMIT_MIN);
  PasswordVerifier v;
  v.opslimit = crypto_pwhash_OPSLIMIT_MIN;
  v.memlimit = crypto_pwhash_MEMLIMIT_MIN;
  v.salt = salt;
  v.stored_key = sha256_of(s.client_key);
  v.server_key = s.server_key;
  return v;
}

std::string lp(const std::string &x) {
  std::string out(8, '\0');
  uint64_t n = x.size();
  for (int i = 7; i >= 0; --i, n >>= 8)
    out[i] = static_cast<char>(n & 0xff);
  return out + x;
}

std::string hash_frames(const std::vector<std::string> &frames) {
  std::string all;
  for (const auto &f : frames)
    all += lp(f);
  return sha256_of(all);
}

// An independent implementation of the client side of the protocol (as
// specified in remote_edit.hpp), to test EditService without sockets.
struct Client {
  std::string password;
  explicit Client(std::string pw) : password(std::move(pw)) {}

  // hello -> parsed reply
  nlohmann::json hello(EditService &svc, bool require = true) {
    auto r = svc.handle(req({"hello"}), "", require);
    REQUIRE(r.size() == 3);
    REQUIRE(r[1] == "ok");
    return nlohmann::json::parse(r[2]);
  }

  struct Built {
    std::vector<std::string> request;
    std::string message;
    ClientSecret secret;
  };

  Built build(const nlohmann::json &hello, const std::string &command,
              const std::vector<std::string> &args) {
    Built b;
    b.secret = ClientSecret::derive(password, b64d(hello["salt"], 16),
                                    hello["opslimit"], hello["memlimit"]);
    std::string server_nonce = b64d(hello["nonce"], 32);
    std::string client_nonce(32, '\0');
    randombytes_buf(client_nonce.data(), 32);
    b.message = std::string("mads-edit-v1") + server_nonce + client_nonce +
                command + '\0' + hash_frames(args);
    std::string sig = hmac_of(sha256_of(b.secret.client_key), b.message);
    std::string proof(32, '\0');
    for (int i = 0; i < 32; ++i)
      proof[i] = static_cast<char>(b.secret.client_key[i] ^ sig[i]);
    b.request = {LIB_VERSION, command, server_nonce + client_nonce + proof};
    b.request.insert(b.request.end(), args.begin(), args.end());
    return b;
  }

  // True if the last frame is a valid signature over the others.
  static bool signature_ok(const Built &b, std::vector<std::string> reply) {
    if (reply.size() < 3)
      return false;
    std::string sig = reply.back();
    reply.pop_back();
    return sig == hmac_of(b.secret.server_key, b.message + hash_frames(reply));
  }
};

} // namespace

TEST_CASE("PasswordVerifier create, to_string and parse round trip",
          "[ini][remote][password]") {
  const PasswordVerifier v = PasswordVerifier::create("correct horse");
  CHECK(v.opslimit == crypto_pwhash_OPSLIMIT_MODERATE);
  CHECK(v.memlimit == crypto_pwhash_MEMLIMIT_MODERATE);
  CHECK(v.salt.size() == 16);
  CHECK(v.stored_key.size() == 32);
  CHECK(v.server_key.size() == 32);
  const std::string line = v.to_string();
  CHECK(line.rfind("mads-edit-password v1 argon2id13 ", 0) == 0);
  CHECK(line.find('\n') == std::string::npos);

  std::string why;
  auto back = PasswordVerifier::parse(line, &why);
  REQUIRE(back);
  CHECK(back->opslimit == v.opslimit);
  CHECK(back->memlimit == v.memlimit);
  CHECK(back->salt == v.salt);
  CHECK(back->stored_key == v.stored_key);
  CHECK(back->server_key == v.server_key);
  CHECK(back->to_string() == line);

  // The client side derives matching keys from the same password and params.
  ClientSecret s =
      ClientSecret::derive("correct horse", v.salt, v.opslimit, v.memlimit);
  CHECK(sha256_of(s.client_key) == v.stored_key);
  CHECK(s.server_key == v.server_key);
}

TEST_CASE("PasswordVerifier::parse rejects malformed lines and bad parameters",
          "[ini][remote][password]") {
  const PasswordVerifier v = make_verifier("x");
  const std::string good = v.to_string();
  REQUIRE(PasswordVerifier::parse(good));

  std::string why;
  CHECK_FALSE(PasswordVerifier::parse("", &why));
  CHECK_FALSE(why.empty());
  CHECK_FALSE(PasswordVerifier::parse("hello world"));
  auto replace_token = [&](size_t index, const std::string &with) {
    std::istringstream in(good);
    std::vector<std::string> tok;
    for (std::string t; in >> t;)
      tok.push_back(t);
    tok[index] = with;
    std::string out;
    for (auto &t : tok)
      out += (out.empty() ? "" : " ") + t;
    return out;
  };
  CHECK_FALSE(PasswordVerifier::parse(replace_token(1, "v2")));
  CHECK_FALSE(PasswordVerifier::parse(replace_token(2, "argon2i13")));
  CHECK_FALSE(PasswordVerifier::parse(replace_token(3, "0")));
  CHECK_FALSE(PasswordVerifier::parse(replace_token(3, "abc")));
  CHECK_FALSE(
      PasswordVerifier::parse(replace_token(3, "99999999999999999999999")));
  CHECK_FALSE(PasswordVerifier::parse(replace_token(4, "1")));
  CHECK_FALSE(
      PasswordVerifier::parse(replace_token(4, "18446744073709551615")));
  CHECK_FALSE(PasswordVerifier::parse(replace_token(5, "AAAA")));
  CHECK_FALSE(PasswordVerifier::parse(replace_token(6, "!!!!")));
  CHECK_FALSE(PasswordVerifier::parse(good + " extra"));
  // ClientSecret::derive refuses what a hostile hello could ask for.
  CHECK_THROWS_AS(ClientSecret::derive("p", std::string(16, 'a'),
                                       crypto_pwhash_OPSLIMIT_MIN,
                                       size_t(2) << 30),
                  std::invalid_argument);
  CHECK_THROWS_AS(ClientSecret::derive("p", std::string(16, 'a'), 0,
                                       crypto_pwhash_MEMLIMIT_MIN),
                  std::invalid_argument);
  CHECK_THROWS_AS(ClientSecret::derive("p", std::string(8, 'a'),
                                       crypto_pwhash_OPSLIMIT_MIN,
                                       crypto_pwhash_MEMLIMIT_MIN),
                  std::invalid_argument);
}

TEST_CASE("password file: write, read, permissions warning",
          "[ini][remote][password]") {
  TempSettings t;
  const fs::path file = password_file_for(t.file);
  CHECK(file.filename() == "mads.ini.edit_password");
  std::vector<std::string> warnings;
  CHECK_FALSE(read_password_file(file, &warnings)); // absent

  const PasswordVerifier v = make_verifier("pw");
  std::string error;
  REQUIRE(write_password_file(file, v, &error));
  auto back = read_password_file(file, &warnings);
  REQUIRE(back);
  CHECK(back->to_string() == v.to_string());
  CHECK(warnings.empty());
  CHECK_FALSE(fs::exists(file.string() + ".tmp"));
#ifndef _WIN32
  struct stat st{};
  REQUIRE(::stat(file.c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0600);
  ::chmod(file.c_str(), 0644);
  read_password_file(file, &warnings);
  CHECK(warnings.size() == 1);
#endif
  // Overwrite keeps working; garbage is an error, not "no password".
  REQUIRE(write_password_file(file, make_verifier("other"), &error));
  t.write("");
  std::ofstream(file) << "garbage\n";
  CHECK_THROWS_AS(read_password_file(file), std::runtime_error);
}

TEST_CASE("EditService password: hello, valid proof, signed reply",
          "[ini][remote][password]") {
  TempSettings t;
  EditService svc(t.file, 5, make_verifier("hunter2"));
  std::vector<std::string> lines;
  svc.log = [&](const std::string &l) { lines.push_back(l); };
  Client c("hunter2");

  auto h = c.hello(svc);
  CHECK(h["auth"] == "password");
  auto b = c.build(h, "get", {});
  auto reply = svc.handle(b.request, "KEYKEYKEYKEY", true);
  REQUIRE(reply.size() == 5);
  CHECK(reply[1] == "ok");
  CHECK(reply[3] == "[broker]\nport = 1\n");
  CHECK(Client::signature_ok(b, reply));

  // A put with a fresh nonce works and its reply is signed too.
  auto h2 = c.hello(svc);
  auto b2 =
      c.build(h2, "put",
              {content_etag("[broker]\nport = 1\n"), "[broker]\nport = 5\n"});
  auto r2 = svc.handle(b2.request, "KEYKEYKEYKEY", true);
  REQUIRE(r2.size() == 4);
  CHECK(r2[1] == "ok");
  CHECK(Client::signature_ok(b2, r2));
  CHECK(t.read() == "[broker]\nport = 5\n");
  CHECK(lines.back().rfind("put by KEYKEYKE\xE2\x80\xA6+password: ok", 0) == 0);

  // Error replies to authenticated requests are signed as well.
  auto h3 = c.hello(svc);
  auto b3 = c.build(h3, "put", {"", "[broken\n"});
  auto r3 = svc.handle(b3.request, "KEYKEYKEYKEY", true);
  CHECK(r3[1] == "error");
  CHECK(Client::signature_ok(b3, r3));
}

TEST_CASE("EditService password: wrong password, replay, tampering",
          "[ini][remote][password]") {
  TempSettings t;
  EditService svc(t.file, 5, make_verifier("hunter2"));
  std::vector<std::string> lines;
  svc.log = [&](const std::string &l) { lines.push_back(l); };

  // Wrong password.
  Client bad("wrong");
  auto h = bad.hello(svc);
  auto r = svc.handle(bad.build(h, "get", {}).request, "", true);
  CHECK(r[1] == "auth");
  CHECK(r.size() == 3);
  CHECK(r[2] == "authentication failed");
  REQUIRE_FALSE(lines.empty());
  CHECK(lines.back().rfind("auth failure by password", 0) == 0);

  // Replay: a valid request works once, the same bytes never again.
  Client good("hunter2");
  auto h2 = good.hello(svc);
  auto b = good.build(h2, "get", {});
  CHECK(svc.handle(b.request, "", true)[1] == "ok");
  CHECK(svc.handle(b.request, "", true)[1] == "auth");

  // Tampering: a proof made for one put text does not authorize another.
  auto h3 = good.hello(svc);
  auto signed_for = good.build(h3, "put", {"", "[a]\nx = 1\n"});
  auto tampered = signed_for.request;
  tampered[4] = "[a]\nx = 666\n";
  CHECK(svc.handle(tampered, "", true)[1] == "auth");
  CHECK(t.read() == "[broker]\nport = 1\n");
  // Same for the command: a get proof cannot be turned into an info.
  auto h4 = good.hello(svc);
  auto get_proof = good.build(h4, "get", {});
  get_proof.request[1] = "info";
  CHECK(svc.handle(get_proof.request, "", true)[1] == "auth");

  // No auth frame at all.
  CHECK(svc.handle(req({"get"}), "", true)[1] == "auth");
}

TEST_CASE("EditService password: nonces are consumed whatever the outcome",
          "[ini][remote][password]") {
  TempSettings t;
  EditService svc(t.file, 5, make_verifier("hunter2"));
  Client good("hunter2"), bad("wrong");

  // A wrong proof burns its nonce: the right proof on the same nonce fails.
  auto h = good.hello(svc);
  auto wrong = bad.build(h, "get", {});
  auto right = good.build(h, "get", {});
  CHECK(svc.handle(wrong.request, "", true)[1] == "auth");
  CHECK(svc.handle(right.request, "", true)[1] == "auth");

  // A nonce the server never issued.
  auto h2 = good.hello(svc);
  auto forged = good.build(h2, "get", {});
  forged.request[2][0] ^= 1; // flips a bit of the server nonce
  CHECK(svc.handle(forged.request, "", true)[1] == "auth");
  // ... and the genuine one it came from is still usable once.
  auto genuine = good.build(h2, "get", {});
  CHECK(svc.handle(genuine.request, "", true)[1] == "ok");
  CHECK(svc.handle(genuine.request, "", true)[1] == "auth");
}

TEST_CASE("EditService password: lockout after 5 failed proofs",
          "[ini][remote][password]") {
  TempSettings t;
  EditService svc(t.file, 5, make_verifier("hunter2"));
  Client bad("wrong"), good("hunter2");
  for (int i = 0; i < 5; ++i) {
    auto h = bad.hello(svc);
    CHECK(svc.handle(bad.build(h, "get", {}).request, "", true)[1] == "auth");
  }
  // Even the right password is refused now.
  auto h = good.hello(svc);
  CHECK(svc.handle(good.build(h, "get", {}).request, "", true)[1] == "auth");
}

TEST_CASE("EditService password: at most 256 outstanding nonces",
          "[ini][remote][password]") {
  TempSettings t;
  EditService svc(t.file, 5, make_verifier("hunter2"));
  Client c("hunter2");
  auto first = c.hello(svc);
  auto first_request = c.build(first, "get", {}).request;
  for (int i = 0; i < 256; ++i)
    (void)c.hello(svc);
  // The oldest nonce was dropped to make room: its proof no longer works.
  CHECK(svc.handle(first_request, "", true)[1] == "auth");
  auto last = c.hello(svc);
  CHECK(svc.handle(c.build(last, "get", {}).request, "", true)[1] == "ok");
}

TEST_CASE("EditService password: listeners that do not require it",
          "[ini][remote][password]") {
  TempSettings t;
  EditService with_pw(t.file, 5, make_verifier("hunter2"));
  // ipc / CURVE-only listener: no password, even though the service has one.
  CHECK(with_pw.handle(req({"hello"}), "ipc", false).at(2) ==
        R"({"auth":"none"})");
  CHECK(with_pw.handle(req({"get"}), "ipc", false)[1] == "ok");
  auto info = nlohmann::json::parse(with_pw.handle(req({"info"}))[2]);
  CHECK(info["auth"] == "none");

  // A password listener on a service without a password refuses everything.
  EditService without(t.file);
  CHECK(without.handle(req({"hello"}), "", true)[1] == "error");
  CHECK(without.handle(req({"get"}), "", true)[1] == "auth");

  // info reports the listener's mode.
  Client c("hunter2");
  auto h = c.hello(with_pw);
  auto b = c.build(h, "info", {});
  auto r = with_pw.handle(b.request, "", true);
  REQUIRE(r[1] == "ok");
  CHECK(nlohmann::json::parse(r[2])["auth"] == "password");
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
            {Listener{endpoint,
                      ServerKeys{broker.public_key, broker.secret_key}}},
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
  RemoteSource source(server.endpoint, server.keys(server.editor), std::nullopt,
                      3000ms);

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
  RemoteSource source(server.endpoint, server.keys(server.intruder),
                      std::nullopt, 500ms);
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
    dir /= "mads_ir_" +
           std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count() %
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

  explicit IpcServer(bool with_zap, bool with_password = false) {
    if (with_password)
      service = EditService(settings.file, 10, make_verifier("hunter2"));
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
  RemoteSource source(server.endpoint, std::nullopt, std::nullopt, 3000ms);
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
  REQUIRE_THROWS_AS(
      serve(ctx2, {Listener{first.endpoint, std::nullopt}}, other, running),
      std::runtime_error);
  // The first one is unaffected, and its file is still there.
  RemoteSource source(first.endpoint, std::nullopt, std::nullopt, 3000ms);
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
  CHECK_THROWS_AS(serve(ctx,
                        {Listener{mads_test::loopback(44752), std::nullopt}},
                        svc, running),
                  std::invalid_argument);
}

TEST_CASE("RemoteSource validates endpoint and keys", "[ini][remote]") {
  ClientKeys keys{"a", "b", "c"};
  // Plaintext tcp is now constructible (development mode: password only).
  CHECK_NOTHROW(RemoteSource("tcp://127.0.0.1:44753", std::nullopt, "pw"));
  CHECK_THROWS_AS(RemoteSource("ipc:///tmp/x.ipc", keys),
                  std::invalid_argument);
  CHECK_THROWS_AS(RemoteSource("nonsense", std::nullopt),
                  std::invalid_argument);
  CHECK_NOTHROW(RemoteSource("tcp://127.0.0.1:44753", keys));
  CHECK_NOTHROW(RemoteSource("ipc:///tmp/x.ipc", std::nullopt));
}

// ------------------------------------------------------ passwords end to end

namespace {

// Serves a password-protected listener (plaintext tcp, or CURVE + password).
struct PasswordServer {
  zmq::context_t ctx;
  Mads::ZapAuth zap{ctx};
  Mads::CurveKeypair broker = Mads::generate_keypair();
  Mads::CurveKeypair editor = Mads::generate_keypair();
  TempSettings settings;
  EditService service{settings.file, 5, make_verifier("hunter2")};
  std::atomic<bool> running{true};
  std::thread thread;
  std::string endpoint;

  PasswordServer(int port, bool curve) : endpoint(mads_test::loopback(port)) {
    Listener l{endpoint, std::nullopt, true};
    if (curve) {
      zap.allow("127.0.0.1");
      zap.configure_domain_curve(kZapDomain, {editor.public_key});
      zap.start();
      l.curve = ServerKeys{broker.public_key, broker.secret_key};
    }
    thread = std::thread([this, l] { serve(ctx, {l}, service, running); });
    std::this_thread::sleep_for(100ms);
  }
  ~PasswordServer() {
    running.store(false);
    if (thread.joinable())
      thread.join();
  }
};

} // namespace

TEST_CASE("plaintext tcp with a password: load, save, AuthError",
          "[ini][remote][password]") {
  PasswordServer server(44754, false);
  {
    RemoteSource source(server.endpoint, std::nullopt, "hunter2", 3000ms);
    Loaded loaded = source.load();
    CHECK(loaded.text == "[broker]\nport = 1\n");
    SaveResult saved = source.save("[broker]\nport = 2\n", loaded.etag);
    REQUIRE(saved.status == SaveResult::Status::Ok);
    CHECK(server.settings.read() == "[broker]\nport = 2\n");
    CHECK(source.save("[broker]\nport = 3\n", loaded.etag).status ==
          SaveResult::Status::Conflict);
    CHECK(nlohmann::json::parse(source.info())["auth"] == "password");
  }
  {
    RemoteSource wrong(server.endpoint, std::nullopt, "nope", 3000ms);
    CHECK_THROWS_AS(wrong.load(), AuthError);
    CHECK(wrong.save("[x]\na = 1\n", "").status == SaveResult::Status::Error);
  }
  {
    RemoteSource none(server.endpoint, std::nullopt, std::nullopt, 3000ms);
    CHECK_THROWS_AS(none.load(), AuthError);
    try {
      none.load();
    } catch (const AuthError &e) {
      CHECK(std::string(e.what()) == "this broker requires a password");
    }
  }
  CHECK(server.settings.read() == "[broker]\nport = 2\n");
}

TEST_CASE("CURVE plus password end to end", "[ini][remote][password]") {
  PasswordServer server(44755, true);
  ClientKeys keys{server.editor.public_key, server.editor.secret_key,
                  server.broker.public_key};
  RemoteSource ok(server.endpoint, keys, "hunter2", 3000ms);
  CHECK(ok.load().text == "[broker]\nport = 1\n");
  RemoteSource nopw(server.endpoint, keys, std::nullopt, 3000ms);
  CHECK_THROWS_AS(nopw.load(), AuthError);
  RemoteSource wrong(server.endpoint, keys, "nope", 3000ms);
  CHECK_THROWS_AS(wrong.load(), AuthError);
}

TEST_CASE("ipc ignores the password even when the service has one",
          "[ini][remote][password]") {
  if (!have_ipc())
    SKIP("no ipc transport");
  IpcServer server(false, true);
  REQUIRE_FALSE(server.error);
  RemoteSource source(server.endpoint, std::nullopt, std::nullopt, 3000ms);
  CHECK(source.load().text == "[broker]\nport = 1\n");
  CHECK(nlohmann::json::parse(source.info())["auth"] == "none");
}

TEST_CASE("serve() validates password listeners", "[ini][remote][password]") {
  TempSettings t;
  zmq::context_t ctx;
  std::atomic<bool> running{true};
  EditService without(t.file);
  EditService with(t.file, 5, make_verifier("pw"));
  // require_password without a service password.
  CHECK_THROWS_AS(
      serve(ctx, {Listener{mads_test::loopback(44756), std::nullopt, true}},
            without, running),
      std::invalid_argument);
  // Plaintext tcp without require_password, even with a service password.
  CHECK_THROWS_AS(
      serve(ctx, {Listener{mads_test::loopback(44756), std::nullopt, false}},
            with, running),
      std::invalid_argument);
  // require_password never on ipc.
  CHECK_THROWS_AS(
      serve(ctx, {Listener{"ipc:///tmp/mads_ir_never.ipc", std::nullopt, true}},
            with, running),
      std::invalid_argument);
}

TEST_CASE("the client rejects an unsigned reply to an authenticated request",
          "[ini][remote][password]") {
  // A fake server that completes the hello correctly but answers the command
  // with an unsigned "ok", as a man in the middle would.
  const PasswordVerifier v = make_verifier("hunter2");
  zmq::context_t ctx;
  zmq::socket_t router(ctx, zmq::socket_type::router);
  router.set(zmq::sockopt::linger, 0);
  router.set(zmq::sockopt::rcvtimeo, 100);
  const std::string endpoint = mads_test::loopback(44757);
  router.bind(endpoint);
  std::atomic<bool> stop{false};
  std::thread fake([&] {
    while (!stop) {
      zmq::multipart_t m;
      if (!m.recv(router))
        continue;
      zmq::multipart_t out;
      out.addstr(m.at(0).to_string()); // routing id
      out.addstr("");
      out.addstr(LIB_VERSION);
      out.addstr("ok");
      if (m.at(3).to_string() == "hello") {
        std::string nonce(32, 'n');
        auto b64 = [](const std::string &raw) {
          std::string o(sodium_base64_encoded_len(
                            raw.size(), sodium_base64_VARIANT_ORIGINAL),
                        '\0');
          sodium_bin2base64(o.data(), o.size(),
                            reinterpret_cast<const unsigned char *>(raw.data()),
                            raw.size(), sodium_base64_VARIANT_ORIGINAL);
          o.resize(strlen(o.c_str()));
          return o;
        };
        nlohmann::json j = {{"auth", "password"},
                            {"salt", b64(v.salt)},
                            {"opslimit", v.opslimit},
                            {"memlimit", v.memlimit},
                            {"nonce", b64(nonce)}};
        out.addstr(j.dump());
      } else {
        out.addstr("etag");
        out.addstr("forged contents");
      }
      out.send(router);
    }
  });
  RemoteSource source(endpoint, std::nullopt, "hunter2", 3000ms);
  CHECK_THROWS_WITH(source.load(),
                    Catch::Matchers::ContainsSubstring("not authentic"));
  stop = true;
  fake.join();
}
