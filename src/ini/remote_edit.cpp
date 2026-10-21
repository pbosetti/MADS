/*
 ____                      _         _____    _ _ _
|  _ \ ___ _ __ ___   ___ | |_ ___  | ____|__| (_) |_
| |_) / _ \ '_ ` _ \ / _ \| __/ _ \ |  _| / _` | | __|
|  _ <  __/ | | | | | (_) | ||  __/ | |__| (_| | | |_
|_| \_\___|_| |_| |_|\___/ \__\___| |_____\__,_|_|\__|

Remote editing of the broker's settings file: see remote_edit.hpp for the
protocol and the security model.

Author(s): Paolo Bosetti
*/
#include "remote_edit.hpp"

#include "detail/broker_ipc.hpp"
#include "mads.hpp"

#include <nlohmann/json.hpp>
#include <sodium.h>
#include <toml++/toml.hpp>
#include <zmq.hpp>
#include <zmq_addon.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Mads::Ini::Remote {

namespace {

namespace fs = std::filesystem;

// Room for the largest file plus the framing of a request or reply. Used as
// ZMQ_MAXMSGSIZE so that libzmq drops a connection announcing more, instead of
// buffering it: a client cannot make the broker allocate gigabytes.
constexpr std::size_t kMaxMessageSize = kMaxFileSize + 64 * 1024;
// A well-formed request has at most 4 frames; anything longer is refused
// before it is even interpreted.
constexpr std::size_t kMaxRequestFrames = 8;

bool read_file(const fs::path &path, std::string &out) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return !in.bad();
}

std::vector<std::string> error_reply(const std::string &message) {
  return {LIB_VERSION, "error", message};
}

std::string utc_timestamp(std::time_t t, const char *format) {
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  const std::size_t n = std::strftime(buf, sizeof(buf), format, &tm);
  return std::string(buf, n);
}

// A backup of `base` is named `<base>.<YYYYmmdd-HHMMSS>.bak`, or
// `<base>.<YYYYmmdd-HHMMSS>-<n>.bak` when that name is taken (several puts in
// one second). Anything else next to the file (FileSource's plain `.bak`, the
// user's own copies) is left alone.
struct Backup {
  std::string stamp; // YYYYmmdd-HHMMSS, sorts chronologically
  unsigned counter = 0;
  fs::path path;
};

std::vector<Backup> list_backups(const fs::path &base) {
  static const std::regex pattern(R"(^(\d{8}-\d{6})(?:-(\d+))?\.bak$)");
  std::vector<Backup> found;
  const std::string prefix = base.filename().string() + ".";
  std::error_code ec;
  for (fs::directory_iterator it(
           base.parent_path().empty() ? fs::path(".") : base.parent_path(), ec),
       end;
       !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix))
      continue;
    std::smatch m;
    const std::string rest = name.substr(prefix.size());
    if (!std::regex_match(rest, m, pattern))
      continue;
    Backup b;
    b.stamp = m[1].str();
    if (m[2].matched)
      b.counter = static_cast<unsigned>(std::stoul(m[2].str()));
    b.path = it->path();
    found.push_back(std::move(b));
  }
  std::sort(found.begin(), found.end(), [](const Backup &a, const Backup &b) {
    return std::tie(a.stamp, a.counter) < std::tie(b.stamp, b.counter);
  });
  return found;
}

// Copies `path` to a fresh backup name; returns an empty string on success,
// else the reason.
std::string make_backup(const fs::path &path) {
  const std::string stamp = utc_timestamp(std::time(nullptr), "%Y%m%d-%H%M%S");
  std::error_code ec;
  for (unsigned n = 0; n < 1000; ++n) {
    fs::path bak = path;
    bak += "." + stamp + (n ? "-" + std::to_string(n) : "") + ".bak";
    if (fs::exists(bak, ec))
      continue;
    fs::copy_file(path, bak, fs::copy_options::none, ec);
    return ec ? "cannot write backup " + bak.string() + ": " + ec.message()
              : std::string();
  }
  return "too many backups with the same timestamp";
}

void prune_backups(const fs::path &path, std::size_t keep) {
  auto backups = list_backups(path);
  if (backups.size() <= keep)
    return;
  std::error_code ec;
  for (std::size_t i = 0; i + keep < backups.size(); ++i)
    fs::remove(backups[i].path, ec); // best effort
}

// ------------------------------------------------------------ crypto helpers

constexpr const char *kAuthTag = "mads-edit-v1";
constexpr std::size_t kSaltBytes = 16;
constexpr std::size_t kKeyBytes = 32;
constexpr std::size_t kNonceBytes = 32;
constexpr std::size_t kMaxNonces = 256;
constexpr auto kNonceLifetime = std::chrono::seconds(60);
constexpr int kMaxFailures = 5;
constexpr auto kLockout = std::chrono::seconds(30);
constexpr std::size_t kMaxMemlimit = std::size_t(1) << 30; // 1 GiB, client cap

static_assert(crypto_auth_hmacsha256_KEYBYTES == kKeyBytes);
static_assert(crypto_hash_sha256_BYTES == kKeyBytes);

void ensure_sodium() {
  // Idempotent; returns 1 when already initialised.
  if (sodium_init() < 0)
    throw std::runtime_error("cannot initialise libsodium");
}

void wipe(std::string &s) {
  if (!s.empty())
    sodium_memzero(s.data(), s.size());
}

std::string random_bytes(std::size_t n) {
  ensure_sodium();
  std::string out(n, '\0');
  randombytes_buf(out.data(), n);
  return out;
}

bool equal_ct(const std::string &a, const std::string &b) {
  return a.size() == b.size() &&
         sodium_memcmp(a.data(), b.data(), a.size()) == 0;
}

std::string hmac(const std::string &key, const std::string &message) {
  std::string out(crypto_auth_hmacsha256_BYTES, '\0');
  crypto_auth_hmacsha256(
      reinterpret_cast<unsigned char *>(out.data()),
      reinterpret_cast<const unsigned char *>(message.data()), message.size(),
      reinterpret_cast<const unsigned char *>(key.data()));
  return out;
}

std::string sha256(const std::string &data) {
  std::string out(crypto_hash_sha256_BYTES, '\0');
  crypto_hash_sha256(reinterpret_cast<unsigned char *>(out.data()),
                     reinterpret_cast<const unsigned char *>(data.data()),
                     data.size());
  return out;
}

// SHA-256(L(f_1) || ... || L(f_n)), L(x) = 8-byte big-endian length || x.
std::string frames_hash(const std::vector<std::string> &frames,
                        std::size_t first = 0) {
  crypto_hash_sha256_state state;
  crypto_hash_sha256_init(&state);
  for (std::size_t i = first; i < frames.size(); ++i) {
    unsigned char len[8];
    std::uint64_t n = frames[i].size();
    for (int b = 7; b >= 0; --b, n >>= 8)
      len[b] = static_cast<unsigned char>(n & 0xff);
    crypto_hash_sha256_update(&state, len, sizeof(len));
    crypto_hash_sha256_update(
        &state, reinterpret_cast<const unsigned char *>(frames[i].data()),
        frames[i].size());
  }
  std::string out(crypto_hash_sha256_BYTES, '\0');
  crypto_hash_sha256_final(&state,
                           reinterpret_cast<unsigned char *>(out.data()));
  return out;
}

std::string auth_message(const std::string &server_nonce,
                         const std::string &client_nonce,
                         const std::string &command,
                         const std::vector<std::string> &args) {
  std::string m = kAuthTag;
  m += server_nonce;
  m += client_nonce;
  m += command;
  m += '\0';
  m += frames_hash(args);
  return m;
}

std::string xor_strings(const std::string &a, const std::string &b) {
  std::string out(a.size(), '\0');
  for (std::size_t i = 0; i < a.size(); ++i)
    out[i] = static_cast<char>(a[i] ^ b[i]);
  return out;
}

std::string b64_encode(const std::string &raw) {
  ensure_sodium();
  std::string out(
      sodium_base64_encoded_len(raw.size(), sodium_base64_VARIANT_ORIGINAL),
      '\0');
  sodium_bin2base64(out.data(), out.size(),
                    reinterpret_cast<const unsigned char *>(raw.data()),
                    raw.size(), sodium_base64_VARIANT_ORIGINAL);
  out.resize(std::strlen(out.c_str())); // drop the NUL
  return out;
}

// Decodes standard base64 into exactly `expected` bytes; nullopt otherwise.
std::optional<std::string> b64_decode(const std::string &text,
                                      std::size_t expected) {
  ensure_sodium();
  std::string out(expected, '\0');
  std::size_t len = 0;
  const char *end = nullptr;
  if (sodium_base642bin(reinterpret_cast<unsigned char *>(out.data()),
                        out.size(), text.data(), text.size(), nullptr, &len,
                        &end, sodium_base64_VARIANT_ORIGINAL) != 0 ||
      len != expected || end != text.data() + text.size())
    return std::nullopt;
  return out;
}

bool kdf_params_valid(unsigned long long opslimit, std::size_t memlimit) {
  return opslimit >= crypto_pwhash_OPSLIMIT_MIN &&
         opslimit <= crypto_pwhash_OPSLIMIT_MAX &&
         memlimit >= crypto_pwhash_MEMLIMIT_MIN &&
         memlimit <= crypto_pwhash_MEMLIMIT_MAX;
}

} // namespace

// ------------------------------------------------------- password verifier

namespace {

// SaltedPassword = Argon2id13(password, salt); wipes nothing itself.
std::string salted_password(std::string_view password, const std::string &salt,
                            unsigned long long opslimit, std::size_t memlimit) {
  ensure_sodium();
  std::string out(kKeyBytes, '\0');
  if (crypto_pwhash(reinterpret_cast<unsigned char *>(out.data()), out.size(),
                    password.data(), password.size(),
                    reinterpret_cast<const unsigned char *>(salt.data()),
                    opslimit, memlimit, crypto_pwhash_ALG_ARGON2ID13) != 0)
    throw std::runtime_error("password key derivation failed (out of memory?)");
  return out;
}

constexpr const char *kVerifierPrefix = "mads-edit-password v1 argon2id13";

} // namespace

PasswordVerifier PasswordVerifier::create(std::string_view password) {
  PasswordVerifier v;
  v.opslimit = crypto_pwhash_OPSLIMIT_MODERATE;
  v.memlimit = crypto_pwhash_MEMLIMIT_MODERATE;
  v.salt = random_bytes(kSaltBytes);
  std::string salted =
      salted_password(password, v.salt, v.opslimit, v.memlimit);
  std::string client_key = hmac(salted, "Client Key");
  v.stored_key = sha256(client_key);
  v.server_key = hmac(salted, "Server Key");
  wipe(salted);
  wipe(client_key);
  return v;
}

std::string PasswordVerifier::to_string() const {
  return std::string(kVerifierPrefix) + " " + std::to_string(opslimit) + " " +
         std::to_string(memlimit) + " " + b64_encode(salt) + " " +
         b64_encode(stored_key) + " " + b64_encode(server_key);
}

std::optional<PasswordVerifier> PasswordVerifier::parse(std::string_view line,
                                                        std::string *error) {
  auto bad = [&](const std::string &why) -> std::optional<PasswordVerifier> {
    if (error)
      *error = why;
    return std::nullopt;
  };
  std::istringstream in{std::string(line)};
  std::vector<std::string> tok;
  for (std::string t; in >> t;)
    tok.push_back(t);
  if (tok.size() != 8 || tok[0] != "mads-edit-password" || tok[1] != "v1" ||
      tok[2] != "argon2id13")
    return bad("not a mads-edit-password v1 argon2id13 line");
  auto number = [](const std::string &t) -> std::optional<unsigned long long> {
    if (t.empty() || t.size() > 20 ||
        t.find_first_not_of("0123456789") != std::string::npos)
      return std::nullopt;
    errno = 0;
    const unsigned long long v = std::strtoull(t.c_str(), nullptr, 10);
    if (errno)
      return std::nullopt;
    return v;
  };
  const auto ops = number(tok[3]);
  const auto mem = number(tok[4]);
  if (!ops || !mem || *mem > std::numeric_limits<std::size_t>::max() ||
      !kdf_params_valid(*ops, static_cast<std::size_t>(*mem)))
    return bad("key derivation parameters out of range");
  PasswordVerifier v;
  v.opslimit = *ops;
  v.memlimit = static_cast<std::size_t>(*mem);
  auto salt = b64_decode(tok[5], kSaltBytes);
  auto stored = b64_decode(tok[6], kKeyBytes);
  auto server = b64_decode(tok[7], kKeyBytes);
  if (!salt || !stored || !server)
    return bad("malformed salt or key");
  v.salt = *salt;
  v.stored_key = *stored;
  v.server_key = *server;
  return v;
}

constexpr const char *kSecretPrefix = "mads-edit-secret v1 argon2id13";

std::string ClientSecret::to_string() const {
  return std::string(kSecretPrefix) + " " + std::to_string(opslimit) + " " +
         std::to_string(memlimit) + " " + b64_encode(salt) + " " +
         b64_encode(client_key) + " " + b64_encode(server_key);
}

std::optional<ClientSecret> ClientSecret::parse(std::string_view line) {
  std::istringstream in{std::string(line)};
  std::vector<std::string> tok;
  for (std::string t; in >> t;)
    tok.push_back(t);
  if (tok.size() != 8 || tok[0] != "mads-edit-secret" || tok[1] != "v1" ||
      tok[2] != "argon2id13")
    return std::nullopt;
  for (const auto &n : {tok[3], tok[4]})
    if (n.empty() || n.size() > 20 ||
        n.find_first_not_of("0123456789") != std::string::npos)
      return std::nullopt;
  errno = 0;
  const unsigned long long ops = std::strtoull(tok[3].c_str(), nullptr, 10);
  const unsigned long long mem = std::strtoull(tok[4].c_str(), nullptr, 10);
  if (errno || mem > std::numeric_limits<std::size_t>::max() ||
      !kdf_params_valid(ops, static_cast<std::size_t>(mem)))
    return std::nullopt;
  auto salt = b64_decode(tok[5], kSaltBytes);
  auto client = b64_decode(tok[6], kKeyBytes);
  auto server = b64_decode(tok[7], kKeyBytes);
  if (!salt || !client || !server)
    return std::nullopt;
  ClientSecret secret;
  secret.salt = *salt;
  secret.opslimit = ops;
  secret.memlimit = static_cast<std::size_t>(mem);
  secret.client_key = *client;
  secret.server_key = *server;
  return secret;
}

fs::path password_file_for(const fs::path &settings) {
  fs::path p = settings;
  p += ".edit_password";
  return p;
}

bool write_password_file(const fs::path &file, const PasswordVerifier &verifier,
                         std::string *error) {
  auto fail = [&](const std::string &why) {
    if (error)
      *error = why;
    return false;
  };
  const std::string content = verifier.to_string() + "\n";
  fs::path tmp = file;
  tmp += ".tmp";
  std::error_code ec;
  fs::remove(tmp, ec);
#ifndef _WIN32
  // Created with 0600 from the start: the verifier is never visible to
  // others, not even briefly.
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0)
    return fail("cannot create " + tmp.string() + ": " + std::strerror(errno));
  const char *p = content.data();
  std::size_t left = content.size();
  while (left > 0) {
    const ssize_t n = ::write(fd, p, left);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      ::close(fd);
      fs::remove(tmp, ec);
      return fail("cannot write " + tmp.string());
    }
    p += n;
    left -= static_cast<std::size_t>(n);
  }
  ::fsync(fd);
  if (::close(fd) != 0) {
    fs::remove(tmp, ec);
    return fail("cannot write " + tmp.string());
  }
#else
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << content;
    out.close();
    if (!out) {
      fs::remove(tmp, ec);
      return fail("cannot write " + tmp.string());
    }
  }
#endif
  fs::rename(tmp, file, ec);
  if (ec) {
    fs::remove(file, ec);
    fs::rename(tmp, file, ec);
  }
  if (ec) {
    std::error_code ignored;
    fs::remove(tmp, ignored);
    return fail("cannot replace " + file.string() + ": " + ec.message());
  }
  return true;
}

std::optional<PasswordVerifier>
read_password_file(const fs::path &file, std::vector<std::string> *warnings) {
  std::error_code ec;
  if (!fs::exists(file, ec))
    return std::nullopt;
  std::ifstream in(file);
  if (!in)
    throw std::runtime_error("cannot read " + file.string());
  std::string line;
  std::getline(in, line);
  std::string why;
  auto v = PasswordVerifier::parse(line, &why);
  if (!v)
    throw std::runtime_error("malformed password file " + file.string() + ": " +
                             why);
#ifndef _WIN32
  struct stat st{};
  if (warnings && ::stat(file.c_str(), &st) == 0 && (st.st_mode & 077))
    warnings->push_back(file.string() +
                        " is accessible by group or others; chmod 600 it");
#else
  (void)warnings;
#endif
  return v;
}

ClientSecret ClientSecret::derive(std::string_view password, std::string salt,
                                  unsigned long long opslimit,
                                  std::size_t memlimit) {
  if (salt.size() != kSaltBytes)
    throw std::invalid_argument("invalid salt size");
  if (!kdf_params_valid(opslimit, memlimit))
    throw std::invalid_argument("key derivation parameters out of range");
  // The hello is not authenticated: do not let it make us burn unbounded
  // memory or time.
  if (memlimit > kMaxMemlimit || opslimit > crypto_pwhash_OPSLIMIT_SENSITIVE)
    throw std::invalid_argument(
        "key derivation parameters too expensive (refused)");
  ClientSecret secret;
  secret.salt = std::move(salt);
  secret.opslimit = opslimit;
  secret.memlimit = memlimit;
  std::string salted =
      salted_password(password, secret.salt, opslimit, memlimit);
  secret.client_key = hmac(salted, "Client Key");
  secret.server_key = hmac(salted, "Server Key");
  wipe(salted);
  return secret;
}

// ------------------------------------------------------------- EditService

EditService::EditService(fs::path settings_path, std::size_t max_backups,
                         std::optional<PasswordVerifier> password)
    : _path(std::move(settings_path)),
      // At least one: a put must never be able to destroy the only copy.
      _max_backups(std::max<std::size_t>(max_backups, 1)),
      _password(std::move(password)) {}

std::vector<std::string>
EditService::handle(const std::vector<std::string> &request,
                    const std::string &user, bool require_password) {
  // A Z85 key is shortened to its first 8 characters; names like "ipc" stay.
  std::string who = user.empty()
                        ? std::string(require_password ? "" : "unknown")
                    : user.size() > 8 ? user.substr(0, 8) + "\xE2\x80\xA6"
                                      : user;
  if (require_password)
    who += who.empty() ? "password" : "+password";
  auto note = [&](const std::string &line) {
    if (!log)
      return;
    try {
      log(line);
    } catch (...) {
    }
  };
  // Records an error in the log and builds the reply.
  auto fail = [&](const std::string &command, const std::string &message) {
    note(command + " by " + who + ": error, " + message);
    return error_reply(message);
  };

  std::string command = "request";
  try {
    if (request.size() < 3)
      return fail(command, "malformed request");
    try {
      if (!Mads::check_version(request[0]))
        return fail(command, "incompatible version " + request[0] +
                                 " (server is " + LIB_VERSION + ")");
    } catch (const std::exception &err) {
      return fail(command, err.what());
    }
    command = request[1];
    const std::string &auth = request[2];
    const std::vector<std::string> args(request.begin() + 3, request.end());
    const auto now = std::chrono::steady_clock::now();

    if (command == "hello") {
      nlohmann::json j;
      if (!require_password) {
        j["auth"] = "none";
      } else if (!_password) {
        return fail(command, "password authentication is not configured");
      } else {
        ensure_sodium();
        // Drop expired nonces, then make room: oldest first.
        _nonces.erase(
            std::remove_if(_nonces.begin(), _nonces.end(),
                           [&](const Nonce &n) { return n.expires <= now; }),
            _nonces.end());
        while (_nonces.size() >= kMaxNonces)
          _nonces.erase(_nonces.begin());
        Nonce n{random_bytes(kNonceBytes), now + kNonceLifetime};
        j["auth"] = "password";
        j["salt"] = b64_encode(_password->salt);
        j["opslimit"] = _password->opslimit;
        j["memlimit"] = _password->memlimit;
        j["nonce"] = b64_encode(n.value);
        _nonces.push_back(std::move(n));
      }
      return {LIB_VERSION, "ok", j.dump()};
    }

    // Authentication of get/put/info on a password listener. On success
    // `server_key` and `signed_message` make the reply signature.
    std::string server_key, signed_message;
    const bool is_data_command =
        command == "get" || command == "put" || command == "info";
    if (is_data_command && require_password) {
      auto refuse = [&](const std::string &reason) {
        note("auth failure by " + who + " (" + command + "): " + reason);
        return std::vector<std::string>{LIB_VERSION, "auth",
                                        "authentication failed"};
      };
      if (!_password)
        return refuse("no password configured");
      if (auth.size() != 2 * kNonceBytes + kKeyBytes)
        return refuse("missing or malformed proof"); // not counted as a guess

      _nonces.erase(
          std::remove_if(_nonces.begin(), _nonces.end(),
                         [&](const Nonce &n) { return n.expires <= now; }),
          _nonces.end());
      const std::string server_nonce = auth.substr(0, kNonceBytes);
      const std::string client_nonce = auth.substr(kNonceBytes, kNonceBytes);
      const std::string proof = auth.substr(2 * kNonceBytes);
      // The nonce is looked up and consumed whatever happens next, so a
      // nonce can be tried once and never again.
      const auto it =
          std::find_if(_nonces.begin(), _nonces.end(), [&](const Nonce &n) {
            return equal_ct(n.value, server_nonce);
          });
      const bool known = it != _nonces.end();
      if (known)
        _nonces.erase(it);
      if (now < _locked_until)
        return refuse("locked out");
      if (!known)
        return refuse("unknown, replayed or expired nonce"); // not a guess

      const std::string msg =
          auth_message(server_nonce, client_nonce, command, args);
      std::string client_key =
          xor_strings(proof, hmac(_password->stored_key, msg));
      const bool granted = equal_ct(sha256(client_key), _password->stored_key);
      wipe(client_key);
      if (!granted) {
        if (++_failures >= kMaxFailures) {
          _failures = 0;
          _locked_until = now + kLockout;
          note("auth lockout for 30 s after " + std::to_string(kMaxFailures) +
               " failed proofs");
        }
        return refuse("bad proof");
      }
      signed_message = msg;
      _failures = 0;
      server_key = _password->server_key;
    }

    // Runs the command; a signature is appended afterwards if authenticated.
    auto execute = [&]() -> std::vector<std::string> {
      if (command == "get") {
        if (!args.empty())
          return fail(command, "malformed get");
        std::string text;
        if (!read_file(_path, text))
          return fail(command, "cannot read the settings file");
        return {LIB_VERSION, "ok", content_etag(text), text};
      }

      if (command == "info") {
        if (!args.empty())
          return fail(command, "malformed info");
        std::string text;
        std::error_code ec;
        if (!read_file(_path, text))
          return fail(command, "cannot read the settings file");
        // file_clock has no portable conversion to system_clock before C++20
        // library support is universal, so shift by the difference of "now".
        const auto ftime = fs::last_write_time(_path, ec);
        std::string mtime;
        if (!ec) {
          const auto shifted =
              std::chrono::system_clock::now() +
              std::chrono::duration_cast<std::chrono::system_clock::duration>(
                  ftime - fs::file_time_type::clock::now());
          mtime = utc_timestamp(std::chrono::system_clock::to_time_t(shifted),
                                "%Y-%m-%dT%H:%M:%SZ");
        }
        nlohmann::json j;
        j["path"] = _path.string();
        j["size"] = text.size();
        j["mtime"] = mtime;
        j["etag"] = content_etag(text);
        j["version"] = LIB_VERSION;
        j["auth"] = require_password ? "password" : "none";
        return {LIB_VERSION, "ok", j.dump()};
      }

      if (command == "put") {
        if (args.size() != 2)
          return fail(command, "malformed put");
        const std::string &base_etag = args[0];
        const std::string &text = args[1];
        if (text.size() > kMaxFileSize)
          return fail(command, "file too large (" +
                                   std::to_string(text.size()) +
                                   " bytes, limit " +
                                   std::to_string(kMaxFileSize) + ")");
        try {
          (void)toml::parse(text);
        } catch (const toml::parse_error &err) {
          return fail(command,
                      "not valid TOML: " + std::string(err.description()) +
                          " (line " + std::to_string(err.source().begin.line) +
                          ")");
        }

        std::error_code ec;
        const bool exists = fs::exists(_path, ec);
        if (exists && !base_etag.empty()) {
          std::string current;
          if (!read_file(_path, current))
            return fail(command, "cannot read the settings file");
          if (content_etag(current) != base_etag) {
            note("put by " + who + ": conflict");
            return {LIB_VERSION, "conflict", content_etag(current), current};
          }
        }

        fs::path tmp = _path;
        tmp += ".tmp";
        {
          std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
          out.write(text.data(), static_cast<std::streamsize>(text.size()));
          out.close();
          if (!out) {
            fs::remove(tmp, ec);
            return fail(command, "cannot write the temporary file");
          }
        }
        if (exists) {
          // The new file keeps the permissions of the one it replaces.
          const auto status = fs::status(_path, ec);
          if (!ec)
            fs::permissions(tmp, status.permissions(), ec);
          const std::string backup_error = make_backup(_path);
          if (!backup_error.empty()) {
            fs::remove(tmp, ec);
            return fail(command, backup_error);
          }
          prune_backups(_path, _max_backups);
        }
        fs::rename(tmp, _path, ec);
        if (ec && exists) {
          // Some platforms refuse to rename over an existing file.
          fs::remove(_path, ec);
          fs::rename(tmp, _path, ec);
        }
        if (ec) {
          std::error_code ignored;
          fs::remove(tmp, ignored);
          return fail(command,
                      "cannot replace the settings file: " + ec.message());
        }
        note("put by " + who + ": ok, " + std::to_string(text.size()) +
             " bytes");
        return {LIB_VERSION, "ok", content_etag(text)};
      }

      return fail(command, "unknown command");
    };

    std::vector<std::string> reply = execute();
    if (!signed_message.empty()) {
      // Signature over the other frames, so the client can tell the reply
      // came from the holder of the verifier and was not altered.
      reply.push_back(hmac(server_key, signed_message + frames_hash(reply)));
    }
    return reply;
  } catch (const std::exception &err) {
    return fail(command, err.what());
  } catch (...) {
    return error_reply("internal error");
  }
}

// ------------------------------------------------------------------- serve

namespace {

// One bound listener: its ROUTER socket and, for ipc, the socket file this
// process created (removed on destruction, only if still ours).
struct BoundListener {
  std::unique_ptr<zmq::socket_t> socket;
  bool curve = false;
  bool require_password = false;
  Mads::detail::IpcSocketFile file;
};

void configure_listener(zmq::socket_t &socket, const Listener &listener) {
  socket.set(zmq::sockopt::linger, 0);
  socket.set(zmq::sockopt::sndtimeo, 1000);
  socket.set(zmq::sockopt::maxmsgsize, static_cast<int64_t>(kMaxMessageSize));
  if (listener.curve) {
    socket.set(zmq::sockopt::curve_server, 1);
    socket.set(zmq::sockopt::curve_secretkey, listener.curve->secret_key);
    // Must precede bind(): the domain selects the editor-only allowlist.
    socket.set(zmq::sockopt::zap_domain, kZapDomain);
  }
}

// Reads one pending multipart message, and answers it unless it is malformed
// (then it is dropped silently).
void serve_one(zmq::socket_t &socket, bool curve, bool require_password,
               EditService &service,
               const std::function<void(const std::string &)> &note) {
  std::vector<zmq::message_t> frames;
  bool more = true, oversized = false;
  while (more) {
    zmq::message_t frame;
    if (!socket.recv(frame, zmq::recv_flags::none))
      return;
    more = frame.more();
    if (frames.size() <= kMaxRequestFrames + 1)
      frames.push_back(std::move(frame));
    else
      oversized = true; // keep draining the message, bounded memory
  }
  if (frames.empty())
    return;

  // Envelope: routing id(s) up to and including the empty delimiter that a
  // REQ client adds.
  std::size_t delimiter = 0;
  while (delimiter < frames.size() && frames[delimiter].size() != 0)
    ++delimiter;
  if (delimiter == 0 || delimiter >= frames.size()) {
    note("request without a REQ envelope dropped");
    return;
  }

  std::string user;
  if (curve) {
    // ZAP's User-Id: the Z85 key of the client, set only for requests the
    // edit-domain allowlist granted. Read from the first content frame; if
    // absent the request is refused (fail closed) even though the handshake
    // succeeded.
    if (delimiter + 1 < frames.size()) {
      try {
        if (const char *uid = frames[delimiter + 1].gets("User-Id"))
          user = uid;
      } catch (const zmq::error_t &) {
      }
    }
  } else if (!require_password) {
    user = "ipc"; // plaintext ipc: the owner-only socket directory is the gate
  } else {
    // Plaintext tcp, development mode: only the password identifies the
    // client, and handle() enforces it.
    user = "-";
  }

  std::vector<std::string> reply;
  if (user.empty()) {
    note("request without an authenticated key refused");
    reply = error_reply("not authorized");
  } else if (oversized) {
    reply = error_reply("malformed request");
  } else {
    std::vector<std::string> request;
    for (std::size_t i = delimiter + 1; i < frames.size(); ++i)
      request.emplace_back(frames[i].data<char>(), frames[i].size());
    reply = service.handle(request, user == "-" ? std::string() : user,
                           require_password);
  }

  zmq::multipart_t out;
  for (std::size_t i = 0; i <= delimiter; ++i)
    out.add(std::move(frames[i]));
  for (const auto &part : reply)
    out.addstr(part);
  out.send(socket);
}

} // namespace

void serve(zmq::context_t &context, const std::vector<Listener> &listeners,
           EditService &service, const std::atomic<bool> &running) {
  using Mads::detail::IpcSocketState;

  // Validate everything before binding anything.
  std::vector<std::optional<Mads::detail::Endpoint>> parsed;
  for (const auto &l : listeners) {
    auto e = Mads::detail::parse_endpoint(l.endpoint);
    if (!e)
      throw std::invalid_argument("invalid endpoint " + l.endpoint);
    if (l.require_password && e->is_ipc())
      throw std::invalid_argument("a password cannot be required on " +
                                  l.endpoint +
                                  " (ipc:// is never password-protected)");
    if (l.require_password && !service.has_password())
      throw std::invalid_argument("a password is required on " + l.endpoint +
                                  " but the service has none");
    if (!l.curve && !e->is_ipc() && !(e->is_tcp() && l.require_password))
      throw std::invalid_argument(
          "refusing a plaintext edit listener on " + l.endpoint +
          " (only ipc:// or a password-protected tcp:// may go without CURVE)");
    if (e->is_ipc() && !Mads::detail::zmq_has_ipc())
      throw std::runtime_error("this libzmq build has no ipc:// transport: " +
                               l.endpoint);
    parsed.push_back(std::move(e));
  }

  // Destroyed in reverse order on return or throw: sockets close before the
  // files they created are unlinked.
  struct Guard {
    std::vector<BoundListener> bound;
    ~Guard() {
      for (auto it = bound.rbegin(); it != bound.rend(); ++it) {
        it->socket.reset();
        it->file.remove();
      }
    }
  } guard;

  for (std::size_t i = 0; i < listeners.size(); ++i) {
    const auto &l = listeners[i];
    BoundListener b;
    b.curve = l.curve.has_value();
    b.require_password = l.require_password;
    b.socket =
        std::make_unique<zmq::socket_t>(context, zmq::socket_type::router);
    configure_listener(*b.socket, l);
    if (parsed[i]->is_ipc()) {
      // libzmq unlinks the path before binding: never let that steal the
      // socket of a broker that is still listening on it.
      switch (Mads::detail::probe_ipc_socket(parsed[i]->path)) {
      case IpcSocketState::Live:
        throw std::runtime_error(parsed[i]->path +
                                 " is in use by another process");
      case IpcSocketState::Foreign:
        throw std::runtime_error(parsed[i]->path +
                                 " exists and is not a socket");
      default:
        break;
      }
      b.socket->bind(l.endpoint);
      b.file.claim(parsed[i]->path);
    } else {
      b.socket->bind(l.endpoint);
    }
    guard.bound.push_back(std::move(b));
  }

  auto note = [&](const std::string &line) {
    if (!service.log)
      return;
    try {
      service.log(line);
    } catch (...) {
    }
  };

  std::vector<zmq::pollitem_t> items;
  for (auto &b : guard.bound)
    items.push_back({b.socket->handle(), 0, ZMQ_POLLIN, 0});

  while (running.load()) {
    try {
      if (items.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        continue;
      }
      zmq::poll(items, std::chrono::milliseconds(200));
      for (std::size_t i = 0; i < items.size(); ++i) {
        if (!(items[i].revents & ZMQ_POLLIN))
          continue;
        auto &b = guard.bound[i];
        // One message per readiness event is enough: poll fires again if
        // more are queued, and every socket gets its turn.
        try {
          serve_one(*b.socket, b.curve, b.require_password, service, note);
        } catch (const zmq::error_t &err) {
          if (err.num() == ETERM)
            return;
          note(std::string("edit service: ") + err.what());
        } catch (const std::exception &err) {
          note(std::string("edit service: ") + err.what());
        }
      }
    } catch (const zmq::error_t &err) {
      if (err.num() == ETERM)
        return;
      note(std::string("edit service: ") + err.what());
    }
  }
}

std::string local_endpoint(int port, const fs::path &ipc_dir) {
  if (!Mads::detail::zmq_has_ipc())
    throw std::runtime_error(
        "this libzmq build has no ipc:// transport; use the tcp endpoint");
  fs::path dir;
  if (ipc_dir.empty()) {
    dir = Mads::detail::runtime_dir();
  } else {
    dir = ipc_dir;
    Mads::detail::ensure_private_dir(dir, "ipc_dir");
  }
  return Mads::detail::default_ipc_endpoint(dir, static_cast<unsigned>(port),
                                            "edit");
}

// -------------------------------------------------------------- client keys

namespace {

std::string read_key_line(const fs::path &file) {
  std::ifstream in(file);
  if (!in)
    throw std::runtime_error("cannot open key file " + file.string());
  std::string line;
  std::getline(in, line);
  while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
    line.pop_back();
  if (line.size() != 40)
    throw std::runtime_error("malformed key file " + file.string() +
                             " (expected 40 Z85 characters)");
  return line;
}

} // namespace

ClientKeys load_client_keys(const fs::path &dir, const std::string &client,
                            const std::string &server) {
  ClientKeys keys;
  keys.public_key = read_key_line(dir / (client + ".pub"));
  keys.secret_key = read_key_line(dir / (client + ".key"));
  keys.server_public_key = read_key_line(dir / (server + ".pub"));
  return keys;
}

ClientKeys load_client_key_files(const fs::path &client_secret_key_file,
                                 const fs::path &server_public_key_file) {
  // `mads --keypair NAME` writes NAME.key (secret) and NAME.pub (public).
  fs::path pub = client_secret_key_file;
  pub.replace_extension(".pub");
  ClientKeys keys;
  keys.secret_key = read_key_line(client_secret_key_file);
  keys.public_key = read_key_line(pub);
  keys.server_public_key = read_key_line(server_public_key_file);
  return keys;
}

// ------------------------------------------------------------ RemoteSource

RemoteSource::RemoteSource(std::string endpoint, std::optional<ClientKeys> keys,
                           std::optional<std::string> password,
                           std::chrono::milliseconds timeout)
    : _endpoint(std::move(endpoint)), _keys(std::move(keys)),
      _password(std::move(password)), _timeout(timeout), _context(nullptr) {
  const auto e = Mads::detail::parse_endpoint(_endpoint);
  if (!e)
    throw std::invalid_argument("invalid endpoint " + _endpoint);
  if (e->is_ipc() && _keys)
    throw std::invalid_argument("an ipc:// endpoint takes no CURVE keys");
  if (!e->is_ipc() && !e->is_tcp())
    throw std::invalid_argument("unsupported endpoint " + _endpoint);
  _context = new zmq::context_t(1);
}

RemoteSource::~RemoteSource() {
  if (_password)
    wipe(*_password);
  if (_secret) {
    wipe(_secret->client_key);
    wipe(_secret->server_key);
  }
  delete _context;
}

std::vector<std::string>
RemoteSource::request(const std::vector<std::string> &frames) {
  const std::string timeout_message =
      "no reply from " + _endpoint + " within " +
      std::to_string(_timeout.count()) +
      (_keys ? " ms (broker down, remote_edit disabled, or this editor's key "
               "is not authorized)"
             : " ms (broker not running on this host, or remote_edit "
               "disabled)");
  try {
    zmq::socket_t socket(*_context, zmq::socket_type::req);
    const int timeout = static_cast<int>(_timeout.count());
    socket.set(zmq::sockopt::linger, 0);
    socket.set(zmq::sockopt::rcvtimeo, timeout);
    socket.set(zmq::sockopt::sndtimeo, timeout);
    socket.set(zmq::sockopt::maxmsgsize, static_cast<int64_t>(kMaxMessageSize));
    if (_keys) {
      socket.set(zmq::sockopt::curve_serverkey, _keys->server_public_key);
      socket.set(zmq::sockopt::curve_publickey, _keys->public_key);
      socket.set(zmq::sockopt::curve_secretkey, _keys->secret_key);
    }
    socket.connect(_endpoint);

    auto exchange = [&](const std::vector<std::string> &out_frames) {
      zmq::multipart_t out;
      out.addstr(LIB_VERSION);
      for (const auto &frame : out_frames)
        out.addstr(frame);
      if (!out.send(socket))
        throw std::runtime_error(timeout_message);
      zmq::multipart_t in;
      if (!in.recv(socket) || in.size() < 2)
        throw std::runtime_error(timeout_message);
      std::vector<std::string> reply;
      for (std::size_t i = 0; i < in.size(); ++i)
        reply.push_back(in[i].to_string());
      return reply;
    };

    // Hello first: does the server want a password, and with which nonce?
    const auto hello = exchange({"hello", ""});
    if (hello[1] == "error")
      throw std::runtime_error(hello.size() > 2 ? hello[2] : "remote error");
    if (hello[1] != "ok" || hello.size() != 3)
      throw std::runtime_error("unexpected hello reply from the edit service");
    nlohmann::json h = nlohmann::json::parse(hello[2], nullptr, false);
    if (!h.is_object() || !h.contains("auth") || !h["auth"].is_string())
      throw std::runtime_error("malformed hello reply from the edit service");
    const std::string auth_mode = h["auth"];

    std::vector<std::string> request_frames{frames[0], ""};
    request_frames.insert(request_frames.end(), frames.begin() + 1,
                          frames.end());
    std::string message, server_key;
    std::optional<ClientSecret> candidate;
    if (auth_mode == "password") {
      if (!_password && !_secret)
        throw AuthError("this broker requires a password");
      if (!h.value("salt", "").size() || !h.contains("opslimit") ||
          !h.contains("memlimit") || !h.contains("nonce") ||
          !h["opslimit"].is_number_unsigned() ||
          !h["memlimit"].is_number_unsigned() || !h["nonce"].is_string() ||
          !h["salt"].is_string())
        throw std::runtime_error("malformed hello reply from the edit service");
      auto salt = b64_decode(h["salt"], kSaltBytes);
      auto nonce = b64_decode(h["nonce"], kNonceBytes);
      if (!salt || !nonce)
        throw std::runtime_error("malformed hello reply from the edit service");
      const unsigned long long ops = h["opslimit"];
      const unsigned long long mem = h["memlimit"];
      const bool remembered = _secret && _secret->salt == *salt &&
                              _secret->opslimit == ops &&
                              _secret->memlimit == mem;
      if (!remembered) {
        // A remembered secret for other parameters (the broker's password
        // was changed) is useless without the password.
        if (!_password)
          throw AuthError("the remembered password is no longer valid");
        try {
          // The expensive step; its parameters are capped by derive(). Kept
          // as a candidate: it is remembered only once the server accepts it.
          candidate = ClientSecret::derive(*_password, *salt, ops,
                                           static_cast<std::size_t>(mem));
        } catch (const std::invalid_argument &err) {
          throw std::runtime_error(std::string("refusing the broker's ") +
                                   "password parameters: " + err.what());
        }
      }
      const ClientSecret &use = remembered ? *_secret : *candidate;
      const std::string client_nonce = random_bytes(kNonceBytes);
      const std::vector<std::string> args(frames.begin() + 1, frames.end());
      message = auth_message(*nonce, client_nonce, frames[0], args);
      std::string signature = hmac(sha256(use.client_key), message);
      std::string proof = xor_strings(use.client_key, signature);
      wipe(signature);
      request_frames[1] = *nonce + client_nonce + proof;
      server_key = use.server_key;
    } else if (auth_mode == "none") {
      // Without CURVE, plaintext tcp is only ever served with a password: a
      // "none" there is a downgrade (or a misconfigured server) and the
      // reply could not be trusted.
      if (!_keys && Mads::detail::parse_endpoint(_endpoint)->is_tcp())
        throw std::runtime_error(
            "refusing to talk to " + _endpoint +
            ": plaintext tcp without a password is not allowed");
    } else {
      throw std::runtime_error("unknown authentication mode from the "
                               "edit service");
    }

    std::vector<std::string> reply = exchange(request_frames);
    if (reply[1] == "auth") {
      if (!message.empty() && !_password)
        throw AuthError("the remembered password is no longer valid");
      throw AuthError(reply.size() > 2 ? reply[2] : "authentication failed");
    }
    if (!message.empty()) {
      // Every reply to an authenticated request must carry a valid signature.
      const std::string signature = reply.back();
      reply.pop_back();
      if (reply.size() < 2 ||
          !equal_ct(signature, hmac(server_key, message + frames_hash(reply))))
        throw std::runtime_error(
            "reply from " + _endpoint +
            " is not authentic (missing or invalid signature)");
      if (candidate) {
        // The server accepted the proof and proved it knows the verifier:
        // the derived secret is right, keep it.
        set_secret(std::move(*candidate));
      }
    }
    return reply;
  } catch (const zmq::error_t &err) {
    if (err.num() == EAGAIN)
      throw std::runtime_error(timeout_message);
    throw std::runtime_error("edit connection to " + _endpoint + ": " +
                             err.what());
  }
}

namespace {

// Throws the server's message for an "error" reply, or a protocol error.
void expect_ok(const std::vector<std::string> &reply, std::size_t frames) {
  if (reply[1] == "error")
    throw std::runtime_error(reply.size() > 2 ? reply[2] : "remote error");
  if (reply[1] != "ok" || reply.size() != frames)
    throw std::runtime_error("unexpected reply from the edit service");
}

} // namespace

Loaded RemoteSource::load() {
  const auto reply = request({"get"});
  expect_ok(reply, 4);
  return {reply[3], reply[2]};
}

SaveResult RemoteSource::save(const std::string &text,
                              const std::string &base_etag) {
  SaveResult result;
  try {
    const auto reply = request({"put", base_etag, text});
    if (reply[1] == "ok" && reply.size() == 3) {
      result.status = SaveResult::Status::Ok;
      result.etag = reply[2];
    } else if (reply[1] == "conflict" && reply.size() == 4) {
      result.status = SaveResult::Status::Conflict;
      result.etag = reply[2]; // the current etag; the text is not kept
      result.message = _endpoint + " changed since it was loaded";
    } else if (reply[1] == "error") {
      result.message = reply.size() > 2 ? reply[2] : "remote error";
    } else {
      result.message = "unexpected reply from the edit service";
    }
  } catch (const AuthError &err) {
    result.status = SaveResult::Status::Error;
    result.message = err.what();
    result.auth_required = true;
  } catch (const std::exception &err) {
    result.status = SaveResult::Status::Error;
    result.message = err.what();
  }
  return result;
}

std::optional<ClientSecret> RemoteSource::secret() const { return _secret; }

void RemoteSource::set_secret(ClientSecret secret) {
  if (_secret) {
    wipe(_secret->client_key);
    wipe(_secret->server_key);
  }
  _secret = std::move(secret);
}

std::string RemoteSource::current_etag() {
  const auto j = nlohmann::json::parse(info(), nullptr, false);
  if (!j.is_object() || !j.contains("etag") || !j["etag"].is_string())
    throw std::runtime_error("malformed info reply from the edit service");
  return j["etag"];
}

std::string RemoteSource::describe() const { return _endpoint + " (remote)"; }

std::string RemoteSource::info() {
  const auto reply = request({"info"});
  expect_ok(reply, 3);
  return reply[2];
}

} // namespace Mads::Ini::Remote
