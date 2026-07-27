/*
  ____  _               _
 |  _ \(_)_ __ ___  ___| |_ ___  _ __
 | | | | | '__/ _ \/ __| __/ _ \| '__|
 | |_| | | | |  __/ (__| || (_) | |
 |____/|_|_|  \___|\___|\__\___/|_|
   ____             __ _
  / ___|___  _ __  / _(_) __ _
 | |   / _ \| '_ \| |_| |/ _` |
 | |__| (_) | | | |  _| | (_| |
  \____\___/|_| |_|_| |_|\__, |
                         |___/

Pure config module for `mads up` (src/main/up.cpp): parses `director.toml` (the
config format owned by https://github.com/mads-net/mads_director), validates
it, expands `scale`/`${PWD}`/`${ID}` templating, and orders processes so that
every dependency (`after`) starts before its dependents.

No ZMQ, no process spawning, no `Mads::Agent` dependency here on purpose (see
the "Parser ownership" decision in NEW_FEATURES.md's P5 section): this file is
meant to be liftable wholesale into a future shared header, or contributed
upstream to mads_director.

Author(s): Paolo Bosetti
*/
#ifndef MADS_DIRECTOR_CONFIG_HPP
#define MADS_DIRECTOR_CONFIG_HPP

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Mads {

/**
 * @brief Parses a duration string like "500ms", "2s", "1.5s", "3m" into a
 * `std::chrono::milliseconds` value. A bare number (no unit) is interpreted
 * as seconds. Returns std::nullopt on malformed input (empty string,
 * non-numeric magnitude, unknown unit, or a negative value).
 *
 * Recognized units: "ms" (milliseconds), "s" (seconds), "m" (minutes).
 * Used both by `ready = "delay:<dur>"` and by mads-up's CLI duration flags
 * (--timeout, --grace), so it lives here rather than duplicated in up.cpp.
 */
std::optional<std::chrono::milliseconds> parse_duration(const std::string &text);

/// Discriminates the four `ready = "..."` probe kinds.
enum class ReadyKind { Broker, Port, Log, Delay };

/**
 * @brief A parsed, validated `ready = "..."` value.
 *
 * Grammar (see NEW_FEATURES.md P5): `"broker"` | `"broker:<uri>"` |
 * `"port:<n>"` | `"log:<regex>"` | `"delay:<dur>"`. This key is additive and
 * silently ignored by Director's own parser (it never validates unknown keys
 * inside a process table -- confirmed against mads_director v2.2.0's
 * src/config.cpp, see the comment at the top of director_config.cpp).
 */
struct ReadySpec {
  ReadyKind kind = ReadyKind::Delay;
  std::string broker_uri;   // ReadyKind::Broker (defaulted if not given)
  int port = 0;             // ReadyKind::Port
  std::string log_pattern;  // ReadyKind::Log (ECMAScript regex, matched
                             // against each captured stdout/stderr line)
  std::chrono::milliseconds delay{0}; // ReadyKind::Delay
};

/// Default broker settings URI probed by a bare `ready = "broker"`.
constexpr const char *kDefaultBrokerProbeUri = "tcp://localhost:9092";

/**
 * @brief Parses a raw `ready` string into a ReadySpec. Exposed standalone
 * (rather than only reachable through load_director_config()) so it has its
 * own focused unit tests.
 */
std::optional<ReadySpec> parse_ready_spec(const std::string &value,
                                          std::string *out_error);

/**
 * @brief One process instance, after `scale` expansion and template
 * substitution. This is what `mads up` actually starts: one ProcessConfig
 * per launched OS process.
 */
struct ProcessConfig {
  std::string name;      // instance name, e.g. "api" or "api[2]" when scale>1
  std::string base_name; // section name before scale expansion, e.g. "api"
  std::string command;   // fully expanded (${PWD}/${ID} substituted)
  std::string workdir;   // absolute, resolved against the config file's dir
  bool enabled = true;
  bool relaunch = false;
  bool tty = false; // parsed for fidelity; headless mads-up ignores it
  int instance_id = 0; // 0-based, matches Director's ${ID} substitution
  // Resolved dependency instance names (a base process with scale>1 that
  // another depends on via `after` expands to *all* of its instances, exactly
  // as mads_director's ProcessManager::build_process_definitions does).
  std::vector<std::string> after;
  std::optional<ReadySpec> ready;
};

/**
 * @brief A fully parsed, validated, expanded `director.toml`. `processes` is
 * already in a valid topological start order (every entry's `after` names
 * appear earlier in the vector), so `mads up` can just iterate it in order.
 */
struct DirectorConfig {
  std::optional<std::string> terminal;    // GUI-only, ignored headless
  double sample_rate_seconds = 2.0;       // GUI-only, ignored headless
  std::filesystem::path base_dir;         // directory containing the config
  std::vector<ProcessConfig> processes;   // topologically ordered
};

/**
 * @brief Loads, validates, expands and orders a director.toml file.
 * @param path path to the director.toml file.
 * @param out_error set to a human-readable message on failure (malformed
 * TOML, missing/invalid `command`, unknown `after` target, dependency cycle,
 * invalid `scale`, duplicate process name, invalid `ready` value, etc).
 * @param out_warnings if non-null, appended with one human-readable message
 * per ignored unknown key/section. This module never writes to stdout/stderr
 * itself (it stays a pure function of its input); the caller (mads-up)
 * decides how to display these.
 * @return the config on success; std::nullopt on failure.
 *
 * Unknown top-level keys/sections/tables and unknown keys inside a process
 * table are ignored, never fatal -- a deliberate design choice (see
 * NEW_FEATURES.md's P5 "Parser ownership" decision), not a fidelity match to
 * Director's own parser (which is stricter about non-table top-level
 * entries; see the comment at the top of director_config.cpp).
 */
std::optional<DirectorConfig>
load_director_config(const std::string &path, std::string *out_error,
                     std::vector<std::string> *out_warnings = nullptr);

} // namespace Mads

#endif // MADS_DIRECTOR_CONFIG_HPP
