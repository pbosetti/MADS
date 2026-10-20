/*
 ____                      _         _____    _ _ _
|  _ \ ___ _ __ ___   ___ | |_ ___  | ____|__| (_) |_
| |_) / _ \ '_ ` _ \ / _ \| __/ _ \ |  _| / _` | | __|
|  _ <  __/ | | | | | (_) | ||  __/ | |__| (_| | | |_
|_| \_\___|_| |_| |_|\___/ \__\___| |_____\__,_|_|\__|

Remote editing of the broker's settings file: an opt-in service the broker
runs on its own socket (`[broker] remote_edit = true`), and the client side
used by `mads ini --remote` and by the editor's File > Open remote.

Security model: over tcp the service only runs on a CURVE server socket
(the broker refuses to offer it without --crypto), in the ZAP domain
kZapDomain, whose client allowlist holds ONLY the editor keys (`*.pub` in
the broker's editors_dir, by default <keys_dir>/editors) -- not the agent
keys that may use the bus. On the broker's host it is also offered on an
ipc:// endpoint without CURVE: like the bus's ipc endpoints, its access
control is the owner-only runtime directory holding the socket file, so
only processes of the broker's OS user can reach it. Plaintext is never
accepted on anything but ipc://. The service can read and replace exactly
one file, the broker's own settings file: requests carry no paths.

Protocol: multipart messages, a REQ client talking to a ROUTER server.
Every request starts with the client's LIB_VERSION; every reply starts with
the server's LIB_VERSION and a status ("ok", "conflict" or "error"):

  get                    -> ok, etag, text
  put, base_etag, text   -> ok, etag                    (written)
                         -> conflict, etag, text        (file changed since
                                                         base_etag; nothing
                                                         written; current
                                                         content returned)
                         -> error, message              (invalid TOML, too
                                                         large, I/O error)
  info                   -> ok, json {path, size, mtime, etag, version}
  anything else, or a version Mads::check_version() rejects
                         -> error, message

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
#include <string>
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

/// Server-side logic, independent of sockets (unit-testable).
class EditService {
public:
  /// Serves `settings_path`, keeping the newest `max_backups` backups
  /// `<file>.<YYYYmmdd-HHMMSS>.bak` (plus a counter on clashes) next to it.
  explicit EditService(std::filesystem::path settings_path,
                       std::size_t max_backups = 10);

  /// Handles one request (frames after the routing envelope) and returns the
  /// reply frames. `user` identifies the client in log lines (the Z85 public
  /// key from ZAP's User-Id, or "" when unknown). Never throws.
  std::vector<std::string> handle(const std::vector<std::string> &request,
                                  const std::string &user = "");

  /// Called with one human-readable line per put (who, outcome, size) and
  /// per error. Default: none.
  std::function<void(const std::string &)> log;

private:
  std::filesystem::path _path;
  std::size_t _max_backups;
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
  /// accepts only for an ipc:// endpoint.
  std::optional<ServerKeys> curve;
};

/// Binds one ROUTER socket per listener in `context` and serves `service`
/// until `running` turns false (polls at least every 200 ms). A CURVE
/// listener sets ZMQ_ZAP_DOMAIN = kZapDomain (the context must already run a
/// ZapAuth handler that knows it, see ZapAuth::configure_domain_curve()) and
/// refuses requests without a ZAP User-Id; a plaintext ipc listener reports
/// its requests as user "ipc". An ipc listener does not bind over a socket
/// file another process still listens on, and removes its socket file on
/// return. Throws std::invalid_argument for a plaintext non-ipc listener and
/// zmq::error_t / std::runtime_error when an endpoint cannot be bound,
/// before serving anything.
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

/// The broker's settings file seen through the edit service. One REQ socket
/// per request (no stuck state after a timeout). load() throws
/// std::runtime_error on timeout, refused authentication (reported as a
/// timeout by ZMQ: the message says so) or an "error" reply.
class RemoteSource : public Source {
public:
  /// `endpoint` like "tcp://host:9094" (needs `keys`) or the ipc:// one of
  /// local_endpoint() (plaintext: `keys` must be empty).
  RemoteSource(std::string endpoint, std::optional<ClientKeys> keys,
               std::chrono::milliseconds timeout = std::chrono::seconds(5));
  ~RemoteSource() override;
  Loaded load() override;
  SaveResult save(const std::string &text,
                  const std::string &base_etag) override;
  /// "<endpoint> (remote)".
  std::string describe() const override;
  /// The info reply as JSON text; throws like load().
  std::string info();

private:
  std::vector<std::string> request(const std::vector<std::string> &frames);

  std::string _endpoint;
  std::optional<ClientKeys> _keys;
  std::chrono::milliseconds _timeout;
  zmq::context_t *_context; // owned
};

} // namespace Mads::Ini::Remote
