/*
  __  __    _    ____  ____
 |  \/  |  / \  |  _ \/ ___|
 | |\/| | / _ \ | | | \___ \
 | |  | |/ ___ \| |_| |___) |
 |_|  |_/_/   \_\____/|____/

Command line interface for Mads
Wraps all mads-* comands
Also provides ini and service internal commands
Author: Paolo Bosetti, July 2024
*/

#include "../mads.hpp"
#include "../agent.hpp"
#include "../exec_path.hpp"
#include <cxxopts.hpp>
#include <filesystem>
#include <inja/inja.hpp>
#include <iostream>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <rang.hpp>
#include <zmqpp/zmqpp.hpp>
#include <zmqpp/curve.hpp>

#ifdef _WIN32
#include <process.h>
#define execv(cmd, argv) _execv(cmd, argv)
// disable root operations on Windows
#define getuid() 1
#endif

#define SYSTEMD_PATH "/etc/systemd/system"
#define RELEASE_URL "https://git.new/mads"
#define BETA_URL "https://github.com/pbosetti/mads/releases"

using namespace std;
using namespace inja;
using json = nlohmann::json;
using namespace cxxopts;
using namespace rang;
namespace fs = std::filesystem;

/*
  _   _ _   _ _
 | | | | |_(_) |___
 | | | | __| | / __|
 | |_| | |_| | \__ \
  \___/ \__|_|_|___/

*/

bool includes(vector<string> const &commands, string const &command) {
  return find(commands.begin(), commands.end(), command) != commands.end();
}

bool save_keypair(pair<string, string> &key_files, const string &path, const string &name, bool force=false) {
  zmqpp::curve::keypair keypair = zmqpp::curve::generate_keypair();
  key_files.first = ( fs::path(path) / (name + ".key") ).string();
  key_files.second = ( fs::path(path) / (name + ".pub") ).string();
  if (!force) {
    if (fs::exists(key_files.first) || fs::exists(key_files.second)) {
      cerr << fg::red << "Error: key files already exist. Use -f to overwrite."
           << fg::reset << endl;
      return false;
    }
  }
  try {
    ofstream key_file(key_files.first);
    ofstream pub_file(key_files.second);
    key_file << keypair.secret_key;
    pub_file << keypair.public_key;
    key_file.close();
    pub_file.close();
  } catch (const std::exception &e) {
    cerr << fg::red << "Error: cannot write key files: " << e.what()
         << fg::reset << endl;
    return false;
  }
  return true;
}


/*
  _       _                                                 _
 (_)_ __ (_)   ___ ___  _ __ ___  _ __ ___   __ _ _ __   __| |
 | | '_ \| |  / __/ _ \| '_ ` _ \| '_ ` _ \ / _` | '_ \ / _` |
 | | | | | | | (_| (_) | | | | | | | | | | | (_| | | | | (_| |
 |_|_| |_|_|  \___\___/|_| |_| |_|_| |_| |_|\__,_|_| |_|\__,_|

*/

int make_ini(int argc, char **argv) {
  string template_dir = Mads::exec_dir("../share/templates/");
  string etc_dir = Mads::exec_dir("../etc/");
  string output = "";
  json data;
  data["broker"] = "localhost";
  data["port_frontend"] = "9090";
  data["port_backend"] = "9091";
  data["port_settings"] = "9092";
  data["mongo_uri"] = "mongodb://localhost:27017";
  Options options("mads ini",
                  "Create INI file template, version " + Mads::version());
  // clang-format off
  options.add_options()
    ("o,output", "Output file on a given path", value<string>())
#ifndef _WIN32
    ("i,install", "Install INI file to " + etc_dir, value<bool>())
#endif
    ("b,broker", "broker hostname or IP", value<string>())
    ("F,frontend", "frontend port", value<int>())
    ("B,backend", "backend port", value<int>())
    ("s,settings", "settings port", value<int>())
    ("f,fps", "Frames per second", value<int>())
    ("m,mongo", "MongoDB URI", value<string>())
    ("h,help", "Print help");
  // clang-format on
  ParseResult options_parsed;
  try {
    options_parsed = options.parse(argc, argv);
  } catch (const std::exception &e) {
    cerr << e.what() << endl;
    cerr << fg::red << "Unknown CLI option" << fg::reset << '\n';
    cerr << options.help() << endl;
    return -1;
  }
  if (options_parsed.count("help")) {
    cout << options.help() << endl;
    return 0;
  }
  if (options_parsed.count("output")) {
    if (options_parsed.count("install")) {
      cerr << fg::red << "Cannot install and write to file at the same time"
           << fg::reset << endl;
      return -1;
    }
    output = options_parsed["output"].as<string>();
  }
  if (options_parsed.count("broker")) {
    data["broker"] = options_parsed["broker"].as<string>();
  }
  if (options_parsed.count("frontend")) {
    data["port_frontend"] = options_parsed["frontend"].as<int>();
  }
  if (options_parsed.count("backend")) {
    data["port_backend"] = options_parsed["backend"].as<int>();
  }
  if (options_parsed.count("settings")) {
    data["port_settings"] = options_parsed["settings"].as<int>();
  }
  if (options_parsed.count("fps")) {
    data["fps"] = options_parsed["fps"].as<int>();
  }
  if (options_parsed.count("mongo")) {
    data["mongo_uri"] = options_parsed["mongo"].as<string>();
  }
  if (output != "") {
    filesystem::path out_path{output};
    if (out_path.is_relative()) {
      out_path = filesystem::absolute(out_path);
    }
    auto path = out_path.parent_path();
    auto file = out_path.filename();
    auto tmp = filesystem::temp_directory_path();
    if (!filesystem::exists(path)) {
      cerr << fg::red << "Path " << path << " does not exist" << fg::reset
           << endl;
      return -1;
    }
    Environment env{template_dir + "/", tmp.string() + "/"};
    env.write("mads.ini", data, file.string());
    try {
      filesystem::copy(tmp / file, path / file);
      filesystem::remove(tmp / file);
    } catch (const filesystem::filesystem_error &e) {
      filesystem::remove(tmp / file);
      cerr << e.what() << endl;
      cerr << fg::red << "Cannot write or overwrite " << path / file
           << fg::reset << endl;
      return -1;
    }
    cout << fg::green << "INI file written to " << output << fg::reset << endl;
    return 0;
  } 
#ifndef _WIN32
  if (options_parsed.count("install")) {
    if (!filesystem::exists(etc_dir)) {
      try {
        filesystem::create_directory(etc_dir);
      } catch (const filesystem::filesystem_error &e) {
        cerr << fg::red << "Cannot create directory " << etc_dir
             << " (need sudo?)" << fg::reset << endl;
        return -1;
      }
    }
    auto etc = filesystem::absolute(etc_dir);
    auto tmp = filesystem::temp_directory_path();
    Environment env{template_dir + "/", tmp.string() + "/"};
    env.write("mads.ini", data, "mads.ini");
    try {
      filesystem::copy(tmp / "mads.ini", etc / "mads.ini");
      filesystem::remove(tmp / "mads.ini");
    } catch (const filesystem::filesystem_error &e) {
      filesystem::remove(tmp / "mads.ini");
      cerr << fg::red << "Cannot write or overwrite " << etc / "mads.ini"
           << fg::reset << endl;
      return -1;
    }
    cout << fg::green << "INI file installed to " << etc_dir << "/mads.ini"
         << fg::reset << endl;
    return 0;
  }
#endif
  Environment env{template_dir + "/", "."};
  cout << env.render_file("mads.ini", data) << endl;
  return 0;
}

/*
                      _                               _
  ___  ___ _ ____   _(_) ___ ___    ___ _ __ ___   __| |
 / __|/ _ \ '__\ \ / / |/ __/ _ \  / __| '_ ` _ \ / _` |
 \__ \  __/ |   \ V /| | (_|  __/ | (__| | | | | | (_| |
 |___/\___|_|    \_/ |_|\___\___|  \___|_| |_| |_|\__,_|

*/

int make_service(int argc, char **argv) {
  auto template_dir = Mads::exec_dir("../share/templates/");
  string this_exe = Mads::exec_path().stem().string();
  int start = 1;
  json data;
  string command_line = Mads::exec_dir() + "/";
  string args = "";
  if (argc < 3) {
    cerr << fg::red << "No enough arguments provided" << fg::reset << endl
         << "Usage: mads service <service name> <launch command>" << endl
         << "e.g.:  mads service mads-feedback feedback -s "
            "tcp://broker.local:9092"
         << endl;
    return -1;
  }
  for (int i = 1; i < argc; i++) {
    args += argv[i];
    args += " ";
  }
  data["service_name"] = string(MADS_PREFIX) + argv[1];
  argv++;
  argc--;
  if (argc < 2) {
    cerr << fg::red << "No service name provided" << fg::reset << endl;
    return -1;
  }
  if (strncmp(argv[1], MADS_PREFIX, strlen(MADS_PREFIX)) == 0) {
    argv++;
    argc--;
    data["name"] = argv[0];
  } else if (this_exe == argv[1]) {
    argv += 2;
    argc -= 2;
    command_line += MADS_PREFIX;
    data["name"] = string(MADS_PREFIX) + argv[0];
  } else {
    argv++;
    argc--;
    data["name"] = string(MADS_PREFIX) + argv[0];
    command_line += MADS_PREFIX;
  }
  for (int i = 0; i < argc; i++) {
    command_line += argv[i];
    command_line += " ";
  }
  data["command"] = command_line;
  data["systemd_path"] = SYSTEMD_PATH;
  data["ini_file"] = Mads::exec_dir("../etc/mads.ini");
  data["this_exe"] = this_exe;
  data["args"] = args;
  Environment env{template_dir + "/", "."};
  if (getuid() == 0) {
    string dest = string(SYSTEMD_PATH) + "/" +
                  data["service_name"].get<string>() + ".service";
    ofstream file(dest);
    file << env.render_file("service.tpl", data) << endl;
    file.close();
    cout << fg::green << "Service file written to " << dest << fg::reset
         << endl;
    cout << "Start it with " << style::bold << "sudo systemctl start "
         << data["service_name"].get<string>() << style::reset << endl;
    cout << "Enable it permanently with " << style::bold
         << "sudo systemctl enable " << data["service_name"].get<string>()
         << style::reset << endl;
  } else {
    cout << env.render_file("service.tpl", data) << endl;
  }
  return 0;
}


void update(const std::string &url) {
  cout << "Press Enter to open " << style::bold << style::underline << url
       << style::reset << " in your browser (Ctrl-C to cancel)..." << endl;
  string dummy;
  getline(cin, dummy);
#ifdef _WIN32
  string cmd = string("cmd /C start \"\" \"") + url + "\"";
#elif defined(__APPLE__)
  string cmd = string("open \"") + url + "\"";
#else
  string cmd = string("xdg-open \"") + url + "\" 2>/dev/null";
#endif
  int rc = std::system(cmd.c_str());
  if (rc != 0) {
    cerr << fg::red << "Failed to open browser (command returned " << rc
         << "). Please open " << style::bold << style::underline 
         << " manually." << fg::reset << style::reset << endl;
  }
}



/*
  __  __       _
 |  \/  | __ _(_)_ __
 | |\/| |/ _` | | '_ \
 | |  | | (_| | | | | |
 |_|  |_|\__,_|_|_| |_|

*/

int main(int argc, char **argv) {
  string exec_dir = Mads::exec_dir();
  auto template_dir = Mads::exec_dir("../share/templates/");
  bool force = false;

  vector<string> ext_commands;
  for (auto const &item : filesystem::directory_iterator(exec_dir)) {
    string filename = item.path().stem().string();
    if (filename.substr(0, strlen(MADS_PREFIX)) == MADS_PREFIX) {
      ext_commands.push_back(filename.substr(strlen(MADS_PREFIX)));
    }
  }

  if (argc > 1) {
    if (includes(ext_commands, argv[1])) {
#ifdef _WIN32
      cerr << "On Windows, please use the command " << style::bold << fg::green
           << MADS_PREFIX << argv[1] << style::reset << fg::reset << " directly"
           << endl;
      return -1;
#else
      string cmd = exec_dir + "/" + string(MADS_PREFIX) + argv[1];
      execv(cmd.c_str(), argv + 1);
#endif
    } else if (strncmp(argv[1], "ini", 3) == 0) {
      return make_ini(argc - 1, argv + 1);
    } else if (strncmp(argv[1], "update", 3) == 0) {
      update(RELEASE_URL);
      return 0;
    } else if (strncmp(argv[1], "beta", 3) == 0) {
      update(BETA_URL);
      return 0;
    }
#ifdef __linux__
    else if (strncmp(argv[1], "service", 7) == 0) {
      return make_service(argc - 1, argv + 1);
    }
#endif
  }

  // clang-format off
  Options options("mads", 
    "Mads command line interface version " + Mads::version());
  options.add_options()
    ("i,info", "Print information on MADS installation")
    ("p,prefix", "Print MADS linstall prefix")
    ("plugins", "List plugins in default plugins directory")
    ("keypair", "Generate ZMQ CURVE keypair", value<string>()->implicit_value("mads"))
    ("f,force", "Force operation (if applicable)")
    ("v,version", "Print version")
    ("h,help", "Print help");
  
  string plugins_dir = 
  #ifdef _WIN32
    Mads::exec_dir("../bin/");
  #else
    Mads::exec_dir("../lib/");
  #endif

  ParseResult options_parsed;
  // clang-format on
  try {
    options_parsed = options.parse(argc, argv);
  } catch (const std::exception &e) {
    cerr << e.what() << endl;
    cerr << fg::red << "Unknown CLI option" << fg::reset << '\n';
    cerr << options.help() << endl;
  }
  if (options_parsed.unmatched().size() > 0) {
    cerr << fg::red << "Unexpected CLI option: ";
    for (auto const &opt : options_parsed.unmatched()) {
      cerr << opt << " ";
    }
    cerr << fg::reset << endl;
    cerr << options.help() << endl;
    return -1;
  }

  if (options_parsed.count("force")) {
    force = true;
  }
  if (options_parsed.count("help")) {
    cout << options.help() << endl;
    return 0;
  }
  if (options_parsed.count("version")) {
    cout << Mads::version();
    regex ver_regex("^v\\d+\\.\\d+\\.\\d+$");
    string ver = Mads::version();
    if (std::regex_match(ver, ver_regex)) {
      cout << style::italic << " (build " << LIB_GIT_TAG << ")"
           << style::reset;
    }
    cout << endl;
    return 0;
  }
  if (options_parsed.count("info")) {
    cout << "Mads version: " << style::bold << Mads::version() << style::reset
         << endl;
    cout << "Mads binary directory: " << style::bold << exec_dir << style::reset
         << endl;
    cout << "Mads plugins directory: " << style::bold
         << plugins_dir<< style::reset << endl;
    cout << "Mads template directory: " << style::bold << template_dir
         << style::reset << endl;
    cout << "Mads INI file: " << style::bold
         << Mads::exec_dir("../etc/mads.ini") << style::reset << endl;
    return 0;
  }
  if (options_parsed.count("keypair")) {
    zmqpp::context context;
    string name = options_parsed["keypair"].as<string>();
    pair<string, string> key_files;
    if (!save_keypair(key_files, fs::current_path().string(), name, force)) {
      cerr << fg::red << "Keypair generation failed" << fg::reset << endl;
      return -1;
    }
    cout << fg::green << "Keypair generated:" << fg::reset << endl;
    cout << "  Secret key: " << style::bold << key_files.first << style::reset
         << endl;
    cout << "  Public key: " << style::bold << key_files.second << style::reset
         << endl;
    cout << "If this pair is for an agent, save both keys in the agent device."
         << endl;
    cout << "If this pair is for the broker, " << style::bold
         << "distribute only the public key to the clients." << style::reset
         << endl;
    cout << "Broker and agents look for the keys in the etc directory under "
            "the prefix directory (here "
         << Mads::prefix() << "/etc) by default." << endl;
    cout << fg::yellow << "Keep the secret key safe!" << fg::reset << endl;
    return 0;
  }
  if (options_parsed.count("prefix")) {
    cout << Mads::prefix() << endl;
    return 0;
  }
  if (options_parsed.count("plugins")) {
    cout << "Plugins directory: " << style::bold
         << plugins_dir << style::reset << endl;
    cout << "Available plugins:" << endl;
    if (!fs::exists(plugins_dir)) {
      cout << "No plugins found" << endl;
      return 0;
    }
    for (auto const &cmd : fs::directory_iterator(plugins_dir)) {
      if (cmd.path().extension() != ".plugin") {
        continue;
      }
      cout << "  " << cmd.path().filename().string() << endl;
    }
    return 0;
  }
#ifdef _WIN32
#define FIELD_WIDTH 15
  cout << style::bold << "Available executables and subcommands:" 
       << style::reset << endl;
  for (auto const &cmd : ext_commands) {
    cout << setw(FIELD_WIDTH) << string(MADS_PREFIX + cmd) << style::italic
         << " (external executable)" << style::reset << endl;
  }
  cout << style::bold << "Available mads subcommands:" << style::reset << endl;
#else
#define FIELD_WIDTH 11
  cout << style::bold << "Available mads subcommands:" << style::reset << endl;
  for (auto const &cmd : ext_commands) {
    cout << setw(FIELD_WIDTH) << cmd << style::italic << " (wraps "
         << MADS_PREFIX << cmd << ")" << style::reset << endl;
  }
#endif
  cout << setw(FIELD_WIDTH) << "ini" << style::italic << " (internal)"
       << style::reset << endl;
  cout << setw(FIELD_WIDTH) << "update" << style::italic << " (internal)"
       << style::reset << endl;
  cout << setw(FIELD_WIDTH) << "beta" << style::italic << " (internal)"
       << style::reset << endl;
#ifdef __linux__
  cout << setw(FIELD_WIDTH) << "service" << style::italic << " (internal)"
       << style::reset << endl;
#endif

  return 0;
}