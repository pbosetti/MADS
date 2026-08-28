#include "doctor_checks.hpp"

#include "broker_probe.hpp"
#include "detail/fd_limit.hpp"

#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <toml++/toml.hpp>
#include <zmq.h>

namespace Mads {
namespace Doctor {

namespace {
namespace fs = std::filesystem;
using json = nlohmann::json;

std::string strip_trailing_cr(std::string s) {
  if (!s.empty() && s.back() == '\r') {
    s.pop_back();
  }
  return s;
}
} // namespace

/* ---- 1. Settings file found and parses ---------------------------------- */

CheckResult check_settings_file(const fs::path &path) {
  CheckResult r;
  r.name = "settings_file";
  const std::string target = path.string();

  if (target.rfind("tcp://", 0) == 0) {
    r.status = Status::Pass;
    r.message = "Settings source '" + target +
               "' is a remote broker URI; local file check skipped "
               "(see the broker-reachable check).";
    return r;
  }

  if (!fs::exists(path)) {
    r.status = Status::Fail;
    r.message = "Settings file not found: " + target;
    r.fix_hint = "Run `mads doctor --fix` to scaffold a default " + target +
                ", or `mads ini -o " + target + "`.";
    r.fixable = true;
    return r;
  }

  try {
    auto table = toml::parse_file(target);
    (void)table;
    r.status = Status::Pass;
    r.message = "Settings file '" + target + "' parses as valid TOML.";
  } catch (const toml::parse_error &e) {
    std::ostringstream ss;
    ss << "TOML parse error in '" << target << "': " << e.description()
       << " (" << e.source().begin << ")";
    r.status = Status::Fail;
    r.message = ss.str();
    r.fix_hint = "Fix the syntax error reported above.";
  } catch (const std::exception &e) {
    r.status = Status::Fail;
    r.message =
        std::string("Failed to read settings file '") + target + "': " + e.what();
  }
  return r;
}

/* ---- 2. Broker reachable -------------------------------------------------- */

CheckResult evaluate_broker_reachable(const std::string &uri, bool reachable,
                                      bool crypto) {
  CheckResult r;
  r.name = "broker_reachable";
  const std::string how = crypto ? " (CURVE)" : "";
  if (reachable) {
    r.status = Status::Pass;
    r.message = "Broker settings endpoint " + uri + how + " is reachable.";
  } else {
    r.status = Status::Fail;
    r.message = "Broker settings endpoint " + uri + how + " did not respond.";
    // The encryption mode has to match on both ends, and a mismatch looks
    // exactly like a broker that is down -- so name it in the hint rather
    // than sending the user off to check an address that was never wrong.
    r.fix_hint =
        crypto ? "Start the broker (`mads broker --crypto`), check that its "
                 "--keys_dir/--key_broker name the same keys as this probe, "
                 "or drop --crypto here if the broker runs unencrypted."
               : "Start the broker (`mads broker`), check the [broker] "
                 "address/port in your settings file, or pass --crypto here "
                 "if the broker runs with CURVE encryption.";
  }
  return r;
}

CheckResult check_broker_reachable(const std::string &uri,
                                   std::chrono::milliseconds timeout,
                                   const std::optional<CurveKeyCheck> &curve) {
  return evaluate_broker_reachable(uri, probe_broker(uri, timeout, curve),
                                   curve.has_value());
}

/* ---- 3. Declared plugin(s) resolve and load ------------------------------ */

CheckResult evaluate_plugin_load(const PluginLoadFacts &facts) {
  CheckResult r;
  r.name = "plugin_load:" + facts.plugin_file;

  if (!facts.file_exists) {
    r.status = Status::Fail;
    r.message = "Plugin file not found: " + facts.plugin_file;
    r.fix_hint =
        "Check the 'attachment' path in your settings file, or build/copy "
        "the plugin into place.";
    return r;
  }

  if (!facts.loaded) {
    r.status = Status::Fail;
    r.message = "Plugin '" + facts.plugin_file + "' failed to load" +
               (facts.error.empty() ? "." : (": " + facts.error));
    r.fix_hint =
        "Rebuild the plugin against the current mads_plugin/pugg version "
        "(see `mads plugin --update`), or check its dependencies with "
        "`mads inspect_plugin`.";
    return r;
  }

  r.status = Status::Pass;
  r.message = "Plugin '" + facts.plugin_file + "' loaded as " +
             (facts.driver_kind.empty() ? "?" : facts.driver_kind) + "/" +
             facts.driver_name + " (protocol v" +
             std::to_string(facts.protocol_version) + ").";
  return r;
}

/* ---- 4. Plugin protocol matches the pinned mads_plugin version ---------- */

std::optional<int> parse_pinned_plugin_protocol(const std::string &manifest_json_text) {
  try {
    const json parsed = json::parse(manifest_json_text);
    if (parsed.contains("plugin_protocol") &&
        parsed["plugin_protocol"].is_number_integer()) {
      return parsed["plugin_protocol"].get<int>();
    }
  } catch (const std::exception &) {
    // fall through to nullopt
  }
  return std::nullopt;
}

CheckResult evaluate_plugin_protocol(int loaded_version,
                                     std::optional<int> pinned_version,
                                     int min_supported) {
  CheckResult r;
  r.name = "plugin_protocol";

  if (loaded_version < 0) {
    r.status = Status::Warn;
    r.message = "Plugin protocol unknown (the plugin did not load; see the "
               "plugin-load check).";
    return r;
  }

  if (loaded_version < min_supported) {
    r.status = Status::Fail;
    r.message = "Plugin protocol v" + std::to_string(loaded_version) +
               " is older than the minimum supported v" +
               std::to_string(min_supported) + ".";
    r.fix_hint = "Run `mads plugin --update` on the plugin's source project, "
                "or rebuild it against a newer mads_plugin.";
    return r;
  }

  if (!pinned_version.has_value()) {
    r.status = Status::Warn;
    r.message = "Plugin protocol v" + std::to_string(loaded_version) +
               " (could not read the pinned protocol from "
               "share/plugin_deps.json to compare).";
    return r;
  }

  if (loaded_version != *pinned_version) {
    r.status = Status::Warn;
    r.message = "Plugin protocol v" + std::to_string(loaded_version) +
               " differs from this MADS installation's pinned v" +
               std::to_string(*pinned_version) + ".";
    r.fix_hint = "Run `mads plugin --update` on the plugin's source project "
                "to migrate it to the pinned protocol.";
    return r;
  }

  r.status = Status::Pass;
  r.message = "Plugin protocol v" + std::to_string(loaded_version) +
             " matches the pinned version.";
  return r;
}

/* ---- 5. CURVE key files exist and are well-formed ------------------------ */

bool is_well_formed_curve_key(const std::string &key_line) {
  if (key_line.size() != 40) {
    return false;
  }
  uint8_t decoded[32];
  return zmq_z85_decode(decoded, key_line.c_str()) != nullptr;
}

CheckResult check_curve_keys(const CurveKeyCheck &cfg) {
  CheckResult r;
  r.name = "curve_keys";

  if (!fs::exists(cfg.key_dir) || !fs::is_directory(cfg.key_dir)) {
    r.status = Status::Fail;
    r.message = "CURVE key directory does not exist: " + cfg.key_dir.string();
    r.fix_hint = "Create the directory and generate a keypair with `mads "
                "--keypair`, or point --keys_dir at the right location.";
    return r;
  }

  struct Need {
    fs::path path;
    const char *label;
  };
  const std::vector<Need> needed = {
      {cfg.key_dir / (cfg.client_key_name + ".key"), "client secret key"},
      {cfg.key_dir / (cfg.client_key_name + ".pub"), "client public key"},
      {cfg.key_dir / (cfg.server_key_name + ".pub"),
       "server/broker public key"},
  };

  std::vector<std::string> problems;
  for (const auto &need : needed) {
    if (!fs::exists(need.path)) {
      problems.push_back(std::string(need.label) + " missing (" +
                         need.path.string() + ")");
      continue;
    }
    std::ifstream file(need.path);
    std::string line;
    std::getline(file, line);
    line = strip_trailing_cr(line);
    if (!is_well_formed_curve_key(line)) {
      problems.push_back(std::string(need.label) +
                         " is not a well-formed Z85 CURVE key (" +
                         need.path.string() + ")");
    }
  }

  if (!problems.empty()) {
    std::ostringstream ss;
    ss << "CURVE key problem(s) in " << cfg.key_dir.string() << ": ";
    for (std::size_t i = 0; i < problems.size(); ++i) {
      if (i > 0) {
        ss << "; ";
      }
      ss << problems[i];
    }
    r.status = Status::Fail;
    r.message = ss.str();
    r.fix_hint = "Regenerate the keypair(s) with `mads --keypair <name>` "
                "and place them in " +
                cfg.key_dir.string() + ".";
    return r;
  }

  r.status = Status::Pass;
  r.message =
      "CURVE key files in " + cfg.key_dir.string() + " exist and are well-formed.";
  return r;
}

/* ---- 6. Local port-availability sanity check ------------------------------ */

CheckResult evaluate_port_available(const std::string &host, int port, bool in_use) {
  CheckResult r;
  r.name = "port:" + std::to_string(port);
  const std::string host_port =
      (host.empty() ? std::string("127.0.0.1") : host) + ":" + std::to_string(port);

  if (in_use) {
    r.status = Status::Fail;
    r.message = "Port " + host_port + " is already in use.";
    r.fix_hint = "Stop whatever is already bound to " + host_port +
                " (perhaps an already-running broker), or change the port "
                "in your settings file.";
  } else {
    r.status = Status::Pass;
    r.message = "Port " + host_port + " is free.";
  }
  return r;
}

CheckResult check_port_available(const std::string &host, int port,
                                 std::chrono::milliseconds timeout) {
  return evaluate_port_available(host, port, probe_tcp_port(host, port, timeout));
}

/* ---- 7. CURVE handshake actually succeeds ---------------------------------
   Check 5 (check_curve_keys) only proves the key *files* are well-formed;
   it says nothing about whether the broker will actually accept them. The
   live probe is Mads::probe_curve_handshake() (src/broker_probe.hpp, driven
   by a Mads::SocketMonitor -- ZMQ_DEVELOPMENT.md §2.1); this evaluator turns
   its three-way outcome into a CheckResult, split out the same way check 2's
   evaluate_broker_reachable() is. */

CheckResult evaluate_curve_handshake(const std::string &uri,
                                     CurveProbeResult result) {
  CheckResult r;
  r.name = "curve_handshake";
  switch (result) {
  case CurveProbeResult::Connected:
    r.status = Status::Pass;
    r.message = "CURVE handshake with " + uri + " succeeded.";
    break;
  case CurveProbeResult::RejectedAuth:
    r.status = Status::Fail;
    r.message = "The broker at " + uri +
               " rejected this client's CURVE key (ZAP handshake failure).";
    r.fix_hint = "Confirm this client's public key is in the broker's key "
                "directory, and that --keys_dir/--key_client/--key_broker "
                "name the same files on both sides.";
    break;
  case CurveProbeResult::Timeout:
    r.status = Status::Fail;
    r.message = "No response from " + uri +
               " within the timeout -- broker unreachable, not listening "
               "with CURVE, or an older libzmq that predates the "
               "handshake-failure event.";
    r.fix_hint = "Start the broker with --crypto, or check the [broker] "
                "address/port in your settings file.";
    break;
  }
  return r;
}

CheckResult check_curve_handshake(const std::string &uri, const CurveKeyCheck &cfg,
                                  std::chrono::milliseconds timeout) {
  return evaluate_curve_handshake(
      uri, probe_curve_handshake(uri, cfg.key_dir, cfg.client_key_name,
                                 cfg.server_key_name, timeout));
}

/* ---- 8. Open-file limit --------------------------------------------------- */

CheckResult evaluate_fd_limit(bool supported, uint64_t soft, uint64_t hard,
                              std::optional<int64_t> configured) {
  CheckResult r;
  r.name = "Open-file limit";

  if (!supported) {
    r.status = Status::Pass;
    r.message = "This platform has no per-process descriptor limit for "
                "sockets, so fleet size is not bounded by one.";
    return r;
  }

  const uint64_t capacity = Mads::detail::agent_capacity(soft);
  const std::string room = std::to_string(soft) + " descriptors, room for " +
                           "about " + std::to_string(capacity) +
                           " connected agents";

  // A request the hard limit cannot satisfy is the one case where the settings
  // file is actively misleading: it looks configured, but the broker will
  // silently get less than it asked for.
  if (configured.has_value() && *configured > 0 &&
      static_cast<uint64_t>(*configured) > hard) {
    r.status = Status::Warn;
    r.message = "max_open_files = " + std::to_string(*configured) +
                " exceeds this process's hard limit of " +
                std::to_string(hard) + ", so the broker will get " + room + ".";
    r.fix_hint = "Raise the hard limit with LimitNOFILE= in the systemd unit "
                 "(or `ulimit -Hn` as root); max_open_files cannot go above it.";
    return r;
  }

  if (soft > Mads::detail::FD_LOW_WATERMARK) {
    r.status = Status::Pass;
    r.message = "A broker started the same way as this check would have " +
                room + ".";
    return r;
  }

  r.status = Status::Warn;
  r.message = "A broker started the same way as this check would have only " +
              room + ". Every connected agent costs two descriptors, and "
              "libzmq refuses the ones past the limit almost silently.";
  r.fix_hint = hard > soft
                   ? "Set `max_open_files` in the [broker] section of the "
                     "settings file (up to the hard limit of " +
                         std::to_string(hard) +
                         "), or LimitNOFILE= in the systemd unit."
                   : "Raise the hard limit with LimitNOFILE= in the systemd "
                     "unit, or `ulimit -Hn` as root -- the soft limit is "
                     "already at it.";
  return r;
}

CheckResult check_fd_limit(std::optional<int64_t> configured) {
  const auto limits = Mads::detail::query_fd_limits();
  return evaluate_fd_limit(limits.supported, limits.soft, limits.hard,
                           configured);
}

} // namespace Doctor
} // namespace Mads
