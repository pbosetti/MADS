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

} // namespace Mads

#endif // MADS_BROKER_PROBE_HPP
