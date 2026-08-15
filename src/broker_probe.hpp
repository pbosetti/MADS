/*
  ____            _
 | __ ) _ __ ___ | | _____ _ __
 |  _ \| '__/ _ \| |/ / _ \ '__|
 | |_) | | | (_) |   <  __/ |
 |____/|_|  \___/|_|\_\___|_|
  ____            _
 |  _ \ _ __ ___ | |__   ___
 | |_) | '__/ _ \| '_ \ / _ \
 |  __/| | | (_) | |_) |  __/
 |_|   |_|  \___/|_.__/ \___|

Small, dependency-light readiness probes shared by `mads up`'s
`ready = "broker" | "port:<n>"` and (later, on a different branch) by
`mads doctor`'s broker-reachability check. Kept outside agent.hpp/.cpp on
purpose: neither probe needs a full Mads::Agent.

Author(s): Paolo Bosetti
*/
#ifndef MADS_BROKER_PROBE_HPP
#define MADS_BROKER_PROBE_HPP

#include <chrono>
#include <filesystem>
#include <string>

namespace Mads {

/**
 * @brief Probes whether a MADS broker's settings endpoint is up and speaking
 * the MADS wire protocol: connects a REQ socket to `uri`, sends
 * `[LIB_VERSION, "settings", <probe-name>]` and waits up to `timeout` for any
 * reply. Any protocol-shaped reply counts as success -- this does not try to
 * load or validate settings for a real agent, it only proves the broker is
 * listening and answering. ZeroMQ's own TCP reconnect logic means a single
 * bounded receive is enough to also cover "not up yet" within `timeout`.
 *
 * @param uri broker settings endpoint, e.g. "tcp://localhost:9092".
 * @param timeout time budget to wait for a reachable broker.
 * @return true if the broker replied within timeout, false otherwise (never
 * throws).
 */
bool probe_broker(const std::string &uri, std::chrono::milliseconds timeout);

/**
 * @brief Probes whether a local TCP port is accepting connections, retrying
 * (short bounded attempts) until `timeout` elapses. Used by
 * `ready = "port:<n>"`.
 *
 * @param host hostname or IP to probe; "localhost" and an empty string both
 * mean 127.0.0.1.
 * @param port TCP port number.
 * @param timeout time budget to wait for the port to start accepting.
 * @return true if a connection succeeded within timeout, false otherwise
 * (never throws).
 */
bool probe_tcp_port(const std::string &host, int port,
                    std::chrono::milliseconds timeout);

/// Outcome of a CURVE-handshake probe (ZMQ_DEVELOPMENT.md §2.1): what a
/// socket monitor actually observed, distinguishing "the broker rejected
/// this key" from "nothing answered at all" -- a rejection previously
/// surfaced as an identical bare timeout in both cases.
enum class CurveProbeResult { Connected, RejectedAuth, Timeout };

/**
 * @brief Attempts a real CURVE handshake against `uri`, using the client and
 * server keys Mads::CurveAuth::setup_curve_client() reads from `key_dir`
 * (same file convention as `mads doctor --crypto`'s key check). A
 * Mads::SocketMonitor attached before connecting distinguishes a genuine
 * ZMQ_EVENT_HANDSHAKE_FAILED_AUTH rejection from an unreachable broker or an
 * unresponsive one, both of which previously looked identical -- a timeout.
 *
 * @param uri broker endpoint to connect a CURVE client socket to (frontend,
 * backend, or settings -- any CURVE-secured socket works).
 * @param key_dir directory holding the client/server key files.
 * @param client_key_name base name of the client's key files.
 * @param server_key_name base name of the server's public key file.
 * @param timeout time budget to wait for the handshake to resolve.
 * @return the observed outcome; never throws (a key-loading failure or any
 * other exception is reported as Timeout).
 */
CurveProbeResult
probe_curve_handshake(const std::string &uri,
                      const std::filesystem::path &key_dir,
                      const std::string &client_key_name,
                      const std::string &server_key_name,
                      std::chrono::milliseconds timeout);

} // namespace Mads

#endif // MADS_BROKER_PROBE_HPP
