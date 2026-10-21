/*
 ____                      _         _____    _ _ _
|  _ \ ___ _ __ ___   ___ | |_ ___  | ____|__| (_) |_
| |_) / _ \ '_ ` _ \ / _ \| __/ _ \ |  _| / _` | | __|
|  _ <  __/ | | | | | (_) | ||  __/ | |__| (_| | | |_
|_| \_\___|_| |_| |_|\___/ \__\___| |_____\__,_|_|\__|

Remote editing of the broker's settings file: an opt-in service the broker
runs on its own sockets (`[broker] remote_edit = true`), and the client side
used by the editor's Open panel.

Access control, per listener:

  ipc://, on the broker's host  plaintext; the owner-only runtime directory
                                holding the socket file is the access control
                                (only the broker's OS user gets in), as for
                                the bus's ipc endpoints. Never a password.
  tcp://, broker with --crypto  CURVE, ZAP domain kZapDomain. The client key
                                must be an editor key (editors_dir) or, when
                                editors_dir is empty and a password is set, a
                                bus key. Plus the password, if one is set.
  tcp://, broker without crypto only with a password ("development mode"):
                                the password is never sent and requests
                                cannot be altered in transit, but the file
                                contents travel in clear.

The password is never stored nor sent: the broker keeps a SCRAM-style
verifier (see PasswordVerifier) in a separate, owner-only file next to the
settings file, never served by anything. The service can read and replace
exactly one file, the broker's own settings file: requests carry no paths.

Protocol: multipart messages, a REQ client talking to a ROUTER server.
Requests are [LIB_VERSION, command, auth, args...]; replies are
[LIB_VERSION, status, results...] where status is "ok", "conflict", "auth"
or "error":

  hello                  -> ok, json {auth: "none"|"password", and with a
                            password: salt, opslimit, memlimit (the verifier's
                            KDF parameters) and nonce (a fresh server nonce)}
  get                    -> ok, etag, text
  put  base_etag, text   -> ok, etag                    (written)
                         -> conflict, etag, text        (file changed since
                                                         base_etag; nothing
                                                         written)
                         -> error, message              (invalid TOML, too
                                                         large, I/O error)
  info                   -> ok, json {path, size, mtime, etag, version, auth}
  any of get/put/info on a password listener without a valid auth frame
                         -> auth, message
  anything else, or a version Mads::check_version() rejects
                         -> error, message

The auth frame is empty when no password is involved. Otherwise it is the
96 bytes server_nonce (32, from a hello) || client_nonce (32, random) ||
client_proof (32). The server looks the server nonce up and consumes it
whatever the outcome (unknown or expired -> auth). The proof is computed over

  auth_message = "mads-edit-v1" || server_nonce || client_nonce
                 || command || 0x00 || SHA-256(L(arg_1) || ... || L(arg_n))

where L(x) = 8-byte big-endian length of x || x, and server_nonce is the one
from a hello (single use, valid 60 s). With
ClientKey/StoredKey/ServerKey as in PasswordVerifier:

  client_signature = HMAC-SHA-256(StoredKey, auth_message)
  client_proof     = ClientKey XOR client_signature

The server recovers ClientKey and accepts iff SHA-256(ClientKey) equals
StoredKey (constant-time). Every reply to an authenticated request gets a
final frame HMAC-SHA-256(ServerKey, auth_message || SHA-256(L(reply_1) ||
... || L(reply_m))) over the other reply frames; the client rejects replies
without a valid one. After 5 consecutive failed proofs the service answers
"auth" to every password request for 30 s.

etags are Mads::Ini::content_etag() of the file content.

Author(s): Paolo Bosetti
*/
#pragma once

#include "ini_document.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace zmq {
class context_t;
}

namespace Mads::Ini::Remote {

/// ZAP domain of the edit socket: its allowlist is separate from the bus's.
inline constexpr const char *kZapDomain = "mads-edit";
inline constexpr int kDefaultPort = 9094;
/// Name of the edit port in the broker's service-discovery announcement.
inline constexpr const char *kDiscoveryPortName = "edit";
/// Largest settings file accepted by put.
inline constexpr std::size_t kMaxFileSize = 4 * 1024 * 1024;

/// SCRAM-style password verifier (RFC 5802 roles, libsodium primitives):
///
///   SaltedPassword = Argon2id13(password, salt[16], opslimit, memlimit), 32 B
///   ClientKey      = HMAC-SHA-256(SaltedPassword, "Client Key")
///   StoredKey      = SHA-256(ClientKey)
///   ServerKey      = HMAC-SHA-256(SaltedPassword, "Server Key")
///
/// The broker keeps salt, KDF parameters, StoredKey and ServerKey: enough to
/// check a proof, not to produce one, so a leaked verifier allows at most an
/// (Argon2-slowed) offline guessing attack.
struct PasswordVerifier {
  unsigned long long opslimit = 0;
  std::size_t memlimit = 0;
  std::string salt;       ///< 16 raw bytes
  std::string stored_key; ///< 32 raw bytes
  std::string server_key; ///< 32 raw bytes

  /// Derives a verifier with a fresh random salt and libsodium's MODERATE
  /// Argon2id limits (about a second and 256 MiB on a desktop).
  static PasswordVerifier create(std::string_view password);
  /// One line: `mads-edit-password v1 argon2id13 <opslimit> <memlimit>
  /// <salt> <stored_key> <server_key>` (standard base64).
  std::string to_string() const;
  /// Parses to_string()'s format; nullopt (and `error`) when malformed or
  /// with KDF parameters outside libsodium's valid range.
  static std::optional<PasswordVerifier> parse(std::string_view line,
                                               std::string *error = nullptr);
};

/// `<settings_path>.edit_password`.
std::filesystem::path password_file_for(const std::filesystem::path &settings);
/// Writes `verifier` atomically with owner-only permissions (POSIX: 0600).
/// Returns false and sets `error` on failure.
bool write_password_file(const std::filesystem::path &file,
                         const PasswordVerifier &verifier,
                         std::string *error = nullptr);
/// Reads a verifier file: nullopt when absent; throws std::runtime_error when
/// present but unreadable or malformed. Appends a warning when the file is
/// readable or writable by group or others (POSIX).
std::optional<PasswordVerifier>
read_password_file(const std::filesystem::path &file,
                   std::vector<std::string> *warnings = nullptr);

/// Client-side key material derived from a password and a verifier's KDF
/// parameters (the expensive Argon2id step, done once and cached).
struct ClientSecret {
  std::string salt;
  unsigned long long opslimit = 0;
  std::size_t memlimit = 0;
  std::string client_key; ///< 32 raw bytes
  std::string server_key; ///< 32 raw bytes
  /// Throws std::invalid_argument for KDF parameters a client must not
  /// accept from an unauthenticated hello: outside libsodium's range, or
  /// memlimit > 1 GiB, or opslimit > crypto_pwhash_OPSLIMIT_SENSITIVE.
  static ClientSecret derive(std::string_view password, std::string salt,
                             unsigned long long opslimit, std::size_t memlimit);
  /// One line: `mads-edit-secret v1 argon2id13 <opslimit> <memlimit> <salt>
  /// <client_key> <server_key>` (standard base64), for a user prefs file.
  ///
  /// SECURITY: the stored line is password-equivalent for that one broker:
  /// whoever reads it can authenticate to (and be authenticated by) the
  /// broker whose verifier it matches, until the broker's password changes.
  /// The password itself cannot be recovered from it (Argon2id), and it is
  /// useless for any other broker or service. Store it like a password
  /// (owner-only file).
  std::string to_string() const;
  /// Parses to_string()'s format; nullopt when malformed or with KDF
  /// parameters outside libsodium's valid range.
  static std::optional<ClientSecret> parse(std::string_view line);
};

/// Server-side logic, independent of sockets (unit-testable).
class EditService {
public:
  /// Serves `settings_path`, keeping the newest `max_backups` backups
  /// `<file>.<YYYYmmdd-HHMMSS>.bak` (plus a counter on clashes) next to it.
  /// With a `password`, listeners that require it check the auth frame.
  explicit EditService(std::filesystem::path settings_path,
                       std::size_t max_backups = 10,
                       std::optional<PasswordVerifier> password = {});

  /// Handles one request (frames after the routing envelope) and returns the
  /// reply frames. `user` identifies the client in log lines (the Z85 public
  /// key from ZAP's User-Id, "ipc", or "" when unknown). With
  /// `require_password`, get/put/info need a valid auth frame (and a
  /// password must have been configured, else every such request is
  /// refused). Never throws.
  std::vector<std::string> handle(const std::vector<std::string> &request,
                                  const std::string &user = "",
                                  bool require_password = false);

  bool has_password() const { return _password.has_value(); }

  /// Called with one human-readable line per put (who, outcome, size), per
  /// failed authentication and per error. Default: none.
  std::function<void(const std::string &)> log;

private:
  std::filesystem::path _path;
  std::size_t _max_backups;
  std::optional<PasswordVerifier> _password;
  // Outstanding server nonces with their expiry (single use, 60 s, at most
  // 256 kept), failed-proof counter and lockout deadline.
  struct Nonce {
    std::string value;
    std::chrono::steady_clock::time_point expires;
  };
  std::vector<Nonce> _nonces;
  int _failures = 0;
  std::chrono::steady_clock::time_point _locked_until{};
};

/// CURVE material of the server socket.
struct ServerKeys {
  std::string public_key; ///< Z85, the broker's
  std::string secret_key; ///< Z85, the broker's
};

/// One endpoint the service listens on.
struct Listener {
  std::string endpoint;
  /// CURVE keys; without them the listener is plaintext, which serve()
  /// accepts only for an ipc:// endpoint or with `require_password`.
  std::optional<ServerKeys> curve;
  /// get/put/info need the password (never on ipc:// listeners).
  bool require_password = false;
};

/// Binds one ROUTER socket per listener in `context` and serves `service`
/// until `running` turns false (polls at least every 200 ms). A CURVE
/// listener sets ZMQ_ZAP_DOMAIN = kZapDomain (the context must already run a
/// ZapAuth handler that knows it, see ZapAuth::configure_domain_curve()) and
/// refuses requests without a ZAP User-Id; a plaintext ipc listener reports
/// its requests as user "ipc". An ipc listener does not bind over a socket
/// file another process still listens on, and removes its socket file on
/// return. Throws std::invalid_argument for a plaintext tcp listener without
/// require_password, for require_password on an ipc listener or on a
/// service without a password, and zmq::error_t / std::runtime_error when an
/// endpoint cannot be bound -- all before serving anything.
void serve(zmq::context_t &context, const std::vector<Listener> &listeners,
           EditService &service, const std::atomic<bool> &running);

/// The ipc:// endpoint of the edit service of a broker on this host, named
/// like the broker's other ipc endpoints (Mads::detail::default_ipc_endpoint
/// with role "edit") in `ipc_dir`, by default the same runtime directory the
/// broker uses.
std::string local_endpoint(int port = kDefaultPort,
                           const std::filesystem::path &ipc_dir = {});

/// CURVE material of a client.
struct ClientKeys {
  std::string public_key;        ///< Z85, the editor's own
  std::string secret_key;        ///< Z85, the editor's own
  std::string server_public_key; ///< Z85, the broker's
};

/// Reads `<dir>/<client>.pub`, `<dir>/<client>.key` and `<dir>/<server>.pub`
/// (first line of each, as CurveAuth does). Throws std::runtime_error naming
/// the missing or malformed file.
ClientKeys load_client_keys(const std::filesystem::path &dir,
                            const std::string &client = "editor",
                            const std::string &server = "broker");

/// Like load_client_keys() for explicit files: `client_secret_key_file` is
/// the `NAME.key` written by `mads --keypair NAME` (40 Z85 characters); the
/// client public key is its sibling `NAME.pub`. `server_public_key_file` is
/// the broker's `.pub`. Throws std::runtime_error naming the bad file.
ClientKeys
load_client_key_files(const std::filesystem::path &client_secret_key_file,
                      const std::filesystem::path &server_public_key_file);

/// Thrown by RemoteSource when the service wants a password and none, or a
/// wrong one, was given (status "auth"): the caller may ask for one and retry.
class AuthError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/// The broker's settings file seen through the edit service. One REQ socket
/// per request (no stuck state after a timeout), starting with a hello: the
/// server says whether it wants the password and hands out the nonce. load()
/// throws AuthError on an "auth" reply and std::runtime_error on timeout,
/// CURVE refusal (reported as a timeout by ZMQ: the message says so), an
/// unauthenticated reply to an authenticated request, or an "error" reply.
class RemoteSource : public Source {
public:
  /// `endpoint` like "tcp://host:9094" or the ipc:// one of local_endpoint().
  /// `keys` select CURVE: required for tcp:// to a broker with --crypto,
  /// refused for ipc://. `password` is used when the server asks for it
  /// (ClientSecret derived on first use and cached).
  RemoteSource(std::string endpoint, std::optional<ClientKeys> keys,
               std::optional<std::string> password = {},
               std::chrono::milliseconds timeout = std::chrono::seconds(5));
  ~RemoteSource() override;
  Loaded load() override;
  SaveResult save(const std::string &text,
                  const std::string &base_etag) override;
  /// "<endpoint> (remote)".
  std::string describe() const override;
  /// The info reply as JSON text; throws like load().
  std::string info();
  /// The etag of the broker's current file, from the (authenticated) info
  /// command; throws like load().
  std::string current_etag() override;

  /// The password-derived secret, set after the first request the server
  /// accepted (nullopt before, and for servers that need no password).
  /// Password-equivalent for that broker: see ClientSecret::to_string().
  std::optional<ClientSecret> secret() const;
  /// Preloads a remembered secret. When a hello's salt and KDF parameters
  /// match it, it is used without deriving, so no password is needed. If
  /// they do not match (the password was changed) and there is no password,
  /// or a proof made with it is refused, the request throws
  /// AuthError("the remembered password is no longer valid").
  void set_secret(ClientSecret secret);

private:
  std::vector<std::string> request(const std::vector<std::string> &frames);

  std::string _endpoint;
  std::optional<ClientKeys> _keys;
  std::optional<std::string> _password; // wiped (sodium_memzero) on destruction
  std::optional<ClientSecret> _secret;
  std::chrono::milliseconds _timeout;
  zmq::context_t *_context; // owned
};

} // namespace Mads::Ini::Remote
