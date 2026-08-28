// Reference: mads_director tag v2.4.2 (this repo pins that exact tag in the
// top-level CMakeLists.txt, `FetchContent_Declare(mads-director ... GIT_TAG
// v2.4.2 ...)`). Verified by shallow-cloning that tag and reading
// src/config.cpp, src/process_manager.cpp and src/platform_process_{posix,
// windows}.cpp directly (not the README on `main`, which can drift). Summary
// of what v2.4.2 actually does, so a future reader knows what to re-check if
// Director moves:
//
//  - Schema: a `[director]` table (optional; keys `terminal` string and
//    `sample_rate` number of seconds, both optional) plus one table per
//    process, keyed by the process name. Recognized per-process keys:
//    `command` (string, mandatory, non-empty), `after` (string, optional --
//    a *single* other process name, making the dependency graph a forest,
//    not a general DAG), `workdir` (string, optional, resolved against the
//    config file's directory when relative or absent), `enabled` (bool,
//    default true), `scale` (int >= 1, default 1), `relaunch` (bool, default
//    false), `tty` (bool, default false), `base_instance_id` (int >= 0,
//    default 0).
//  - `command` is SHELL-INTERPRETED, not tokenized/exec'd directly:
//      * POSIX (platform_process_posix.cpp, exec_child_command()): runs
//        `$SHELL -lc "<command>"`, falling back to `/bin/sh -lc`/`-c` if
//        $SHELL is unset or execl() of it fails.
//      * Windows (platform_process_windows.cpp): runs
//        `cmd.exe /S /C "<command>"` via CreateProcessA.
//    NEW_FEATURES.md flagged this as unverified; it is shell-out, confirmed.
//  - `scale = N` expands a process into N instances named `base`, `base[2]`,
//    ... `base[N]` (1-based suffix; the bare name with no suffix when N==1),
//    see process_manager.cpp scaled_name(). `${ID}` in `command` is
//    `base_instance_id + <0-based instance index>` (expand_command_template()
//    takes the base as its 4th argument and adds it in); `${PWD}` is the
//    instance's resolved working directory. `base_instance_id` shifts only
//    `${ID}`, never the `base[N]` instance names, which stay 1-based.
//    Added in v2.4.2; a config without the key behaves as before (base 0).
//  - `after = "x"` on a process with scale M expands to a dependency on
//    *all* M instances of `x` (build_process_definitions()): a dependent
//    only starts once every instance of its declared dependency has
//    started. Cycle detection runs on the un-expanded (base name) graph via
//    DFS (detect_cycle_dfs()).
//  - Parser leniency: config.cpp never validates against an allow-list of
//    keys inside a process table (it only looks up the keys it knows about
//    via `table["key"]`), so an extra key such as our own `ready = "..."` is
//    silently ignored by Director's parser -- confirming NEW_FEATURES.md's
//    assumption that `ready` is safely additive. The one place v2.4.2 is
//    NOT lenient: a top-level entry that isn't a TOML table (e.g. a bare
//    top-level key) is a hard parse error ("All top-level entries must be
//    process tables..."). This module deliberately does NOT match that
//    strictness (see load_director_config()'s doc comment) -- being liberal
//    about unknown/malformed top-level entries is this module's own design
//    choice (NEW_FEATURES.md P5 "Parser ownership"), not a fidelity target.
//  - `terminal`/`tty` are GUI-only (attach windows); headless execution
//    ignores them, matching NEW_FEATURES.md.
//
// If mads_director's pinned tag moves past v2.4.2, re-diff its src/config.cpp
// and src/process_manager.cpp against the summary above.

#include "director_config.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace Mads {

namespace {

// ---------------------------------------------------------------------------
// Duration parsing
// ---------------------------------------------------------------------------

std::string trim(const std::string &s) {
  size_t begin = 0, end = s.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(s[begin])))
    ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])))
    --end;
  return s.substr(begin, end - begin);
}

// ---------------------------------------------------------------------------
// Raw (pre-expansion) process representation
// ---------------------------------------------------------------------------

struct RawProcess {
  std::string name;
  std::string command;
  std::optional<std::string> after;
  std::optional<std::string> workdir;
  bool enabled = true;
  int scale = 1;
  int base_instance_id = 0;
  bool relaunch = false;
  bool tty = false;
  std::optional<ReadySpec> ready;
};

std::filesystem::path config_base_dir(const std::string &path) {
  std::error_code ec;
  std::filesystem::path absolute =
      std::filesystem::absolute(std::filesystem::path(path), ec);
  if (ec) {
    absolute = std::filesystem::path(path);
  }
  return absolute.parent_path();
}

std::string scaled_name(const std::string &base_name, int index, int scale) {
  if (scale == 1) {
    return base_name;
  }
  std::ostringstream stream;
  stream << base_name << "[" << (index + 1) << "]";
  return stream.str();
}

std::string expand_command_template(const std::string &command,
                                    const std::string &workdir,
                                    int instance_id) {
  std::string expanded = command;
  const auto replace_all = [](std::string *text, const std::string &token,
                              const std::string &value) {
    std::size_t pos = 0;
    while ((pos = text->find(token, pos)) != std::string::npos) {
      text->replace(pos, token.size(), value);
      pos += value.size();
    }
  };
  replace_all(&expanded, "${PWD}", workdir);
  replace_all(&expanded, "${ID}", std::to_string(instance_id));
  return expanded;
}

// Parses the `[director]` table. Unrecognized keys are reported through
// `warnings`, never fatal.
bool parse_director_section(const toml::table &table, DirectorConfig *config,
                            std::vector<std::string> *warnings,
                            std::string *out_error) {
  static const std::unordered_set<std::string> known{"terminal",
                                                      "sample_rate"};
  for (const auto &[key, node] : table) {
    if (!known.contains(std::string(key.str())) && warnings != nullptr) {
      warnings->push_back("director.toml: unknown key '" +
                          std::string(key.str()) +
                          "' in section '[director]' ignored");
    }
    (void)node;
  }

  if (const auto *terminal_node = table.get("terminal");
      terminal_node != nullptr) {
    const auto terminal = terminal_node->value<std::string>();
    if (!terminal.has_value()) {
      *out_error = "Section '[director]' key 'terminal' must be a string.";
      return false;
    }
    if (!terminal->empty()) {
      config->terminal = *terminal;
    }
  }

  if (const auto *sample_rate_node = table.get("sample_rate");
      sample_rate_node != nullptr) {
    double sample_rate_seconds = 0.0;
    if (const auto value = sample_rate_node->value<double>();
        value.has_value()) {
      sample_rate_seconds = *value;
    } else if (const auto value = sample_rate_node->value<int64_t>();
              value.has_value()) {
      sample_rate_seconds = static_cast<double>(*value);
    } else {
      *out_error =
          "Section '[director]' key 'sample_rate' must be a number of "
          "seconds.";
      return false;
    }
    if (!(sample_rate_seconds > 0.0)) {
      *out_error = "Section '[director]' key 'sample_rate' must be > 0.";
      return false;
    }
    config->sample_rate_seconds = sample_rate_seconds;
  }

  return true;
}

bool parse_process(const std::string &section_name, const toml::table &table,
                   RawProcess *out_process, std::vector<std::string> *warnings,
                   std::string *out_error) {
  static const std::unordered_set<std::string> known{
      "command",  "after", "workdir", "enabled",
      "scale",    "relaunch", "tty",  "ready",
      "base_instance_id"};
  for (const auto &[key, node] : table) {
    if (!known.contains(std::string(key.str())) && warnings != nullptr) {
      warnings->push_back("director.toml: unknown key '" +
                          std::string(key.str()) + "' in section '[" +
                          section_name + "]' ignored");
    }
    (void)node;
  }

  if (section_name.empty()) {
    *out_error = "Process section name must be non-empty.";
    return false;
  }

  const auto command = table["command"].value<std::string>();
  if (!command.has_value() || command->empty()) {
    *out_error =
        "Process '" + section_name + "' must provide a non-empty 'command' string.";
    return false;
  }

  RawProcess process;
  process.name = section_name;
  process.command = *command;

  if (const auto after = table["after"].value<std::string>();
      after.has_value() && !after->empty()) {
    process.after = *after;
  }

  if (const auto workdir = table["workdir"].value<std::string>();
      workdir.has_value() && !workdir->empty()) {
    process.workdir = *workdir;
  }

  if (const auto enabled = table["enabled"].value<bool>();
      enabled.has_value()) {
    process.enabled = *enabled;
  }

  if (const auto scale = table["scale"].value<int64_t>(); scale.has_value()) {
    if (*scale < 1) {
      *out_error =
          "Process '" + process.name + "' has invalid 'scale'. Must be >= 1.";
      return false;
    }
    process.scale = static_cast<int>(*scale);
  }

  if (const auto relaunch = table["relaunch"].value<bool>();
      relaunch.has_value()) {
    process.relaunch = *relaunch;
  }

  if (const auto tty = table["tty"].value<bool>(); tty.has_value()) {
    process.tty = *tty;
  }

  if (const auto base_instance_id = table["base_instance_id"].value<int64_t>();
      base_instance_id.has_value()) {
    if (*base_instance_id < 0) {
      *out_error = "Process '" + process.name +
                   "' has invalid 'base_instance_id'. Must be >= 0.";
      return false;
    }
    process.base_instance_id = static_cast<int>(*base_instance_id);
  }

  if (const auto ready = table["ready"].value<std::string>();
      ready.has_value() && !ready->empty()) {
    std::string ready_error;
    auto spec = parse_ready_spec(*ready, &ready_error);
    if (!spec.has_value()) {
      *out_error = "Process '" + process.name + "' has invalid 'ready': " +
                  ready_error;
      return false;
    }
    process.ready = spec;
  }

  *out_process = std::move(process);
  return true;
}

// DFS-based cycle detection + dependency-first ("topological") ordering over
// the *base* process name graph (before scale expansion). `after` gives each
// base process at most one outgoing edge, so the graph is a forest once
// proven acyclic -- there is no need for a general multi-parent toposort.
bool visit_base(const std::string &name,
                const std::unordered_map<std::string, std::string> &after_of,
                std::unordered_set<std::string> *visiting,
                std::unordered_set<std::string> *done,
                std::vector<std::string> *order, std::string *out_error) {
  if (done->contains(name)) {
    return true;
  }
  if (visiting->contains(name)) {
    *out_error = "Dependency cycle detected around process '" + name + "'.";
    return false;
  }
  visiting->insert(name);
  if (const auto it = after_of.find(name); it != after_of.end()) {
    if (!visit_base(it->second, after_of, visiting, done, order, out_error)) {
      return false;
    }
  }
  visiting->erase(name);
  done->insert(name);
  order->push_back(name);
  return true;
}

} // namespace

// ---------------------------------------------------------------------------
// parse_duration
// ---------------------------------------------------------------------------

std::optional<std::chrono::milliseconds>
parse_duration(const std::string &text) {
  const std::string trimmed = trim(text);
  if (trimmed.empty()) {
    return std::nullopt;
  }

  size_t i = 0;
  bool seen_digit = false, seen_dot = false;
  while (i < trimmed.size() &&
        (std::isdigit(static_cast<unsigned char>(trimmed[i])) ||
         trimmed[i] == '.')) {
    if (trimmed[i] == '.') {
      if (seen_dot) {
        return std::nullopt;
      }
      seen_dot = true;
    } else {
      seen_digit = true;
    }
    ++i;
  }
  if (!seen_digit) {
    return std::nullopt;
  }

  const std::string number = trimmed.substr(0, i);
  const std::string unit = trim(trimmed.substr(i));

  double value = 0.0;
  try {
    size_t consumed = 0;
    value = std::stod(number, &consumed);
    if (consumed != number.size()) {
      return std::nullopt;
    }
  } catch (const std::exception &) {
    return std::nullopt;
  }
  if (value < 0.0) {
    return std::nullopt;
  }

  double ms;
  if (unit.empty() || unit == "s") {
    ms = value * 1000.0;
  } else if (unit == "ms") {
    ms = value;
  } else if (unit == "m") {
    ms = value * 60000.0;
  } else {
    return std::nullopt;
  }

  return std::chrono::milliseconds(static_cast<int64_t>(std::llround(ms)));
}

// ---------------------------------------------------------------------------
// parse_ready_spec
// ---------------------------------------------------------------------------

std::optional<ReadySpec> parse_ready_spec(const std::string &value,
                                          std::string *out_error) {
  const auto fail = [&](const std::string &msg) -> std::optional<ReadySpec> {
    if (out_error != nullptr) {
      *out_error = msg;
    }
    return std::nullopt;
  };

  if (value.empty()) {
    return fail("empty 'ready' value");
  }

  const auto colon = value.find(':');
  const std::string head = colon == std::string::npos ? value : value.substr(0, colon);
  const std::string rest =
      colon == std::string::npos ? std::string() : value.substr(colon + 1);

  ReadySpec spec;

  if (head == "broker") {
    spec.kind = ReadyKind::Broker;
    spec.broker_uri = rest.empty() ? kDefaultBrokerProbeUri : rest;
    return spec;
  }

  if (head == "port") {
    if (rest.empty()) {
      return fail("'ready = \"port:<n>\"' requires a port number");
    }
    try {
      size_t consumed = 0;
      const int port = std::stoi(rest, &consumed);
      if (consumed != rest.size() || port <= 0 || port > 65535) {
        return fail("'ready = \"port:" + rest + "\"' is not a valid port number");
      }
      spec.kind = ReadyKind::Port;
      spec.port = port;
      return spec;
    } catch (const std::exception &) {
      return fail("'ready = \"port:" + rest + "\"' is not a valid port number");
    }
  }

  if (head == "log") {
    if (rest.empty()) {
      return fail("'ready = \"log:<regex>\"' requires a regex");
    }
    try {
      std::regex compiled(rest);
      (void)compiled;
    } catch (const std::regex_error &e) {
      return fail("'ready = \"log:" + rest + "\"' has an invalid regex: " +
                  e.what());
    }
    spec.kind = ReadyKind::Log;
    spec.log_pattern = rest;
    return spec;
  }

  if (head == "delay") {
    if (rest.empty()) {
      return fail("'ready = \"delay:<dur>\"' requires a duration");
    }
    const auto dur = parse_duration(rest);
    if (!dur.has_value()) {
      return fail("'ready = \"delay:" + rest + "\"' is not a valid duration");
    }
    spec.kind = ReadyKind::Delay;
    spec.delay = *dur;
    return spec;
  }

  return fail("unknown 'ready' probe kind '" + head +
             "' (expected broker, port, log or delay)");
}

// ---------------------------------------------------------------------------
// load_director_config
// ---------------------------------------------------------------------------

std::optional<DirectorConfig>
load_director_config(const std::string &path, std::string *out_error,
                     std::vector<std::string> *out_warnings,
                     const DirectorLoadOptions &options) {
  // Validated up front, before the file is even opened: an out-of-range
  // override is the caller's mistake, and it should be reported the same way
  // whatever the file happens to contain.
  if (options.base_instance_id.has_value() && *options.base_instance_id < 0) {
    if (out_error != nullptr) {
      *out_error = "Invalid base_instance_id override " +
                   std::to_string(*options.base_instance_id) +
                   ". Must be >= 0.";
    }
    return std::nullopt;
  }
  try {
    const auto parsed = toml::parse_file(path);

    DirectorConfig config;
    std::vector<RawProcess> raw_processes;
    std::unordered_set<std::string> names;

    for (const auto &[key, node] : parsed) {
      const auto *table = node.as_table();
      if (table == nullptr) {
        if (out_warnings != nullptr) {
          out_warnings->push_back(
              "director.toml: top-level entry '" + std::string(key.str()) +
              "' is not a table; ignored");
        }
        continue;
      }

      const std::string section_name(key.str());
      if (section_name == "director") {
        if (!parse_director_section(*table, &config, out_warnings, out_error)) {
          return std::nullopt;
        }
        continue;
      }

      RawProcess process;
      if (!parse_process(section_name, *table, &process, out_warnings,
                        out_error)) {
        return std::nullopt;
      }

      if (!names.insert(process.name).second) {
        *out_error = "Duplicate process name detected: '" + process.name + "'.";
        return std::nullopt;
      }

      raw_processes.push_back(std::move(process));
    }

    if (raw_processes.empty()) {
      *out_error = "Config must include at least one process section like [api].";
      return std::nullopt;
    }

    config.base_dir = config_base_dir(path);

    // Validate `after` targets and build the base-name dependency graph.
    std::unordered_map<std::string, std::string> after_of;
    std::unordered_map<std::string, int> scale_of;
    for (const auto &process : raw_processes) {
      scale_of[process.name] = process.scale;
      if (process.after.has_value()) {
        if (!names.contains(*process.after)) {
          *out_error = "Process '" + process.name +
                      "' references unknown dependency in 'after': '" +
                      *process.after + "'.";
          return std::nullopt;
        }
        after_of[process.name] = *process.after;
      }
    }

    // Cycle detection + dependency-first base order.
    std::vector<std::string> base_order;
    {
      std::unordered_set<std::string> visiting, done;
      for (const auto &process : raw_processes) {
        if (!visit_base(process.name, after_of, &visiting, &done, &base_order,
                        out_error)) {
          return std::nullopt;
        }
      }
    }

    std::unordered_map<std::string, const RawProcess *> raw_by_name;
    for (const auto &process : raw_processes) {
      raw_by_name[process.name] = &process;
    }

    // Expand scale/templates in dependency-first base order; every instance
    // of a base process is emitted contiguously, so instance order overall
    // stays a valid topological order (see director_config.hpp).
    for (const auto &base_name : base_order) {
      const RawProcess &process = *raw_by_name.at(base_name);
      for (int i = 0; i < process.scale; ++i) {
        ProcessConfig instance;
        instance.name = scaled_name(process.name, i, process.scale);
        instance.base_name = process.name;
        instance.enabled = process.enabled;
        instance.relaunch = process.relaunch;
        instance.tty = process.tty;
        // Director offsets ${ID} by the section's `base_instance_id`
        // (default 0), so a scale=3 / base_instance_id=10 process expands to
        // ${ID} 10, 11, 12 -- while the instance *names* stay base[1..3].
        //
        // Applied here rather than during parsing so an override wins over
        // every section uniformly, including ones that set the key
        // explicitly, and so the file's own values are still validated even
        // when they are about to be replaced.
        const int base_instance_id =
            options.base_instance_id.value_or(process.base_instance_id);
        instance.instance_id = base_instance_id + i;
        instance.ready = process.ready;

        if (process.workdir.has_value()) {
          const std::filesystem::path candidate(*process.workdir);
          instance.workdir = (candidate.is_absolute()
                                  ? candidate
                                  : (config.base_dir / candidate))
                                 .string();
        } else {
          instance.workdir = config.base_dir.string();
        }
        instance.command = expand_command_template(
            process.command, instance.workdir, instance.instance_id);

        if (process.after.has_value()) {
          const int dep_scale = scale_of.at(*process.after);
          for (int dep_index = 0; dep_index < dep_scale; ++dep_index) {
            instance.after.push_back(
                scaled_name(*process.after, dep_index, dep_scale));
          }
        }

        config.processes.push_back(std::move(instance));
      }
    }

    return config;
  } catch (const toml::parse_error &error) {
    std::ostringstream stream;
    stream << "TOML parse error: " << error.description() << " at "
          << error.source().begin;
    *out_error = stream.str();
    return std::nullopt;
  } catch (const std::exception &error) {
    *out_error = std::string("Config load failed: ") + error.what();
    return std::nullopt;
  }
}

} // namespace Mads
