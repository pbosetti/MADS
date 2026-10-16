/*
  ____             _              ____ _               _
 |  _ \  ___   ___| |_ ___  _ __ / ___| |__   ___  ___| | _____
 | | | |/ _ \ / __| __/ _ \| '__| |   | '_ \ / _ \/ __| |/ / __|
 | |_| | (_) | (__| || (_) | |  | |___| | | |  __/ (__|   <\__ \
 |____/ \___/ \___|\__\___/|_|   \____|_| |_|\___|\___|_|\_\___/

Pure, unit-testable check logic backing `mads doctor` (src/main/doctor.cpp).

Every function here either performs a small, self-contained, dependency-light
check (settings-file parsing via toml++, CURVE key well-formedness via
libzmq's Z85 codec, reading the pinned plugin protocol out of a JSON manifest
text) or is a pure *evaluator* that turns already-collected facts (a broker
probe's boolean result, a dry-run plugin load's outcome) into a CheckResult.

Deliberately NOT here: the actual pugg::Kernel-based plugin dry-run load. That
needs the mads_plugin headers (source.hpp/filter.hpp/sink.hpp) and pugg's
Kernel.h, which -- exactly like mads-source/-filter/-sink -- are only on the
include path for src/main (see the top-level CMakeLists.txt's
`include_directories()` call, scoped to src/main and src/plugin). So the
actual dlopen/registration/create() sequence
lives in src/main/doctor.cpp (mirroring src/main/plugin_loader.cpp), which
then hands the resulting facts to evaluate_plugin_load()/evaluate_plugin_protocol()
here for the pass/warn/fail decision -- keeping that decision logic testable
without linking pugg or building a real .plugin fixture.

Author(s): Paolo Bosetti
*/
#ifndef MADS_DOCTOR_CHECKS_HPP
#define MADS_DOCTOR_CHECKS_HPP

#include "broker_probe.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Mads {
namespace Doctor {

/// Outcome of a single check, mirroring the three-state model of
/// `ros2 doctor`/`brew doctor`: Pass (nothing to do), Warn (works but worth a
/// look), Fail (needs fixing before things will work).
enum class Status { Pass, Warn, Fail };

/// One check's result: a status, a human-readable message, and (when
/// actionable) a one-line fix hint. `fixable` marks the small set of checks
/// `--fix` knows how to safely auto-remediate (currently: a missing settings
/// file, and orphaned shared-memory segments).
struct CheckResult {
  std::string name;
  Status status = Status::Pass;
  std::string message;
  std::string fix_hint;
  bool fixable = false;
};

/// Mirrors AgentAppT::CryptoOptions (src/agent_app.hpp): the same
/// key_dir/client/server naming convention every mads-* executable's
/// --crypto/--keys_dir/--key_client/--key_broker flags configure. Declared up
/// here rather than beside check 5, because checks 2 and 5 and 7 all need it:
/// probing a CURVE-secured broker in the clear cannot tell it apart from a
/// broker that is down (see Mads::ProbeCurveKeys).
using CurveKeyCheck = Mads::ProbeCurveKeys;

/* ---- 1. Settings file found and parses ---------------------------------- */

/**
 * @brief Checks that `path` is a local settings file that exists and parses
 * as valid TOML. A `tcp://...` URI is treated as "remote broker, not a local
 * file" and reported Pass with an explanatory note (broker reachability is
 * check 2's job, not this one's).
 */
CheckResult check_settings_file(const std::filesystem::path &path);

/* ---- 2. Broker reachable -------------------------------------------------
   The actual probe is Mads::probe_broker() (src/broker_probe.hpp, from P5);
   evaluate_broker_reachable() is split out so the pass/warn/fail wording is
   testable without a real socket.

   `curve` must be set whenever the broker runs with --crypto: the probe is an
   ordinary REQ round-trip, so an unencrypted one gets dropped in the ZMTP
   handshake and times out indistinguishably from a broker that is not there.
   `crypto` on the evaluator only selects the wording for that case. */

CheckResult evaluate_broker_reachable(const std::string &uri, bool reachable,
                                      bool crypto = false);

CheckResult
check_broker_reachable(const std::string &uri, std::chrono::milliseconds timeout,
                       const std::optional<CurveKeyCheck> &curve = std::nullopt);

/* ---- 3. Declared plugin(s) resolve and load (dry-run) -------------------- */

/// Facts collected by src/main/doctor.cpp's pugg::Kernel dry-run load (see
/// the file-level comment above for why the loading itself lives there).
struct PluginLoadFacts {
  std::string plugin_file;   // path probed
  bool file_exists = false;
  bool loaded = false;       // load + driver lookup + create() all succeeded
  std::string driver_kind;   // "source" | "filter" | "sink" | "" (unknown)
  std::string driver_name;   // the created instance's own kind()
  int protocol_version = -1; // pugg::Driver::version(); -1 if unknown
  std::string error;         // populated when loaded == false
};

CheckResult evaluate_plugin_load(const PluginLoadFacts &facts);

/* ---- 4. Plugin protocol matches the pinned mads_plugin version ---------- */

/// Mirrors `MADS_PLUGIN_MIN_PROTOCOL` in src/main/plugin_loader.cpp: the
/// oldest plugin protocol the loaders are meant to accept. In practice a
/// successful dry-run load (check 3) already implies at least this, since
/// pugg::Kernel::add_server<T>() gates driver registration on the *current*
/// pinned protocol (see doctor.cpp); kept here for a defensive, explicit
/// floor rather than relying on that gate alone.
constexpr int kMinSupportedPluginProtocol = 7;

/**
 * @brief Reads the `plugin_protocol` field out of share/plugin_deps.json's
 * *text* content (already read by the caller -- doctor.cpp reuses
 * Mads::PluginMigrate::read_file() for that, the same helper
 * src/main/plugin_migrate.hpp's own migration engine uses to load the same
 * file). Kept as a pure string->int function so it needs no filesystem
 * access to unit test.
 */
std::optional<int> parse_pinned_plugin_protocol(const std::string &manifest_json_text);

CheckResult evaluate_plugin_protocol(int loaded_version,
                                     std::optional<int> pinned_version,
                                     int min_supported = kMinSupportedPluginProtocol);

/* ---- 5. CURVE key files exist and are well-formed ------------------------
   Uses CurveKeyCheck, declared at the top of this header because check 2
   needs it too. */

/// True if `key_line` (one file line, already trimmed of trailing
/// CR/LF/whitespace) is a well-formed Z85-encoded CURVE key: exactly 40
/// characters that libzmq's zmq_z85_decode() accepts.
bool is_well_formed_curve_key(const std::string &key_line);

/**
 * @brief Checks that a client's CURVE key files exist and are well-formed:
 * `<client>.key` (secret), `<client>.pub` (public), and `<server>.pub` (the
 * broker's public key the client needs to encrypt toward it) -- exactly the
 * three files Mads::CurveAuth::setup_curve_client(socket, client, server)
 * (src/curve.hpp) reads.
 */
CheckResult check_curve_keys(const CurveKeyCheck &cfg);

/* ---- 6. Local port-availability sanity check ------------------------------
   The actual probe is Mads::probe_tcp_port() (src/broker_probe.hpp): its
   "true" means "something answered", i.e. the *opposite* of what doctor
   wants to hear before launching a broker on that port -- so the evaluator
   below inverts the pass/fail mapping relative to `ready = "port:<n>"`'s own
   use of the same probe. */

CheckResult evaluate_port_available(const std::string &host, int port, bool in_use);

CheckResult check_port_available(const std::string &host, int port,
                                 std::chrono::milliseconds timeout);

/* ---- 7. CURVE handshake actually succeeds ---------------------------------
   The actual probe is Mads::probe_curve_handshake() (src/broker_probe.hpp,
   from ZMQ_DEVELOPMENT.md §2.1); evaluate_curve_handshake() is split out so
   the pass/fail wording is testable without a real socket, mirroring
   check 2. Reuses CurveKeyCheck (check 5) for the key-file location. */

CheckResult evaluate_curve_handshake(const std::string &uri,
                                     CurveProbeResult result);

CheckResult check_curve_handshake(const std::string &uri, const CurveKeyCheck &cfg,
                                  std::chrono::milliseconds timeout);

/* ---- 8. Open-file limit leaves room for the fleet -------------------------
   The broker holds two descriptors per connected agent -- the agent's
   publisher on the XSUB frontend, its subscriber on the XPUB backend -- so
   RLIMIT_NOFILE, not anything in libzmq, is what bounds fleet size. The
   default soft limit of 1024 stops at roughly 495 agents, and libzmq refuses
   everything past it near-silently.

   Reported from *this* process's limit: doctor cannot see what a broker
   started under systemd would inherit, so the wording says "a broker started
   the same way as this check". The evaluator takes the limits as plain
   integers rather than the Mads::detail type that produces them, so this
   installed header stays free of src/detail/. */

CheckResult evaluate_fd_limit(bool supported, uint64_t soft, uint64_t hard,
                              std::optional<int64_t> configured);

CheckResult check_fd_limit(std::optional<int64_t> configured);

/* ---- 9. Local transport (ipc://) ------------------------------------------
   The broker binds ipc:// endpoints next to its tcp ones and advertises them
   through the `transport` settings command (src/detail/transport.hpp);
   agents on the broker's host switch to them automatically. These checks
   report what this host can do, what the broker offers, and what an agent
   started here would end up using. Facts are plain strings and booleans so
   this installed header stays free of src/detail/. */

CheckResult evaluate_ipc_support(bool zmq_has_ipc);

/// The per-user runtime directory holding ipc socket files and the
/// shared-memory registry (Mads::detail::runtime_dir()).
struct RuntimeDirFacts {
  std::string path;        // empty if it could not be determined
  bool usable = false;     // exists (created if needed) and owned by us
  bool owner_only = false; // no group/other permission bits (POSIX)
  std::string error;       // why it is not usable
};

RuntimeDirFacts probe_runtime_dir();
CheckResult evaluate_runtime_dir(const RuntimeDirFacts &facts);

/// What the broker answered to `transport`, plus what this host can see of
/// the endpoints it advertised.
struct BrokerTransportFacts {
  std::string uri;                // settings endpoint queried
  bool replied = false;           // anything came back in time
  bool supported = false;         // the reply was a transport JSON object
  std::string broker_host_token;  // as advertised by the broker
  std::string local_host_token;   // Mads::detail::host_token() here
  bool curve = false;             // the broker says CURVE is on
  bool local_has_ipc = false;     // zmq_has("ipc") in this process
  struct IpcEndpoint {
    std::string role;             // "frontend" | "backend" | "settings"
    std::string endpoint;         // "" when not offered
    bool file_exists = false;     // socket file visible from this host
  };
  std::vector<IpcEndpoint> endpoints; // always the three roles, in order
};

/// Parses a `transport` reply (nullopt = no reply at all) and inspects the
/// advertised socket files on this host's filesystem.
BrokerTransportFacts collect_broker_transport(
    const std::string &uri, const std::optional<std::string> &reply,
    const std::string &local_host_token, bool local_has_ipc);

/// Lists the advertised ipc endpoints and whether their socket files exist.
CheckResult evaluate_broker_ipc_endpoints(const BrokerTransportFacts &facts);

/**
 * @brief Predicts the transport an agent started on this host would select,
 * using the same rules as Agent::_select_transport() (src/agent.cpp): ipc
 * when the mode is not "tcp", this libzmq has ipc, the broker offers
 * frontend and backend ipc endpoints, the broker's host token matches ours
 * (skipped when settings are themselves fetched over ipc) and the socket
 * files are visible. The decision itself is the agent's own
 * (detail::choose_transport()). `mode` is the transport value resolved like
 * the agent does ([<agent>] transport, else [agents] transport, else
 * "auto"); an unknown value fails, like it does in the agent. `agent` names
 * the agent section the mode came from, or is empty for [agents].
 */
CheckResult evaluate_agent_transport(const BrokerTransportFacts &facts,
                                     const std::string &mode,
                                     const std::string &agent = "");

/* ---- 10. Shared-memory segment registry -----------------------------------
   POSIX producers register every shared-memory segment they create as
   <runtime_dir>/shm/<name>, holding the owner pid. A producer that crashes
   leaves its segment behind until reboot; Mads::shm::sweep_orphans() (and
   `mads doctor --fix`) unlinks those whose owner is dead. Windows frees a
   segment with its last handle and keeps no registry. */

struct ShmRegistryEntry {
  std::string name;        // segment name without the leading '/'
  int64_t owner_pid = -1;  // -1 if the file could not be read
  bool owner_alive = false;
};

/// Lists `dir` (the registry directory), mirroring sweep_orphans()'s own
/// selection: only `mads_*` files, and an unreadable/garbage pid is listed
/// with owner_pid = -1 and never counted as an orphan. `is_alive` decides
/// whether a pid is still running (injectable for tests). A missing
/// directory yields an empty list.
std::vector<ShmRegistryEntry>
scan_shm_registry(const std::filesystem::path &dir,
                  const std::function<bool(int64_t)> &is_alive);

/// True if process `pid` exists (POSIX kill(pid, 0); EPERM counts as alive,
/// matching sweep_orphans()). Always true on Windows.
bool pid_alive(int64_t pid);

/// `swept` is how many segments a --fix sweep just unlinked, if one ran.
CheckResult evaluate_shm_registry(const std::string &dir,
                                  const std::vector<ShmRegistryEntry> &entries,
                                  std::optional<size_t> swept = std::nullopt);

} // namespace Doctor
} // namespace Mads

#endif // MADS_DOCTOR_CHECKS_HPP
