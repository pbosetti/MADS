/*
  ____             _
 |  _ \  ___   ___| |_ ___  _ __
 | | | |/ _ \ / __| __/ _ \| '__|
 | |_| | (_) | (__| || (_) | |
 |____/ \___/ \___|\__\___/|_|

`mads doctor`: a single-command environment/connectivity health check, MADS's
answer to `ros2 doctor`/`brew doctor`. Each check reports pass/warn/fail with
a one-line fix hint. The pass/warn/fail *decisions* live in
src/doctor_checks.hpp/.cpp (pure, unit-tested without a live broker or a real
.plugin); this file stays thin: CLI parsing, gathering the facts each check
needs (reading the settings file, probing the broker/ports, dry-run loading
declared plugins via pugg -- the same mechanics as
src/main/plugin_loader.cpp/mads-source/-filter/-sink), and printing.

`--plan <director.toml>` delegates to the exact same parse/validate/expand
pipeline `mads up --dry-run` uses (src/director_config.hpp, from P5): no
process spawning, no reproc/up_supervisor dependency, just the pure
config-loading library so this one command can sanity-check a whole
deployment before it is launched for real.

Author(s): Paolo Bosetti
*/
#include "../broker_probe.hpp"
#include "../director_config.hpp"
#include "../doctor_checks.hpp"
#include "../exec_path.hpp"
#include "../mads.hpp"
#include "../topology_graph.hpp"
#include "plugin_migrate.hpp"

#include <cxxopts.hpp>
#include <filter.hpp>
#include <inja/inja.hpp>
#include <nlohmann/json.hpp>
#include <pugg/Kernel.h>
#include <rang.hpp>
#include <sink.hpp>
#include <source.hpp>
#include <toml++/toml.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace std;
using namespace cxxopts;
using namespace rang;
using namespace Mads;
using json = nlohmann::json;
namespace fs = std::filesystem;
using Mads::Doctor::CheckResult;
using Mads::Doctor::Status;

namespace {

// Doctor's own default target differs from every other mads-* executable's
// `-s/--settings` (which defaults to a broker URI, SETTINGS_URI, because a
// running agent normally fetches settings from a live broker). Doctor's
// check 1 is specifically "the local settings file is found and parses", so
// it defaults to a local path in the current directory instead, mirroring
// `mads up`'s `-f/--file` default of "director.toml".
constexpr const char *kDefaultSettingsPath = "mads.ini";
constexpr const char *kDefaultBrokerUri = "tcp://localhost:9092";

int overall_exit_code = 0;

void print_result(const CheckResult &r) {
  switch (r.status) {
  case Status::Pass:
    cout << fg::green << "  [PASS] " << fg::reset;
    break;
  case Status::Warn:
    cout << fg::yellow << "  [WARN] " << fg::reset;
    break;
  case Status::Fail:
    cout << fg::red << "  [FAIL] " << fg::reset;
    overall_exit_code = 1;
    break;
  }
  cout << r.message << endl;
  if (!r.fix_hint.empty() && r.status != Status::Pass) {
    cout << "         " << style::italic << fg::gray << "-> " << r.fix_hint
        << fg::reset << style::reset << endl;
  }
}

// Swaps a broker bind wildcard ("tcp://*:9092") for a connectable host
// ("tcp://localhost:9092"), mirroring Mads::kDefaultBrokerProbeUri's own
// intent (director_config.hpp) for the common `bind = "*"` case. Leaves any
// other host untouched.
string to_probe_uri(const string &bind_uri) {
  const string wildcard = "tcp://*:";
  if (bind_uri.rfind(wildcard, 0) == 0) {
    return "tcp://localhost:" + bind_uri.substr(wildcard.size());
  }
  return bind_uri;
}

// Extracts the numeric port from a "scheme://host:port" address, exactly
// like src/main/broker.cpp itself does when binding.
optional<int> port_of(const string &address) {
  auto pos = address.find_last_of(':');
  if (pos == string::npos) {
    return nullopt;
  }
  try {
    return stoi(address.substr(pos + 1));
  } catch (const exception &) {
    return nullopt;
  }
}

// Resolves an `attachment` key from the settings file, exactly like
// src/main/broker.cpp does when it serves one to a remote agent
// (broker.cpp:466-467): relative to the executable directory, not the
// current working directory.
fs::path resolve_attachment_path(const string &raw) {
  fs::path p(raw);
  if (p.is_relative()) {
    return fs::path(Mads::exec_dir(raw));
  }
  return p;
}

// Resolves a `--plugin` CLI argument exactly like plugin_loader.cpp resolves
// its own positional `plugin` argument: relative to the current working
// directory first (so `mads doctor --plugin ./my.plugin` behaves like any
// other CLI tool), falling back to the installed-plugin location
// (<exec_dir>/../lib on POSIX, <exec_dir>/../bin on Windows) only if that
// path does not exist.
fs::path resolve_cli_plugin_path(const string &raw) {
  fs::path p(raw);
  if (fs::exists(p)) {
    return p;
  }
#ifdef _WIN32
  fs::path installed = Mads::exec_dir("../bin/" + raw);
#else
  fs::path installed = Mads::exec_dir("../lib/" + raw);
#endif
  return installed;
}

// The actual pugg::Kernel dry-run load: load the shared library, look up
// whichever of Source/Filter/Sink it registered a driver for, create() one
// instance (to read its own kind()), then let the Kernel go out of scope
// (unloading the library) without ever calling process()/get_output()/
// load_data(). Mirrors src/main/plugin_loader.cpp's loading sequence, just
// generalized across all three plugin kinds since doctor does not know
// ahead of time which one a given `attachment`/`--plugin` file is.
//
// `driver_name` is the key to look the driver up under, mirroring
// plugin_loader.cpp's own precedence: the section's `driver` setting if one
// was declared, else the file stem (see the caller below).
Mads::Doctor::PluginLoadFacts probe_plugin_load(const fs::path &plugin_file,
                                                 const string &driver_name) {
  Mads::Doctor::PluginLoadFacts facts;
  facts.plugin_file = plugin_file.string();
  facts.file_exists = fs::exists(plugin_file);
  if (!facts.file_exists) {
    facts.error = "file not found";
    return facts;
  }

  pugg::Kernel kernel;
  kernel.add_server<Source<json>>();
  kernel.add_server<Filter<json, json>>();
  kernel.add_server<Sink<json>>();

  if (!kernel.load_plugin(plugin_file.string())) {
    facts.error = kernel.last_error();
    if (facts.error.empty()) {
      facts.error = "cannot load plugin file";
    }
    return facts;
  }

  if (auto *driver = kernel.get_driver<SourceDriver<json>>(
          Source<json>::server_name(), driver_name)) {
    auto instance = driver->create();
    facts.driver_kind = "source";
    facts.driver_name = instance->kind();
    facts.protocol_version = driver->version();
    facts.loaded = true;
  } else if (auto *fdriver = kernel.get_driver<FilterDriver<json, json>>(
                 Filter<json, json>::server_name(), driver_name)) {
    auto instance = fdriver->create();
    facts.driver_kind = "filter";
    facts.driver_name = instance->kind();
    facts.protocol_version = fdriver->version();
    facts.loaded = true;
  } else if (auto *sdriver = kernel.get_driver<SinkDriver<json>>(
                 Sink<json>::server_name(), driver_name)) {
    auto instance = sdriver->create();
    facts.driver_kind = "sink";
    facts.driver_name = instance->kind();
    facts.protocol_version = sdriver->version();
    facts.loaded = true;
  } else {
    facts.error = "no Source/Filter/Sink driver named '" + driver_name +
                  "' found in this plugin file (looked for a 'driver' "
                  "setting or file-stem match, same convention as "
                  "mads-source/-filter/-sink)";
  }

  kernel.clear_drivers();
  return facts;
}

// --plan <director.toml>: reuse P5's pure config module directly and print
// the same kind of expanded-plan report `mads up --dry-run` does (see
// src/main/up.cpp's print_plan()/describe_ready(), duplicated here in
// miniature since those are file-local to up.cpp and this is presentation
// only -- all the actual parsing/validation/expansion is P5's code, not
// reimplemented).
string describe_ready(const ProcessConfig &p) {
  if (!p.ready.has_value()) {
    return "-";
  }
  const ReadySpec &r = *p.ready;
  switch (r.kind) {
  case ReadyKind::Broker:
    return "broker:" + r.broker_uri;
  case ReadyKind::Port:
    return "port:" + to_string(r.port);
  case ReadyKind::Log:
    return "log:" + r.log_pattern;
  case ReadyKind::Delay:
    return "delay:" +
          to_string(chrono::duration_cast<chrono::milliseconds>(r.delay).count()) +
          "ms";
  }
  return "-";
}

int run_plan_check(const string &path) {
  string load_error;
  vector<string> warnings;
  auto config = load_director_config(path, &load_error, &warnings);

  cout << style::bold << "mads doctor --plan " << path << style::reset << endl;
  for (const auto &w : warnings) {
    cout << fg::yellow << "  [WARN] " << w << fg::reset << endl;
  }
  if (!config.has_value()) {
    cout << fg::red << "  [FAIL] " << load_error << fg::reset << endl;
    return 1;
  }

  cout << fg::green << "  [PASS] '" << path << "' parses and expands to "
      << config->processes.size() << " process instance(s)." << fg::reset
      << endl
      << endl;
  cout << style::bold << "Expanded plan (start order):" << style::reset
      << endl;
  for (const auto &p : config->processes) {
    cout << "  " << style::bold << p.name << style::reset;
    if (!p.enabled) {
      cout << fg::yellow << " [disabled]" << fg::reset;
    }
    if (p.relaunch) {
      cout << fg::cyan << " [relaunch]" << fg::reset;
    }
    cout << "\n";
    cout << "    command : " << p.command << "\n";
    cout << "    workdir : " << p.workdir << "\n";
    if (!p.after.empty()) {
      cout << "    after   : ";
      for (size_t i = 0; i < p.after.size(); ++i) {
        cout << p.after[i] << (i + 1 < p.after.size() ? ", " : "");
      }
      cout << "\n";
    }
    if (p.ready.has_value()) {
      cout << "    ready   : " << describe_ready(p) << "\n";
    }
  }
  return 0;
}

// --fix: scaffold a missing default settings file from the same inja
// template `mads ini` renders (src/main/mads.cpp's make_ini()). Only ever
// called when check_settings_file() already reported the file missing, and
// only ever *creates*: an existing file is never touched.
bool fix_missing_settings_file(const fs::path &path) {
  if (fs::exists(path)) {
    return false; // never overwrite
  }
  const string template_dir = Mads::exec_dir("../share/templates/");
  json data;
  data["broker"] = "localhost";
  data["port_frontend"] = "9090";
  data["port_backend"] = "9091";
  data["port_settings"] = "9092";
  data["mongo_uri"] = "mongodb://localhost:27017";

  try {
    fs::path out_path = path.is_relative() ? fs::absolute(path) : path;
    auto parent = out_path.parent_path();
    if (!parent.empty() && !fs::exists(parent)) {
      cerr << fg::red << "Path " << parent << " does not exist" << fg::reset
          << endl;
      return false;
    }
    auto tmp = fs::temp_directory_path();
    inja::Environment env{template_dir + "/", tmp.string() + "/"};
    env.write("mads.ini", data, out_path.filename().string());
    fs::copy(tmp / out_path.filename(), out_path);
    fs::remove(tmp / out_path.filename());
  } catch (const exception &e) {
    cerr << fg::red << "Cannot scaffold " << path.string() << ": " << e.what()
        << fg::reset << endl;
    return false;
  }
  return true;
}

// --graph [output.dot]: build a Mads::AgentTopicInfo map from every
// non-'agents'/non-'broker' section's pub_topic/sub_topic keys -- the same
// section-skipping convention the plugin `attachment` scan above uses -- and
// hand it to the pure Mads::topology_graph() builder (src/topology_graph.hpp,
// no ZMQ/Agent/file I/O of its own), then write the resulting DOT text to
// `output_path`, or stdout when it's empty. Read-only reporting mode, like
// --plan: it does not run the broker/plugin/CURVE checks and does not affect
// overall_exit_code. `curve` is the one thing it does need from --crypto:
// with --graph-live it subscribes to the broker's backend for real, so an
// encrypted fleet needs the keys to read anything at all.
int run_graph_check(const string &settings_path, const string &output_path,
                    Mads::GraphOptions graph_options, bool live,
                    chrono::milliseconds live_timeout,
                    const optional<Doctor::CurveKeyCheck> &curve) {
  toml::table config;
  try {
    config = toml::parse_file(settings_path);
  } catch (const exception &e) {
    cerr << fg::red << "Error: cannot parse '" << settings_path << "': " << e.what()
        << fg::reset << endl;
    return 1;
  }

  map<string, Mads::AgentTopicInfo> agents;
  for (const auto &[key, node] : config) {
    const string section = string(key.str());
    if (section == "agents" || section == "broker") {
      continue;
    }
    const auto *table = node.as_table();
    if (!table) {
      continue;
    }
    Mads::AgentTopicInfo info;
    if (auto pub = (*table)["pub_topic"].value<string>()) {
      info.pub_topic = *pub;
    } else {
      // Agent::init() reads `cfg["pub_topic"].value_or(_name)`, so a section
      // with no pub_topic key still publishes -- under its own name. Filters
      // routinely rely on that (`[running_avg] sub_topic = ["serial_reader"]`
      // with no pub_topic publishes "running_avg"), and omitting it here used
      // to drop those edges from the graph entirely. Flagged as implicit so
      // topology_graph() can draw it muted and never call it dangling: a pure
      // sink inherits exactly the same default and nobody listening to it is
      // the normal case.
      info.pub_topic = section;
      info.pub_implicit = true;
    }
    const auto &sub_node = (*table)["sub_topic"];
    if (sub_node.type() == toml::node_type::string) {
      info.sub_topic.push_back(sub_node.value_or(string("")));
    } else if (const auto *arr = sub_node.as_array()) {
      arr->for_each(
          [&](auto &&el) { info.sub_topic.push_back(el.value_or(string(""))); });
    }
    agents.emplace(section, std::move(info));
  }

  if (live) {
    // The table is published on the broker's *backend* -- where every agent
    // connects its SUB socket -- not on the settings endpoint.
    const string backend =
        to_probe_uri(config["broker"]["backend_address"].value_or(
            string(BROKER_BACKEND)));
    cerr << style::italic << "Reading live subscriptions from " << backend
        << " (up to " << live_timeout.count() << " ms)..." << style::reset
        << endl;
    auto table = Mads::fetch_subscription_table(backend, live_timeout, curve);
    if (!table) {
      // Deliberately fatal rather than falling back to the declared graph: a
      // silently-degraded live graph is byte-identical to a plain one, so it
      // would read as confirmation that the fleet matches its settings when
      // in fact nothing was measured at all.
      cerr << fg::red << "Error: no subscription table arrived from " << backend
          << fg::reset << endl
          << "  -> start the broker with [broker] subscription_table = true, "
             "allow at least 2000 ms with --timeout (the broker republishes "
             "the table about once a second), and pass --crypto/--keys_dir "
             "if the broker runs with CURVE encryption." << endl;
      return 1;
    }
    graph_options.live = std::move(*table);
  }

  const string dot = Mads::topology_graph(agents, graph_options);

  if (output_path.empty()) {
    cout << dot;
    return 0;
  }

  ofstream ofs(output_path);
  if (!ofs) {
    cerr << fg::red << "Error: cannot write to '" << output_path << "'" << fg::reset
        << endl;
    return 1;
  }
  ofs << dot;
  cout << fg::green << "Wrote topology graph (" << agents.size()
      << " agent(s)) to '" << output_path << "'" << fg::reset << endl;
  return 0;
}

} // namespace

int main(int argc, char *argv[]) {
  Options options(argv[0], "MADS environment/connectivity health check, version " +
                              Mads::version());
  // clang-format off
  options.add_options()
    ("s,settings", "Path to the settings file to check (default: " + string(kDefaultSettingsPath) + ")", value<string>()->default_value(kDefaultSettingsPath))
    ("broker", "Broker settings endpoint to probe (default: derived from the [broker] section, else " + string(kDefaultBrokerUri) + ")", value<string>())
    ("timeout", "Timeout in ms for broker/port probes (default: 1000)", value<int>()->default_value("1000"))
    ("plugin", "Dry-run load this plugin file (repeatable; default: every 'attachment' key found in the settings file)", value<vector<string>>())
    ("crypto", "Check CURVE key files and speak CURVE in every broker probe (same convention as other mads-* executables); pass this whenever the broker runs with --crypto")
    ("keys_dir", "Directory where CURVE keys are stored", value<string>()->default_value(Mads::exec_dir("../etc")))
    ("key_broker", "Name of the broker/server key file (without .pub extension)", value<string>()->default_value("broker"))
    ("key_client", "Name of the client key file (without .key/.pub extension)", value<string>()->default_value("client"))
    ("plan", "Validate a director.toml deployment plan (like `mads up --dry-run`) and exit", value<string>())
    ("graph", "Emit a Graphviz DOT topology graph of the settings file's declared pub/sub topics to the given path (default: stdout) and exit", value<string>()->implicit_value(""))
    ("graph-fanout", "With --graph: draw one edge per publisher into every subscribe-all (sub_topic = [\"\"]) subscriber, instead of collapsing them into a count")
    ("graph-live", "With --graph: overlay live subscriber counts read from the broker's subscription table (requires [broker] subscription_table = true) onto the declared graph")
    ("fix", "Attempt safe, non-destructive auto-fixes (currently: scaffold a missing settings file)")
    ("v,version", "Print version")
    ("h,help", "Print usage");
  // clang-format on

  ParseResult parsed;
  try {
    parsed = options.parse(argc, argv);
  } catch (const std::exception &e) {
    cerr << fg::red << "Error: " << e.what() << fg::reset << endl;
    cerr << options.help() << endl;
    return EXIT_FAILURE;
  }

  if (parsed.count("help")) {
    cout << options.help() << endl;
    return 0;
  }
  if (parsed.count("version")) {
    cout << Mads::version() << endl;
    return 0;
  }

  // --plan is a standalone mode, exactly like `mads up --dry-run`: it does
  // not touch the settings file, broker, plugins, or CURVE keys.
  if (parsed.count("plan")) {
    return run_plan_check(parsed["plan"].as<string>());
  }

  // Resolved before anything that talks to the broker: --crypto is not just
  // an extra key-file check, it is the encryption mode every probe below has
  // to speak. A CURVE-secured broker drops a plain peer during the ZMTP
  // handshake, so probing it in the clear reports it as down (checks 2 and
  // --graph-live both used to do exactly that).
  optional<Doctor::CurveKeyCheck> curve_cfg;
  if (parsed.count("crypto")) {
    Doctor::CurveKeyCheck cfg;
    cfg.key_dir = parsed["keys_dir"].as<string>();
    cfg.client_key_name = parsed["key_client"].as<string>();
    cfg.server_key_name = parsed["key_broker"].as<string>();
    curve_cfg = std::move(cfg);
  }

  // --graph is likewise a standalone, read-only reporting mode: it parses
  // the settings file (like check 1) but never probes the broker/plugins/
  // ports/CURVE keys, and always exits immediately after emitting the DOT
  // text.
  if (parsed.count("graph")) {
    Mads::GraphOptions graph_options;
    graph_options.expand_catch_all = parsed.count("graph-fanout") > 0;
    // Live mode needs to outlast the broker's ~1 s republish period; the
    // shared --timeout default of 1000 ms is right on the edge, so give it a
    // floor rather than letting it fail intermittently.
    const auto live_timeout =
        std::max(chrono::milliseconds(parsed["timeout"].as<int>()),
                 chrono::milliseconds(2500));
    return run_graph_check(parsed["settings"].as<string>(),
                           parsed["graph"].as<string>(), graph_options,
                           parsed.count("graph-live") > 0, live_timeout,
                           curve_cfg);
  }

  const auto timeout = chrono::milliseconds(parsed["timeout"].as<int>());
  const fs::path settings_path = parsed["settings"].as<string>();
  const bool settings_is_remote = parsed["settings"].as<string>().rfind("tcp://", 0) == 0;

  cout << style::bold << "mads doctor" << style::reset << " -- checking '"
      << settings_path.string() << "'" << endl
      << endl;

  // --- 1. settings file found and parses ---------------------------------
  CheckResult settings_result = Doctor::check_settings_file(settings_path);
  if (settings_result.status == Status::Fail && parsed.count("fix") &&
      settings_result.fixable && !settings_is_remote) {
    if (fix_missing_settings_file(settings_path)) {
      cout << fg::magenta << "  [FIX]  Scaffolded " << settings_path.string()
          << fg::reset << endl;
      settings_result = Doctor::check_settings_file(settings_path);
    }
  }
  print_result(settings_result);

  // Parse once more (if possible) to drive the remaining checks. A failure
  // here just means checks 3/5/6 fall back to CLI-only defaults instead of
  // being skipped outright.
  optional<toml::table> config;
  if (!settings_is_remote && fs::exists(settings_path)) {
    try {
      config = toml::parse_file(settings_path.string());
    } catch (const exception &) {
      // already reported by check_settings_file()
    }
  }

  // --- 2. broker reachable -------------------------------------------------
  string broker_uri = kDefaultBrokerUri;
  if (parsed.count("broker")) {
    broker_uri = parsed["broker"].as<string>();
  } else if (settings_is_remote) {
    broker_uri = parsed["settings"].as<string>();
  } else if (config.has_value()) {
    string settings_address =
        (*config)["broker"]["settings_address"].value_or(string(kDefaultBrokerUri));
    broker_uri = to_probe_uri(settings_address);
  }
  print_result(Doctor::check_broker_reachable(broker_uri, timeout, curve_cfg));

  // --- 3 & 4. declared plugin(s) resolve/load + protocol match ------------
  // Resolved up front (CLI --plugin resolves like plugin_loader.cpp's own
  // positional argument -- CWD first, installed location as fallback; a
  // settings-file `attachment` key resolves like broker.cpp serves one --
  // relative to the executable directory). Each entry also carries the
  // driver name to look it up under: the section's `driver` setting if one
  // was declared, else the file stem -- the same precedence
  // plugin_loader.cpp applies at load time (a bare --plugin has no section
  // to read a `driver` key from, so it always falls back to the stem).
  vector<pair<fs::path, string>> plugin_files;
  if (parsed.count("plugin")) {
    for (const auto &raw : parsed["plugin"].as<vector<string>>()) {
      auto resolved = resolve_cli_plugin_path(raw);
      plugin_files.emplace_back(resolved, resolved.stem().string());
    }
  } else if (config.has_value()) {
    for (const auto &[key, node] : *config) {
      const string section = string(key.str());
      if (section == "agents" || section == "broker") {
        continue;
      }
      if (const auto *table = node.as_table()) {
        if (auto attachment = (*table)["attachment"].value<string>()) {
          auto resolved = resolve_attachment_path(*attachment);
          string driver_name =
              (*table)["driver"].value_or(resolved.stem().string());
          plugin_files.emplace_back(resolved, driver_name);
        }
      }
    }
  }

  if (plugin_files.empty()) {
    cout << fg::gray << style::italic
        << "  (no plugins declared via 'attachment' in the settings file "
           "and no --plugin given; skipping plugin checks)"
        << style::reset << fg::reset << endl;
  } else {
    const fs::path deps_manifest = Mads::exec_dir("../share/plugin_deps.json");
    string manifest_text;
    optional<int> pinned_protocol;
    if (Mads::PluginMigrate::read_file(deps_manifest, manifest_text)) {
      pinned_protocol = Doctor::parse_pinned_plugin_protocol(manifest_text);
    }

    for (const auto &[resolved, driver_name] : plugin_files) {
      auto facts = probe_plugin_load(resolved, driver_name);
      auto load_result = Doctor::evaluate_plugin_load(facts);
      print_result(load_result);
      print_result(Doctor::evaluate_plugin_protocol(facts.protocol_version,
                                                     pinned_protocol));
    }
  }

  // --- 5. CURVE key files, if configured -----------------------------------
  if (curve_cfg) {
    CheckResult curve_keys_result = Doctor::check_curve_keys(*curve_cfg);
    print_result(curve_keys_result);
    // A live handshake only makes sense once the key files themselves check
    // out; skip it otherwise so a missing/malformed key doesn't also print a
    // redundant, less specific "rejected" or "timed out".
    if (curve_keys_result.status != Status::Fail) {
      print_result(
          Doctor::check_curve_handshake(broker_uri, *curve_cfg, timeout));
    }
  }

  // --- 6. local port-availability sanity check -----------------------------
  if (config.has_value()) {
    struct PortDef {
      const char *key;
      const char *label;
    };
    const vector<PortDef> ports = {
        {"frontend_address", "frontend"},
        {"backend_address", "backend"},
        {"settings_address", "settings"},
    };
    for (const auto &def : ports) {
      string address = (*config)["broker"][def.key].value_or(string(""));
      if (address.empty()) {
        continue;
      }
      if (auto port = port_of(address)) {
        print_result(Doctor::check_port_available(
            "127.0.0.1", *port, std::min(timeout, chrono::milliseconds(300))));
      }
    }
  } else if (!settings_is_remote) {
    cout << fg::gray << style::italic
        << "  (settings file unavailable; skipping port-availability check)"
        << style::reset << fg::reset << endl;
  }

  cout << endl;
  if (overall_exit_code == 0) {
    cout << fg::green << style::bold << "All checks passed." << style::reset
        << fg::reset << endl;
  } else {
    cout << fg::red << style::bold
        << "One or more checks failed; see [FAIL] lines above." << style::reset
        << fg::reset << endl;
  }

  return overall_exit_code;
}
