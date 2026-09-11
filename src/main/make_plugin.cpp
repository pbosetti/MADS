/*******************************************************************************
  __  __       _                _             _       
 |  \/  | __ _| | _____   _ __ | |_   _  __ _(_)_ __  
 | |\/| |/ _` | |/ / _ \ | '_ \| | | | |/ _` | | '_ \ 
 | |  | | (_| |   <  __/ | |_) | | |_| | (_| | | | | |
 |_|  |_|\__,_|_|\_\___| | .__/|_|\__,_|\__, |_|_| |_|
                         |_|            |___/         
Plugin maker: creates stub files for developing a new MADS plugin
*******************************************************************************/
#include <iostream>
#include <algorithm>
#include <fstream>
#include <inja/inja.hpp>
#include <cxxopts.hpp>
#include <filesystem>
#include <rang.hpp>
#include "../exec_path.hpp"
#include "../mads.hpp"
#include "plugin_migrate.hpp"
#include "plugin_skill.hpp"
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace std;
using namespace inja;
using json = nlohmann::json;
using namespace cxxopts;
using namespace rang;

static const vector<string> types = {"source", "sink", "filter"};

/*
  _   _ _   _ _ _ _            __                  _   _                 
 | | | | |_(_) (_) |_ _   _   / _|_   _ _ __   ___| |_(_) ___  _ __  ___ 
 | | | | __| | | | __| | | | | |_| | | | '_ \ / __| __| |/ _ \| '_ \/ __|
 | |_| | |_| | | | |_| |_| | |  _| |_| | | | | (__| |_| | (_) | | | \__ \
  \___/ \__|_|_|_|\__|\__, | |_|  \__,_|_| |_|\___|\__|_|\___/|_| |_|___/
                      |___/                                              
*/
static string lowercase(string str) {
  transform(str.begin(), str.end(), str.begin(), [](unsigned char c){ 
    return std::tolower(c); 
  });
  return str;
}
static string ucfirst(string str) {
  str[0] = toupper(str[0]);
  return str;
}

// Dependency pins for a freshly scaffolded plugin come from the shared manifest
// (share/plugin_deps.json), the single source of truth kept in sync with the
// newest migration step. The defaults below are the fallback for an install
// with a missing manifest; keep them mirroring the manifest, or a scaffolded
// plugin silently ends up one protocol behind.
static json plugin_deps(string const &manifest_path) {
  json deps = {{"plugin_protocol", 8},
               {"plugin_min_protocol", 7},
               {"plugin_git_tag", "v2.4-P8"},
               {"pugg_git_tag", "1.2.0"},
               {"json_version", "v3.12.0"}};
  ifstream mf(manifest_path);
  if (mf) {
    try {
      json manifest = json::parse(mf);
      for (auto const &key : {"plugin_protocol", "plugin_min_protocol",
                              "plugin_git_tag", "pugg_git_tag",
                              "json_version"}) {
        if (manifest.contains(key))
          deps[key] = manifest[key];
      }
    } catch (...) {
      // keep defaults on malformed manifest
    }
  }
  return deps;
}



/*
  __  __       _       
 |  \/  | __ _(_)_ __  
 | |\/| |/ _` | | '_ \ 
 | |  | | (_| | | | | |
 |_|  |_|\__,_|_|_| |_|
                       
*/

int main(int argc, char **argv) {
  json data;
  string str;
  string dir = "plugins/";
  auto exec_path = Mads::exec_path();
  auto template_dir = Mads::exec_dir("../share/templates/");
  auto plugins_dir = Mads::exec_dir("../");
  bool overwrite = false;
  string cli{argv[0]}; // Name of the executable, used
  for (int i = 1; i < argc; i++) {
    cli += " ";
    cli += argv[i];
  }

  Options options(argv[0]);

  options.add_options()
  // clang-format off
    ("n,name", "Name of the plugin", value<string>())
    ("t,type", "Type of the plugin (source, filter, sink)", value<string>())
    ("d,dir", "Directory of the plugin", value<string>())
    ("i,install-dir", "Directory where to install the plugin (def. " + plugins_dir + ")", value<string>())
    ("o,overwrite", "Overwrite existing files")
    ("r,rust", "Create a Rust plugin (uses mads-rsource/rfilter/rsink loader)")
    ("s,datastore", "Enable Datastore class for persistency (C++ only)")
    ("no-skill", "Do not write the mads-plugin agent skill and AGENTS.md")
    ("u,update", "Migrate an existing plugin (dir from --dir or positional) to the current protocol")
    ("dry-run", "With --update: show the changes without writing any file")
    ("no-check", "With --update: skip the post-migration compile check")
    ("from", "With --update: override the detected source protocol", value<int>())
    ("to", "With --update: override the target protocol", value<int>())
    ("v,version", "Print version")
    ("h,help", "Print usage");
  options.parse_positional({"name"});
  options.positional_help("<name of the plugin>");
  // clang-format on

  auto options_parsed = options.parse(argc, argv);

  if (options_parsed.count("version") > 0) {
    cout << Mads::version() << endl;
    exit(0);
  }

  if (options_parsed.count("help") > 0) {
    std::cout << options.help() << endl;
    exit(0);
  }

  // ── Migration mode: update an existing plugin instead of scaffolding a new one
  if (options_parsed.count("update") > 0) {
    filesystem::path project_dir = ".";
    if (options_parsed.count("dir") > 0)
      project_dir = options_parsed["dir"].as<string>();
    else if (options_parsed.count("name") > 0)
      project_dir = options_parsed["name"].as<string>();

    Mads::PluginMigrate::Options mopts;
    mopts.dry_run = options_parsed.count("dry-run") > 0;
    mopts.check = options_parsed.count("no-check") == 0;
    if (options_parsed.count("from") > 0)
      mopts.from_override = options_parsed["from"].as<int>();
    if (options_parsed.count("to") > 0)
      mopts.to_override = options_parsed["to"].as<int>();

    filesystem::path migrations_dir = Mads::exec_dir("../share/plugin_migrations/");
    filesystem::path deps_manifest = Mads::exec_dir("../share/plugin_deps.json");
    int rc = Mads::PluginMigrate::run(project_dir, migrations_dir, deps_manifest,
                                      mopts);

    // Refresh the agent skill so its documented protocol matches the one the
    // project now targets. Only on a real (non-dry) run, and never when the
    // migration itself failed structurally.
    if (rc != 1 && !mopts.dry_run && options_parsed.count("no-skill") == 0) {
      json sdata = plugin_deps(deps_manifest.string());
      string ver = Mads::version();
      sdata["mads_version"] = ver.rfind('v', 0) == 0 ? ver.substr(1) : ver;
      cout << endl << style::bold << "Refreshing agent skill:" << style::reset
           << endl;
      Mads::PluginSkill::install(Mads::exec_dir("../share/skills/mads-plugin/"),
                                 project_dir, sdata, true);
    }
    return rc;
  }

  data["type"] = "source";

  if (options_parsed.count("name") == 0) {
    cerr << fg::red << "No plugin name provided" << fg::reset << endl;
    exit(1);
  } else {
    data["name"] = lowercase(options_parsed["name"].as<string>());
    if (options_parsed.count("name") > 1) 
      cerr << fg::yellow << "Warning: multiple names provided, using " 
           << data["name"] << fg::reset << endl;
  }
  dir = data.value("name", "example_plugin") + "/";
  if (options_parsed.count("type") > 0) {
    data["type"] = lowercase(options_parsed["type"].as<string>());
    if (options_parsed.count("type") > 1) 
      cerr << fg::yellow << "Warning: multiple types provided, using " 
           << data["type"] << fg::reset << endl;
  }

  if (find(types.begin(), types.end(), string(data["type"])) == types.end()) {
    cerr << fg::red << "Invalid plugin type: " << data["type"] << fg::reset 
         << endl;
    exit(1);
  }

  if (options_parsed.count("dir") > 0) {
    dir = options_parsed["dir"].as<string>() + "/";
  }

  if (options_parsed.count("install-dir") > 0) {
    data["install_dir"] = options_parsed["install-dir"].as<string>();
  } else {
    data["install_dir"] = plugins_dir;
  }
  #ifdef _WIN32
  std::string id = data["install_dir"];
  std::replace(id.begin(), id.end(), '\\', '/');
  data["install_dir"] = id;
  #endif

  if (options_parsed.count("overwrite") > 0) {
    overwrite = true;
  }

  if (options_parsed.count("datastore") > 0) {
    data["datastore"] = true;
  } else {
    data["datastore"] = false;
  }

  data["class_name"] = ucfirst(data["name"]) + "Plugin";
  data["parent_header"] = string(data["type"]) + ".hpp";
  data["source_template"] = string(data["type"]) + ".cpp";
  data["type"] = ucfirst(data["type"]);
  data["parent"] = ucfirst(data["type"]);
  data["type_lower"] = lowercase(string(data["type"]));
  data["rust_loader"] = "mads-r" + string(data["type_lower"]);
  data["source_file"] = string(data["name"]) + ".cpp";
  if (Mads::version().rfind('v', 0) == 0) {
    data["mads_version"] = Mads::version().substr(1); // remove leading 'v'
  } else {
    data["mads_version"] = Mads::version();
  }
  data["cli"] = cli;
  data["today"] = Mads::get_ISODate_time(chrono::system_clock::now());
  data["cwd"] = filesystem::current_path().string();
  char hostname[HOST_NAME_MAX] = "unknown";
  bool got_hostname = false;
#ifdef _WIN32
  DWORD hostname_len = HOST_NAME_MAX;
  got_hostname = GetComputerNameA(hostname, &hostname_len) != 0;
#else
  got_hostname = gethostname(hostname, HOST_NAME_MAX) == 0;
#endif
  if (!got_hostname) {
    data["hostname"] = "unknown";
  } else {
    data["hostname"] = hostname;
  }

  // Dependency pins for the generated CMakeLists.txt, and the protocol numbers
  // stamped into the agent skill, come from the shared manifest.
  data.merge_patch(plugin_deps(Mads::exec_dir("../share/plugin_deps.json")));

  bool rust = options_parsed.count("rust") > 0;
  data["rust"] = rust;

  if (rust && options_parsed.count("datastore") > 0) {
    cerr << fg::yellow << "Warning: --datastore is not applicable to Rust plugins, ignoring"
         << fg::reset << endl;
  }

  string build_hint, run_hint;

  filesystem::create_directory(dir);
  filesystem::create_directory(dir + "src/");

  Environment env_primary{template_dir + "/", dir};
  Environment env_src{template_dir + "/", dir + "src/"};
  Environment env_md{template_dir + "/", dir};
  env_md.set_line_statement("%%");

  string readme_file = dir + "README.md";

  if (rust) {
    // ── Rust plugin ──────────────────────────────────────────────────────────
    string cargo_file = dir + "Cargo.toml";
    string lib_file   = dir + "src/lib.rs";

    cout << "Creating Rust plugin " << style::bold << data["name"] << style::reset
         << " of type " << style::bold << data["type"]
         << " in " << style::bold << dir << style::reset << endl;

    cout << "==> " << style::bold << cargo_file << style::reset << ": ";
    if (!overwrite && filesystem::exists(cargo_file)) {
      cout << fg::red << "already exists, skipped; use -o to overwrite"
           << fg::reset << endl;
    } else {
      env_primary.write("rust_Cargo.toml", data, "Cargo.toml");
      cout << fg::green << "created" << fg::reset << endl;
    }

    cout << "==> " << style::bold << lib_file << style::reset << ": ";
    if (!overwrite && filesystem::exists(lib_file)) {
      cerr << fg::red << "already exists, skipped; use -o to overwrite"
           << fg::reset << endl;
    } else {
      env_src.write("rust_" + string(data["type_lower"]) + ".rs", data, "lib.rs");
      cout << fg::green << "created" << fg::reset << endl;
    }

    cout << "==> " << style::bold << readme_file << style::reset << ": ";
    if (!overwrite && filesystem::exists(readme_file)) {
      cerr << fg::red << "already exists, skipped; use -o to overwrite"
           << fg::reset << endl;
    } else {
      env_md.write("rust_README.md", data, "README.md");
      cout << fg::green << "created" << fg::reset << endl;
    }

    build_hint = "cd " + dir + " && cargo build --release";
    // cargo names a cdylib lib<name>.dylib on macOS, lib<name>.so elsewhere
#ifdef __APPLE__
    const string cdylib_ext = ".dylib";
#else
    const string cdylib_ext = ".so";
#endif
    run_hint = string(data["rust_loader"]) + " -n " + string(data["name"]) +
               " target/release/lib" + string(data["name"]) + cdylib_ext;

  } else {
    // ── C++ plugin ───────────────────────────────────────────────────────────
    string cmake_file  = dir + "CMakeLists.txt";
    string source_file = dir + "src/" + string(data["source_file"]);

    cout << "Creating plugin " << style::bold << data["name"] << style::reset
         << " of type " << style::bold << data["type"]
         << " in " << style::bold << dir << style::reset << endl;

    cout << "==> " << style::bold << cmake_file << style::reset << ": ";
    if (!overwrite && filesystem::exists(cmake_file)) {
      cout << fg::red << "already exists, skipped; use -o to overwrite"
           << fg::reset << endl;
    } else {
      env_primary.write("CMakeLists.txt", data, "CMakeLists.txt");
      cout << fg::green << "created" << fg::reset << endl;
    }

    cout << "==> " << style::bold << source_file << style::reset << ": ";
    if (!overwrite && filesystem::exists(source_file)) {
      cerr << fg::red << "already exists, skipped; use -o to overwrite"
           << fg::reset << endl;
    } else {
      env_src.write(data["source_template"], data, data["source_file"]);
      cout << fg::green << "created" << fg::reset << endl;
    }

    cout << "==> " << style::bold << readme_file << style::reset << ": ";
    if (!overwrite && filesystem::exists(readme_file)) {
      cerr << fg::red << "already exists, skipped; use -o to overwrite"
           << fg::reset << endl;
    } else {
      env_md.write("README.md", data, "README.md");
      cout << fg::green << "created" << fg::reset << endl;
    }

    build_hint = "cd " + dir + " && cmake -Bbuild && cmake --build build";
  }

  // Agent documentation: a project-local AGENTS.md pointing at the mads-plugin
  // skill, which carries the runtime context (call order, return-type effects,
  // settings injection) that a plugin author's assistant cannot infer from the
  // plugin API alone. Refreshed by `mads plugin --update`.
  if (options_parsed.count("no-skill") == 0) {
    string agents_file = dir + "AGENTS.md";
    cout << "==> " << style::bold << agents_file << style::reset << ": ";
    if (!overwrite && filesystem::exists(agents_file)) {
      cout << fg::red << "already exists, skipped; use -o to overwrite"
           << fg::reset << endl;
    } else {
      env_md.write("AGENTS.md", data, "AGENTS.md");
      cout << fg::green << "created" << fg::reset << endl;
    }
    Mads::PluginSkill::install(Mads::exec_dir("../share/skills/mads-plugin/"),
                               dir, data, overwrite);
  }

  cout << "To build: " << style::bold << build_hint << style::reset << endl;
  if (!run_hint.empty())
    cout << "To run:   " << style::bold << run_hint << style::reset << endl;

  return 0;
}
