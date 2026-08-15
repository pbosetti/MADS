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
#include <filesystem>
#include <optional>
#include <string>

namespace Mads {
namespace Doctor {

/// Outcome of a single check, mirroring the three-state model of
/// `ros2 doctor`/`brew doctor`: Pass (nothing to do), Warn (works but worth a
/// look), Fail (needs fixing before things will work).
enum class Status { Pass, Warn, Fail };

/// One check's result: a status, a human-readable message, and (when
/// actionable) a one-line fix hint. `fixable` marks the small set of checks
/// `--fix` knows how to safely auto-remediate (currently: a missing settings
/// file).
struct CheckResult {
  std::string name;
  Status status = Status::Pass;
  std::string message;
  std::string fix_hint;
  bool fixable = false;
};

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
   testable without a real socket. */

CheckResult evaluate_broker_reachable(const std::string &uri, bool reachable);

CheckResult check_broker_reachable(const std::string &uri,
                                   std::chrono::milliseconds timeout);

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

/* ---- 5. CURVE key files exist and are well-formed ------------------------ */

/// Mirrors AgentAppT::CryptoOptions (src/agent_app.hpp): the same
/// key_dir/client/server naming convention every mads-* executable's
/// --crypto/--keys_dir/--key_client/--key_broker flags configure.
struct CurveKeyCheck {
  std::filesystem::path key_dir;
  std::string client_key_name = "client";
  std::string server_key_name = "broker";
};

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

} // namespace Doctor
} // namespace Mads

#endif // MADS_DOCTOR_CHECKS_HPP
