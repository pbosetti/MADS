#include "doctor_checks.hpp"

#include "broker_probe.hpp"
#include "detail/endpoint.hpp"
#include "detail/fd_limit.hpp"
#include "detail/runtime_dir.hpp"
#include "detail/transport.hpp"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <toml++/toml.hpp>
#include <zmq.h>

#ifndef _WIN32
#include <cerrno>
#include <signal.h>
#endif

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

  if (detail::is_zmq_uri(target)) {
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

/* ---- 9. Local transport (ipc://) ------------------------------------------ */

CheckResult evaluate_ipc_support(bool zmq_has_ipc) {
  CheckResult r;
  r.name = "ipc_support";
  if (zmq_has_ipc) {
    r.status = Status::Pass;
    r.message = "This libzmq supports the ipc:// transport.";
    return r;
  }
  r.status = Status::Warn;
  r.message = "This libzmq has no ipc:// transport: agents on the broker's "
              "host fall back to tcp loopback.";
#ifdef _WIN32
  r.fix_hint = "On Windows, ipc needs a libzmq built against the Windows 10+ "
               "SDK (afunix.h); rebuild the vendored libzmq with it.";
#else
  r.fix_hint = "Rebuild libzmq with ipc enabled (it is on by default on "
               "Linux and macOS).";
#endif
  return r;
}

RuntimeDirFacts probe_runtime_dir() {
  RuntimeDirFacts f;
  try {
    const fs::path dir = Mads::detail::runtime_dir();
    f.path = dir.string();
    f.usable = true;
#ifdef _WIN32
    // %LOCALAPPDATA% is per-user by ACL; std::filesystem cannot see that.
    f.owner_only = true;
#else
    std::error_code ec;
    const auto perms = fs::status(dir, ec).permissions();
    f.owner_only =
        !ec && (perms & (fs::perms::group_all | fs::perms::others_all)) ==
                   fs::perms::none;
#endif
  } catch (const std::exception &e) {
    f.error = e.what();
  }
  return f;
}

CheckResult evaluate_runtime_dir(const RuntimeDirFacts &facts) {
  CheckResult r;
  r.name = "runtime_dir";
  if (!facts.usable) {
    r.status = Status::Fail;
    r.message = "Runtime directory unusable: " +
                (facts.error.empty() ? std::string("unknown error")
                                     : facts.error);
    r.fix_hint = "Remove or chown the directory named above (it must belong "
                 "to the user running MADS), or set XDG_RUNTIME_DIR.";
    return r;
  }
  if (!facts.owner_only) {
    r.status = Status::Warn;
    r.message = "Runtime directory " + facts.path +
                " is accessible to other users: its permissions are the only "
                "access control on ipc:// endpoints.";
    r.fix_hint = "chmod 700 " + facts.path;
    return r;
  }
  r.status = Status::Pass;
  r.message = "Runtime directory " + facts.path + " is owner-only.";
  return r;
}

BrokerTransportFacts collect_broker_transport(
    const std::string &uri, const std::optional<std::string> &reply,
    const std::string &local_host_token, bool local_has_ipc) {
  BrokerTransportFacts f;
  f.uri = uri;
  f.local_host_token = local_host_token;
  f.local_has_ipc = local_has_ipc;
  f.replied = reply.has_value();
  std::optional<detail::TransportInfo> info;
  if (reply)
    info = detail::parse_transport_reply(*reply);
  f.supported = info.has_value();
  if (info) {
    f.broker_host_token = info->host_token;
    f.curve = info->curve;
  }
  const std::pair<const char *, std::string> roles[] = {
      {"frontend", info ? info->frontend_ipc : std::string()},
      {"backend", info ? info->backend_ipc : std::string()},
      {"settings", info ? info->settings_ipc : std::string()}};
  for (const auto &[role, endpoint] : roles) {
    BrokerTransportFacts::IpcEndpoint ep;
    ep.role = role;
    ep.endpoint = endpoint;
    if (auto e = detail::parse_endpoint(endpoint); e && e->is_ipc()) {
      // symlink_status(): a Windows AF_UNIX socket file is a reparse point
      // that status() may fail to follow (same as the agent's own test).
      std::error_code ec;
      ep.file_exists = fs::exists(fs::symlink_status(e->path, ec));
    }
    f.endpoints.push_back(std::move(ep));
  }
  return f;
}

namespace {
bool offers_data_ipc(const BrokerTransportFacts &f) {
  int n = 0;
  for (const auto &ep : f.endpoints)
    if ((ep.role == "frontend" || ep.role == "backend") && !ep.endpoint.empty())
      ++n;
  return n == 2;
}
} // namespace

CheckResult evaluate_broker_ipc_endpoints(const BrokerTransportFacts &facts) {
  CheckResult r;
  r.name = "broker_ipc";
  if (!facts.replied) {
    r.status = Status::Warn;
    r.message = "No answer to the `transport` command from " + facts.uri +
                "; cannot tell which local transports the broker offers.";
    r.fix_hint = "Check the broker-reachable result above.";
    return r;
  }
  if (!facts.supported) {
    r.status = Status::Warn;
    r.message = "The broker at " + facts.uri +
                " predates the `transport` command: it offers tcp only.";
    r.fix_hint = "Upgrade the broker to use ipc for local agents.";
    return r;
  }
  bool any = false;
  std::vector<std::string> missing;
  std::ostringstream list;
  for (const auto &ep : facts.endpoints) {
    if (ep.endpoint.empty())
      continue;
    any = true;
    list << "\n         " << ep.role << ": " << ep.endpoint
         << (ep.file_exists ? "" : "  (socket file not found here)");
    if (!ep.file_exists)
      missing.push_back(ep.role);
  }
  if (!any) {
    r.status = Status::Pass;
    r.message = "The broker offers no ipc endpoints (ipc disabled, or its "
                "libzmq has no ipc): agents use tcp.";
    return r;
  }
  const bool same_host = facts.broker_host_token == facts.local_host_token;
  if (!missing.empty() && same_host) {
    r.status = Status::Warn;
    r.message = "The broker advertises ipc endpoints, but some socket files "
                "are missing on this host:" +
                list.str();
    r.fix_hint = "Another process may have removed them, or the broker runs "
                 "in a container with its own filesystem; restart the broker "
                 "or set ipc_dir to a shared directory.";
    return r;
  }
  r.status = Status::Pass;
  r.message = std::string("The broker advertises ipc endpoints") +
              (same_host ? "" : " (on another host)") + ":" + list.str();
  return r;
}

CheckResult evaluate_agent_transport(const BrokerTransportFacts &facts,
                                     const std::string &mode) {
  CheckResult r;
  r.name = "agent_transport";
  if (mode != "auto" && mode != "tcp" && mode != "ipc") {
    r.status = Status::Fail;
    r.message = "Invalid [agents] transport = \"" + mode +
                "\": agents will refuse to start.";
    r.fix_hint = "Use \"auto\", \"tcp\" or \"ipc\".";
    return r;
  }
  if (mode == "tcp") {
    r.status = Status::Pass;
    r.message = "Agents on this host use tcp (transport = \"tcp\").";
    return r;
  }

  const bool settings_over_ipc =
      facts.uri.rfind("ipc://", 0) == 0;
  std::string why_not;
  if (!facts.local_has_ipc) {
    why_not = "this libzmq has no ipc transport";
  } else if (!facts.replied) {
    why_not = "the broker did not answer";
  } else if (!facts.supported) {
    why_not = "the broker predates the transport command";
  } else if (!offers_data_ipc(facts)) {
    why_not = "the broker offers no ipc endpoints";
  } else if (!settings_over_ipc &&
             facts.broker_host_token != facts.local_host_token) {
    why_not = "the broker runs on another host";
  } else {
    for (const auto &ep : facts.endpoints) {
      if ((ep.role == "frontend" || ep.role == "backend") && !ep.file_exists) {
        why_not = "the ipc socket " + ep.endpoint + " is not reachable here";
        break;
      }
    }
  }

  if (why_not.empty()) {
    r.status = Status::Pass;
    r.message = "Agents on this host would use ipc (transport = \"" + mode +
                "\").";
    return r;
  }
  if (mode == "ipc" || settings_over_ipc) {
    r.status = Status::Fail;
    r.message = "Agents on this host would refuse to start: " +
                std::string(mode == "ipc" ? "transport = \"ipc\"" :
                                            "settings fetched over ipc") +
                ", but " + why_not + ".";
    r.fix_hint = "Set transport = \"auto\" (or \"tcp\") in [agents], or use a "
                 "tcp:// settings URI.";
    return r;
  }
  r.status = Status::Pass;
  r.message = "Agents on this host would use tcp: " + why_not + ".";
  return r;
}

/* ---- 10. Shared-memory segment registry ----------------------------------- */

bool pid_alive(int64_t pid) {
#ifdef _WIN32
  (void)pid;
  return true;
#else
  if (pid <= 0)
    return true;
  return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
#endif
}

std::vector<ShmRegistryEntry>
scan_shm_registry(const fs::path &dir,
                  const std::function<bool(int64_t)> &is_alive) {
  std::vector<ShmRegistryEntry> out;
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(dir, ec)) {
    std::string name = entry.path().filename().string();
    if (name.rfind("mads_", 0) != 0)
      continue;
    ShmRegistryEntry e;
    e.name = std::move(name);
    std::ifstream f(entry.path());
    long long pid = 0;
    if (f >> pid && pid > 0) {
      e.owner_pid = pid;
      e.owner_alive = is_alive(pid);
    }
    out.push_back(std::move(e));
  }
  std::sort(out.begin(), out.end(),
            [](const auto &a, const auto &b) { return a.name < b.name; });
  return out;
}

CheckResult evaluate_shm_registry(const std::string &dir,
                                  const std::vector<ShmRegistryEntry> &entries,
                                  std::optional<size_t> swept) {
  CheckResult r;
  r.name = "shm_registry";
  size_t live = 0, orphans = 0, unknown = 0;
  for (const auto &e : entries) {
    if (e.owner_pid < 0)
      ++unknown;
    else if (e.owner_alive)
      ++live;
    else
      ++orphans;
  }
  std::ostringstream ss;
  ss << "Shared-memory registry " << dir << ": " << live << " live segment(s)";
  if (orphans)
    ss << ", " << orphans << " orphan(s) (owner dead)";
  if (unknown)
    ss << ", " << unknown << " unreadable entr" << (unknown == 1 ? "y" : "ies");
  if (swept)
    ss << "; swept " << *swept << " orphan(s)";
  ss << ".";
  r.message = ss.str();
  if (orphans) {
    r.status = Status::Warn;
    r.fix_hint = "Run `mads doctor --fix` to unlink orphaned segments (a "
                 "producer crashed without cleaning up; they hold memory "
                 "until reboot).";
    r.fixable = true;
    return r;
  }
  r.status = Status::Pass;
  return r;
}

} // namespace Doctor
} // namespace Mads
