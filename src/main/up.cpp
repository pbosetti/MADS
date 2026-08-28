/*
  _   _
 | | | |_ __
 | | | | '_ \
 | |_| | |_) |
  \___/| .__/
       |_|

`mads up`: a headless, scriptable executor for `director.toml`, the config
format already owned by mads_director (https://github.com/mads-net/mads_director).
Parses/validates/expands the file itself (src/director_config.hpp -- see that
file's top comment for exactly what was verified against Director's pinned
v2.4.2 tag), then starts, supervises and tears down the described processes
via src/up_supervisor.hpp. Foreground-only by design: no daemonization, no PID
file, no `mads down` -- SIGINT/SIGTERM tears everything down and this process
exits, exactly what systemd Type=simple / Docker / CI expect.

Author(s): Paolo Bosetti
*/
#include "../director_config.hpp"
#include "../exec_path.hpp"
#include "../mads.hpp"
#include "../up_supervisor.hpp"

#include <cxxopts.hpp>
#include <rang.hpp>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <iostream>

using namespace std;
using namespace Mads;
using namespace cxxopts;
using namespace rang;

namespace {

atomic<UpSupervisor *> g_supervisor{nullptr};

void install_signal_handlers() {
  auto handler = [](int) {
    UpSupervisor *sup = g_supervisor.load();
    if (sup != nullptr) {
      sup->request_stop();
    }
  };
  signal(SIGINT, handler);
  signal(SIGTERM, handler);
}

string describe_ready(const ProcessConfig &p, bool crypto) {
  if (!p.ready.has_value()) {
    return "-";
  }
  const ReadySpec &r = *p.ready;
  switch (r.kind) {
  case ReadyKind::Broker:
    // The encryption mode is the one thing about a broker probe that is
    // invisible in the plan file itself, so --dry-run has to say it.
    return "broker:" + r.broker_uri + (crypto ? " (CURVE)" : "");
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

void print_plan(const DirectorConfig &config, bool crypto) {
  cout << style::bold << "Expanded plan (" << config.processes.size()
       << " process instance(s), start order):" << style::reset << endl;
  for (const auto &p : config.processes) {
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
      cout << "    ready   : " << describe_ready(p, crypto) << "\n";
    }
  }
}

} // namespace

int main(int argc, char *argv[]) {
  Options options(argv[0], "Headless executor for director.toml, version " +
                              Mads::version());
  // clang-format off
  options.add_options()
    ("f,file", "Path to director.toml", value<string>()->default_value("director.toml"))
    ("until-exit", "Tear down and propagate the exit code once the named process exits", value<string>())
    ("timeout", "Hard cap on the whole run, e.g. '30s', '5m' (default: none)", value<string>())
    ("grace", "SIGTERM -> SIGKILL grace period, e.g. '5s'", value<string>()->default_value("5s"))
    ("max-restarts", "Cap on relaunch attempts per process (default: unlimited)", value<int>())
    ("base-instance-id", "Override every section's base_instance_id, so ${ID} numbering starts here (default: use the file's own values)", value<int>())
    ("no-shell", "Tokenize 'command' and exec it directly instead of shelling out")
    ("crypto", "Speak CURVE in `ready = \"broker\"` probes (same convention as other mads-* executables); pass this whenever the broker runs with --crypto")
    ("keys_dir", "Directory where CURVE keys are stored", value<string>()->default_value(Mads::exec_dir("../etc")))
    ("key_broker", "Name of the broker/server key file (without .pub extension)", value<string>()->default_value("broker"))
    ("key_client", "Name of the client key file (without .key/.pub extension)", value<string>()->default_value("client"))
    ("dry-run", "Print the fully expanded plan (templates resolved, scale expanded, start order, ready probes and their encryption mode) without spawning anything")
    ("q,quiet", "Suppress multiplexed [name] stdout/stderr passthrough")
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

  const string path = parsed["file"].as<string>();

  // Has to reach the loader rather than be applied afterwards: `${ID}` is
  // substituted into each instance's command during expansion, so once
  // load_director_config() returns there is nothing left to renumber.
  DirectorLoadOptions load_options;
  if (parsed.count("base-instance-id")) {
    load_options.base_instance_id = parsed["base-instance-id"].as<int>();
  }

  string load_error;
  vector<string> warnings;
  auto config =
      load_director_config(path, &load_error, &warnings, load_options);
  for (const auto &w : warnings) {
    cerr << fg::yellow << "Warning: " << w << fg::reset << endl;
  }
  if (!config.has_value()) {
    cerr << fg::red << "Error loading '" << path << "': " << load_error
        << fg::reset << endl;
    return EXIT_FAILURE;
  }

  // Built before the --dry-run early exit so the printed plan can say which
  // mode each broker probe will run in. Only `ready = "broker"` uses this:
  // the agents' own encryption is already in their `command` strings.
  optional<Mads::ProbeCurveKeys> curve_cfg;
  if (parsed.count("crypto")) {
    Mads::ProbeCurveKeys cfg;
    cfg.key_dir = parsed["keys_dir"].as<string>();
    cfg.client_key_name = parsed["key_client"].as<string>();
    cfg.server_key_name = parsed["key_broker"].as<string>();
    curve_cfg = std::move(cfg);
  }

  if (parsed.count("dry-run")) {
    print_plan(*config, curve_cfg.has_value());
    return 0;
  }

  UpOptions up_options;
  up_options.curve = curve_cfg;
  if (parsed.count("until-exit")) {
    up_options.until_exit = parsed["until-exit"].as<string>();
  }
  if (parsed.count("timeout")) {
    const string raw = parsed["timeout"].as<string>();
    auto dur = parse_duration(raw);
    if (!dur.has_value()) {
      cerr << fg::red << "Error: invalid --timeout value '" << raw << "'"
          << fg::reset << endl;
      return EXIT_FAILURE;
    }
    up_options.timeout = *dur;
  }
  {
    const string raw = parsed["grace"].as<string>();
    auto dur = parse_duration(raw);
    if (!dur.has_value()) {
      cerr << fg::red << "Error: invalid --grace value '" << raw << "'"
          << fg::reset << endl;
      return EXIT_FAILURE;
    }
    up_options.grace = *dur;
  }
  if (parsed.count("max-restarts")) {
    up_options.max_restarts = parsed["max-restarts"].as<int>();
  }
  up_options.no_shell = parsed.count("no-shell") > 0;
  up_options.quiet = parsed.count("quiet") > 0;

  if (up_options.until_exit.has_value()) {
    const bool found =
        std::any_of(config->processes.begin(), config->processes.end(),
                   [&](const ProcessConfig &p) {
                     return p.name == *up_options.until_exit;
                   });
    if (!found) {
      cerr << fg::red << "Error: --until-exit target '"
          << *up_options.until_exit << "' is not a process in '" << path
          << "'" << fg::reset << endl;
      return EXIT_FAILURE;
    }
  }

  UpSupervisor supervisor(config->processes, up_options);
  g_supervisor.store(&supervisor);
  install_signal_handlers();

  RunResult result = supervisor.run();
  g_supervisor.store(nullptr);

  switch (result.outcome) {
  case RunOutcome::Ok:
    if (!result.message.empty()) {
      cout << style::italic << result.message << style::reset << endl;
    }
    break;
  case RunOutcome::Timeout:
    cerr << fg::red << "mads up: " << result.message << fg::reset << endl;
    break;
  case RunOutcome::ReadyTimeout:
    cerr << fg::red << "mads up: " << result.message << fg::reset << endl;
    break;
  case RunOutcome::StartFailed:
    cerr << fg::red << "mads up: " << result.message << fg::reset << endl;
    break;
  case RunOutcome::ProcessFailed:
    cerr << fg::red << "mads up: " << result.message << fg::reset << endl;
    break;
  }

  return result.exit_code;
}
