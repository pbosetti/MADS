// GUI-free helpers of the editor's remote side: address parsing, the blocking
// connect step (run on a worker thread) and the background etag poller.
#pragma once

#include "remote_edit.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace MadsIni {

/// Where and how to reach a broker's edit service.
struct RemoteTarget {
  std::string endpoint; ///< tcp://host:port or ipc://path
  bool use_keys = false; ///< CURVE (tcp only)
  std::string client_key_file;
  std::string broker_key_file;
};

/// "host", "host:port", "tcp://host[:port]" or "ipc://path" -> an endpoint
/// (bare hosts get kDefaultPort). Empty for blank input.
std::string normalize_address(const std::string &text);
bool is_ipc_endpoint(const std::string &endpoint);

/// Result of connect_remote().
struct ConnectOutcome {
  /// A source whose load() succeeded (set on success only).
  std::unique_ptr<Mads::Ini::Remote::RemoteSource> source;
  /// Its secret, taken before the source is handed over (a password was needed).
  std::optional<Mads::Ini::Remote::ClientSecret> secret;
  bool auth_required = false; ///< wrong or missing password: ask and retry
  std::string error;
};

/// Builds the source (keys loaded from the files when `target.use_keys`),
/// preloads `remembered`, calls load() once. Blocking: network and Argon2.
/// Never throws.
ConnectOutcome connect_remote(const RemoteTarget &target,
                              std::optional<Mads::Ini::Remote::ClientSecret> remembered,
                              std::optional<std::string> password);

/// Polls current_etag() of a broker from a worker thread with its own
/// source. The main thread only reads the last sample.
class RemotePoller {
public:
  RemotePoller() = default;
  ~RemotePoller();
  RemotePoller(const RemotePoller &) = delete;
  RemotePoller &operator=(const RemotePoller &) = delete;

  void start(RemoteTarget target, std::optional<Mads::Ini::Remote::ClientSecret> secret,
             std::chrono::milliseconds interval = std::chrono::seconds(5),
             std::chrono::milliseconds timeout = std::chrono::seconds(2));
  void stop();
  bool running() const { return _thread.joinable(); }
  /// Forgets the last sample and discards one being fetched: call whenever
  /// the editor's own etag changed (load, save).
  void invalidate();
  /// The etag seen by the last completed poll, if any.
  std::optional<std::string> sample() const;

private:
  void loop(RemoteTarget target, std::optional<Mads::Ini::Remote::ClientSecret> secret,
            std::chrono::milliseconds interval, std::chrono::milliseconds timeout);

  std::thread _thread;
  std::atomic<bool> _stop{false};
  mutable std::mutex _mutex;
  std::optional<std::string> _sample;
  unsigned _generation = 0;
};

} // namespace MadsIni
