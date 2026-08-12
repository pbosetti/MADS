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
#include "../https_client.hpp"
#include "../TerminalLogoRenderer.hpp"
#include "../service_discovery.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#endif
#include <cxxopts.hpp>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <inja/inja.hpp>
#include <iostream>
#include <cstdlib>
#include <optional>
#include <nlohmann/json.hpp>
#include <rang.hpp>
#include <zmq.hpp>
#include <zmq_addon.hpp>

#include "zap_auth.hpp"

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

#ifdef _WIN32
static std::wstring utf8_to_wide(const std::string &str) {
  if (str.empty()) return std::wstring();
  int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
  if (size <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(size) - 1, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, out.data(), size);
  return out;
}

static std::wstring quote_win_arg(const std::wstring &arg) {
  if (arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;

  std::wstring out = L"\"";
  size_t backslashes = 0;
  for (wchar_t c : arg) {
    if (c == L'\\') {
      backslashes++;
      continue;
    }
    if (c == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(L'"');
      backslashes = 0;
      continue;
    }
    if (backslashes > 0) {
      out.append(backslashes, L'\\');
      backslashes = 0;
    }
    out.push_back(c);
  }
  if (backslashes > 0) out.append(backslashes * 2, L'\\');
  out.push_back(L'"');
  return out;
}

static int run_windows_subcommand(const std::string &exec_dir, int argc, char **argv) {
  fs::path exe_path = fs::path(exec_dir) / (std::string(MADS_PREFIX) + argv[1]);
  if (!fs::exists(exe_path)) {
    fs::path with_ext = exe_path;
    with_ext += ".exe";
    if (fs::exists(with_ext)) exe_path = with_ext;
  }

  std::wstring exe_w = utf8_to_wide(exe_path.string());
  std::wstring cmd_line = quote_win_arg(exe_w);
  for (int i = 2; i < argc; ++i) {
    cmd_line.push_back(L' ');
    cmd_line += quote_win_arg(utf8_to_wide(argv[i]));
  }

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION pi{};

  std::wstring mutable_cmd_line = cmd_line;
  BOOL ok = CreateProcessW(exe_w.c_str(), mutable_cmd_line.data(), nullptr,
                           nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
  if (!ok) {
    cerr << fg::red << "Error: cannot execute subcommand '" << exe_path.string()
         << "' on Windows (CreateProcessW failed with error " << GetLastError()
         << ")." << fg::reset << endl;
    return -1;
  }

  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exit_code = 1;
  GetExitCodeProcess(pi.hProcess, &exit_code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return static_cast<int>(exit_code);
}
#endif

bool save_keypair(pair<string, string> &key_files, const string &path, const string &name, bool force=false) {
  try {
    Mads::CurveKeypair keypair = Mads::generate_keypair();
    key_files.first = ( fs::path(path) / (name + ".key") ).string();
    key_files.second = ( fs::path(path) / (name + ".pub") ).string();
    if (!force) {
      if (fs::exists(key_files.first) || fs::exists(key_files.second)) {
        cerr << fg::red << "Error: key files already exist. Use -f to overwrite."
             << fg::reset << endl;
        return false;
      }
    }
    ofstream key_file(key_files.first);
    ofstream pub_file(key_files.second);
    key_file << keypair.secret_key;
    pub_file << keypair.public_key;
    key_file.close();
    pub_file.close();
  } catch (const zmq::error_t &e) {
    cerr << fg::red << "Error: cannot generate CURVE keypair: " << e.what()
         << fg::reset << endl;
    cerr << fg::yellow
         << "This build of libzmq does not support CURVE (libsodium). "
         << "Rebuild with ENABLE_CURVE=ON and WITH_LIBSODIUM=ON."
         << fg::reset << endl;
    return false;
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
  _   _           _       _       
 | | | |_ __   __| | __ _| |_ ___ 
 | | | | '_ \ / _` |/ _` | __/ _ \
 | |_| | |_) | (_| | (_| | ||  __/
  \___/| .__/ \__,_|\__,_|\__\___|
       |_|                        
*/

void describe_release(const json &release, bool show_running = true) {
  std::string plat, arch;
#ifdef _WIN32
  plat = "Windows-";
#elif defined(__APPLE__)
  plat = "Darwin-";
#else
  plat = "Linux-";
#endif

#ifdef __x86_64__
  arch = "x86_64";
#elif defined(__aarch64__) and defined(__APPLE__)
  arch = "arm64";
#elif defined(__aarch64__) 
  arch = "aarch64";
#elif defined(__arm__)
  arch = "arm";
#elif defined(_M_X64) and defined(_WIN32)
  arch = "AMD64";
#elif defined(_M_X64)
  arch = "x86_64";
#elif defined(_M_ARM64)
  arch = "aarch64";
#else
  arch = "unknown";
#endif
  if (show_running)
    cout << "You are running MADS " << style::bold << LIB_GIT_TAG
         << style::reset << endl;
  cout << "Latest ";
  if (release["prerelease"])
    cout << fg::yellow << "pre-release ";
  else
    cout << "release ";
  cout << style::bold
       << release.value("tag_name", "no tag") << style::reset << fg::reset 
       << " has " << release["assets"].size() << " assets:" << endl;
  for (json const &a : release["assets"]) {
    if (regex_match(a.value("name", ""), regex(".*" + plat + arch + ".*")) ||
        regex_match(a.value("name", ""), regex(".*" + plat + "universal.*")))
      cout << fg::green << style::bold << "=> ";
    else
      cout << " - ";
    cout << a.value("browser_download_url", "unnamed") << ", "
         << style::reset << style::italic << a.value("size", 0) / 1024 << " kbytes" 
         << fg::reset << style::reset << endl;
  }
  cout << "Release URL: " << release.value("html_url", "unknown") << endl;
  cout << style::italic << "URLs in green match current device (" 
       << fg::green << plat + arch << fg::reset << ")" << style::reset << endl;
}

// Fetch and parse a GitHub API endpoint as JSON, retrying up to n times on
// transient errors. Returns std::nullopt if all attempts fail.
static std::optional<json> github_get_json(
    const std::string &path,
    const std::vector<std::pair<std::string, std::string>> &queries,
    size_t n) {
  for (size_t i = 1; i <= n; i++) {
    try {
      Mads::HttpsClient client;
      client.set_hostname("api.github.com");
      client.set_path(path);
      for (auto const &[key, value] : queries)
        client.add_query_pair(key, value);
      client.set_user_agent("MADS" LIB_VERSION);
      return json::parse(client.get().body);
    } catch (const json::exception &e) {
      cerr << "Error fetching info, retry " << i << "/" << n << endl;
      cerr << "Error: " << e.what() << endl;
      this_thread::sleep_for(chrono::milliseconds(500));
    } catch (const std::exception &e) {
      cerr << "Unexpected error: " << e.what() << endl;
    }
  }
  return std::nullopt;
}

void check_update(bool beta = false, size_t n = 3) {
  // `mads beta`: report the single most recent release, prerelease or not.
  if (beta) {
    auto releases =
        github_get_json("/repos/pbosetti/mads/releases", {{"per_page", "1"}}, n);
    if (releases && releases->is_array() && !releases->empty())
      describe_release((*releases)[0]);
    return;
  }

  // `mads update`: report the latest stable release...
  auto latest =
      github_get_json("/repos/pbosetti/mads/releases/latest", {}, n);
  if (!latest)
    return;
  describe_release(*latest);

  // ...then, in a second section, the latest pre-release if it was published
  // after that stable release. The releases list is sorted newest-first, so the
  // first non-draft pre-release encountered is the most recent one.
  auto releases =
      github_get_json("/repos/pbosetti/mads/releases", {{"per_page", "30"}}, n);
  if (!releases || !releases->is_array())
    return;
  const std::string latest_published = latest->value("published_at", "");
  for (json const &rel : *releases) {
    if (rel.value("draft", false) || !rel.value("prerelease", false))
      continue;
    if (rel.value("published_at", "") > latest_published) {
      cout << endl
           << style::bold << fg::magenta
           << "A newer pre-release is also available:" << fg::reset
           << style::reset << endl;
      describe_release(rel, false);
    }
    break;
  }
}


void update(const std::string &url) {
  cout << style::italic << "This is MADS " << style::bold
       << LIB_GIT_TAG << style::reset
       << "\nPress Enter to open " << style::bold << style::underline << url
       << style::reset << " in your browser\n(or Ctrl-C to cancel)..." << endl;
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
                      _                               _
  ___  ___ _ ____   _(_) ___ ___    ___ _ __ ___   __| |
 / __|/ _ \ '__\ \ / / |/ __/ _ \  / __| '_ ` _ \ / _` |
 \__ \  __/ |   \ V /| | (_|  __/ | (__| | | | | | (_| |
 |___/\___|_|    \_/ |_|\___\___|  \___|_| |_| |_|\__,_|

*/

int make_service(int argc, char **argv) {
  auto template_dir = Mads::exec_dir("../share/templates/");
  string this_exe = Mads::exec_path().stem().string();
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



/*
            _                                  _   _
  ___  ___ | |_ _   _ _ __        _ __  _   _| |_| |__   ___  _ __
 / __|/ _ \| __| | | | '_ \ _____| '_ \| | | | __| '_ \ / _ \| '_ \
 \__ \  __/| |_| |_| | |_) |_____| |_) | |_| | |_| | | | (_) | | | |
 |___/\___| \__|\__,_| .__/      | .__/ \__, |\__|_| |_|\___/|_| |_|
                     |_|         |_|    |___/
*/

// Run a shell command and capture its trimmed stdout. Returns nullopt if the
// process cannot be spawned or exits with a non-zero status.
static std::optional<std::string> capture_output(const std::string &cmd) {
#ifdef _WIN32
  FILE *pipe = _popen(cmd.c_str(), "r");
#else
  FILE *pipe = popen(cmd.c_str(), "r");
#endif
  if (!pipe)
    return std::nullopt;
  std::string out;
  std::array<char, 512> buffer{};
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
    out += buffer.data();
#ifdef _WIN32
  int rc = _pclose(pipe);
#else
  int rc = pclose(pipe);
#endif
  if (rc != 0)
    return std::nullopt;
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r' ||
                          out.back() == ' ' || out.back() == '\t'))
    out.pop_back();
  return out;
}

// Locate the Python interpreter inside an environment root, probing the layout
// differences between venv/uv and conda on Unix and Windows.
static fs::path find_python(const fs::path &env_root) {
  std::vector<fs::path> candidates{
#ifdef _WIN32
      env_root / "Scripts" / "python.exe", // venv / uv
      env_root / "python.exe"              // conda env root
#else
      env_root / "bin" / "python3", env_root / "bin" / "python"
#endif
  };
  for (auto const &c : candidates)
    if (fs::exists(c))
      return c;
  return {};
}

int setup_python(int argc, char **argv) {
  Options options("mads setup-python",
                  "Install the MADS Python wrapper into a Python environment, "
                  "version " +
                      Mads::version());
  // clang-format off
  options.add_options()
    ("venv", "Path to a venv (or uv) environment root", value<string>())
    ("conda", "Path to a conda environment root", value<string>())
    ("uv", "Path to a uv-managed venv root", value<string>())
    ("h,help", "Print help");
  // clang-format on
  ParseResult parsed;
  try {
    parsed = options.parse(argc, argv);
  } catch (const std::exception &e) {
    cerr << e.what() << endl;
    cerr << fg::red << "Unknown CLI option" << fg::reset << '\n';
    cerr << options.help() << endl;
    return -1;
  }
  if (parsed.count("help")) {
    cout << options.help() << endl;
    return 0;
  }

  // 1. Determine the target environment root and kind.
  string env_root, kind;
  size_t n_flags =
      parsed.count("venv") + parsed.count("conda") + parsed.count("uv");
  if (n_flags > 1) {
    cerr << fg::red << "Specify only one of --venv, --conda or --uv."
         << fg::reset << endl;
    return -1;
  }
  if (parsed.count("venv")) {
    env_root = parsed["venv"].as<string>();
    kind = "venv";
  } else if (parsed.count("uv")) {
    env_root = parsed["uv"].as<string>();
    kind = "uv";
  } else if (parsed.count("conda")) {
    env_root = parsed["conda"].as<string>();
    kind = "conda";
  } else {
    const char *venv = getenv("VIRTUAL_ENV");
    const char *conda = getenv("CONDA_PREFIX");
    if (venv && *venv) {
      env_root = venv;
      kind = "venv";
    } else if (conda && *conda) {
      env_root = conda;
      kind = "conda";
    } else {
      cerr << fg::red << "No active Python environment detected." << fg::reset
           << endl;
      cerr << "Activate a venv/conda/uv environment, or pass one explicitly:"
           << endl;
      cerr << "  mads setup-python --venv=/path/to/venv" << endl;
      cerr << "  mads setup-python --conda=/path/to/conda/env" << endl;
      cerr << "  mads setup-python --uv=/path/to/uv/venv" << endl;
      return -1;
    }
  }

  fs::path env_path = fs::absolute(env_root);
  if (!fs::exists(env_path)) {
    cerr << fg::red << "Environment path does not exist: " << env_path.string()
         << fg::reset << endl;
    return -1;
  }

  // 2. Find the interpreter inside the environment.
  fs::path python = find_python(env_path);
  if (python.empty()) {
    cerr << fg::red << "No Python interpreter found under " << env_path.string()
         << fg::reset << endl;
    return -1;
  }

  // 3. Locate the installed wrapper (kept in an isolated directory so the .pth
  //    entry exposes only mads_agent, not the other helper scripts).
  fs::path wrapper_dir = Mads::exec_dir("../share/mads-python");
  fs::path wrapper = fs::path(wrapper_dir) / "mads_agent.py";
  if (!fs::exists(wrapper)) {
    cerr << fg::red << "Cannot find the MADS Python wrapper at "
         << wrapper.string() << fg::reset << endl;
    cerr << "Is this a complete MADS installation?" << endl;
    return -1;
  }

  // 4. Ask the interpreter for its site-packages directory. A temp probe file
  //    avoids fragile nested-quote escaping across shells.
  fs::path tmp = fs::temp_directory_path();
  fs::path probe = tmp / "mads_setup_sitepkg.py";
  {
    ofstream f(probe);
    f << "import sysconfig\nprint(sysconfig.get_path('purelib'))\n";
  }
  string probe_cmd = "\"" + python.string() + "\" \"" + probe.string() + "\"";
  auto purelib = capture_output(probe_cmd);
  error_code ec;
  fs::remove(probe, ec);
  if (!purelib || purelib->empty()) {
    cerr << fg::red << "Could not determine site-packages for " << python.string()
         << fg::reset << endl;
    return -1;
  }

  // 5. Write the .pth file pointing at the wrapper directory.
  fs::path pth = fs::path(*purelib) / "mads_agent.pth";
  {
    ofstream f(pth);
    if (!f) {
      cerr << fg::red << "Cannot write " << pth.string()
           << " (insufficient permissions?)" << fg::reset << endl;
      return -1;
    }
    f << wrapper_dir.string() << "\n";
  }

  // 6. Verify the import works.
  fs::path vprobe = tmp / "mads_setup_import.py";
  {
    ofstream f(vprobe);
    f << "import mads_agent\n";
  }
  string verify_cmd = "\"" + python.string() + "\" \"" + vprobe.string() + "\"";
  auto verified = capture_output(verify_cmd);
  fs::remove(vprobe, ec);

  cout << fg::green << "MADS Python wrapper installed for " << kind
       << " environment:" << fg::reset << endl;
  cout << "  Environment: " << style::bold << env_path.string() << style::reset
       << endl;
  cout << "  Interpreter: " << style::bold << python.string() << style::reset
       << endl;
  cout << "  Path file:   " << style::bold << pth.string() << style::reset
       << "\n               -> " << wrapper_dir.string() << endl;
  if (verified) {
    cout << fg::green << "  Verified: import mads_agent works." << fg::reset
         << endl;
  } else {
    cout << fg::yellow
         << "  Warning: could not verify the import (see errors above)."
         << fg::reset << endl;
  }
  cout << "You can now use " << style::bold << "from mads_agent import Agent"
      << style::reset << " in this environment." << endl
      << "This is a one-time setup; the wrapper will remain available in " 
      << "this environment;" << endl
      << "being a .pth file, it will automatically provide "
      << "the latest MADS installed version." << endl;
  return 0;
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
  auto image_dir = Mads::exec_dir("../share/images/");
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
      return run_windows_subcommand(exec_dir, argc, argv);
#else
      string cmd = exec_dir + "/" + string(MADS_PREFIX) + argv[1];
      execv(cmd.c_str(), argv + 1);
#endif
    } else if (strncmp(argv[1], "ini", 3) == 0) {
      return make_ini(argc - 1, argv + 1);
    } else if (strncmp(argv[1], "update", 3) == 0) {
      check_update(false);
      return 0;
    } else if (strncmp(argv[1], "beta", 3) == 0) {
      check_update(true);
      return 0;
    } else if (strcmp(argv[1], "setup-python") == 0) {
      return setup_python(argc - 1, argv + 1);
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
    ("rooms", "List rooms advertised on the network", value<size_t>()->implicit_value("5000"))
    ("j,json", "Output in JSON format (where applicable)")
    ("v,version", "Print version")
    ("h,help", "Print help");
  // clang-format on

  string plugins_dir = 
  #ifdef _WIN32
    Mads::exec_dir("../bin/");
  #else
    Mads::exec_dir("../lib/");
  #endif

  ParseResult options_parsed;
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
    if (options_parsed.count("json")) {
      json j_info;
      j_info["version"] = Mads::version();
      j_info["exec_dir"] = exec_dir;
      j_info["plugins_dir"] = plugins_dir;
      j_info["template_dir"] = template_dir;
      j_info["ini_file"] = Mads::exec_dir("../etc/mads.ini");
      cout << j_info.dump(2) << endl;
    } else {
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
      cout << style::italic << "Run mads update or mads beta to check for updates"
          << style::reset << endl;
    }
    return 0;
  }
  if (options_parsed.count("keypair")) {
    zmq::context_t context;
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
  if (options_parsed.count("rooms")) {
    Mads::ServiceDiscovery discovery;
    if (!options_parsed.count("json")) {
      cout << "Discovering rooms on the network (timeout: " 
           << options_parsed["rooms"].as<size_t>() << " ms)..." << endl;
    }
    auto rooms = discovery.list_rooms(std::chrono::milliseconds(options_parsed["rooms"].as<size_t>()));
    if (rooms.empty()) {
      cout << "No rooms found" << endl;
      return 0;
    }
    map<string, size_t> field_widths{{"name", string("Room name:").size()},
                                     {"host", string("Host:").size()},
                                     {"url", string("Settings URL:").size()},
                                     {"version", string("Version:").size()}};
    for (auto const &room : rooms) {
      field_widths["name"] = max(field_widths["name"], room.first.size()) + 2;
      field_widths["host"] =
          max(field_widths["host"], room.second.hostname.size()) + 2;
      string url = "tcp://" + room.second.ip + ":" +
                   std::to_string(room.second.ports.at("settings"));
      field_widths["url"] = max(field_widths["url"], url.size()) + 2;
      field_widths["version"] =
          max(field_widths["version"], room.second.version.size());
    }
    if (options_parsed.count("json")) {
      json j_rooms = json::array();
      for (auto const &room : rooms) {
        json j_room;
        j_room["name"] = room.first;
        j_room["host"] = room.second.hostname;
        j_room["ip"] = room.second.ip;
        j_room["ports"] = room.second.ports;
        j_room["version"] = room.second.version;
        j_rooms.push_back(j_room);
      }
      cout << j_rooms.dump(2) << endl;
      return 0;
    }
    cout << "Rooms advertised on the network:" << endl << style::bold;
    cout << setw(field_widths["name"]) << left
         << "Room name:" << setw(field_widths["host"]) << left
         << "Host:" << setw(field_widths["url"]) << left
         << "Settings URL:" << setw(field_widths["version"]) << left
         << "Version:" << endl
         << style::reset;
    for (auto const &room : rooms) {
      cout << setw(field_widths["name"]) << left << room.first
           << setw(field_widths["host"]) << left << room.second.hostname
           << setw(field_widths["url"]) << left
           << "tcp://" + room.second.ip + ":" +
                  std::to_string(room.second.ports.at("settings"))
           << setw(field_widths["version"]) << left << room.second.version
           << endl;
    }
    return 0;
  }

#define FIELD_WIDTH 15
#ifndef _WIN32
  auto logo = filesystem::path(image_dir) / "logo_white.png";
  if(filesystem::exists(logo)) {
    terminal_logo::TerminalLogoRenderer::Options options;
    options.width = 80;
    options.mode = terminal_logo::TerminalLogoRenderer::ColorMode::Auto;
    terminal_logo::TerminalLogoRenderer::render_from_path(logo, std::cout, options);
  }
#endif
  cout  << fg::green << "MADS - Multi-Agent Distributed System" 
        << fg::reset << endl
        << "See " << style::italic << fg::blue 
        << "https://mads-net.github.io" << fg::reset
        << style::reset << " for help and guides" << endl
        << style::bold << "Available mads subcommands:" 
        << style::reset << endl;
  for (auto const &cmd : ext_commands) {
    cout << setw(FIELD_WIDTH) << cmd << style::italic << " (wraps "
         << MADS_PREFIX << cmd << ")" << style::reset << endl;
  }
  cout << setw(FIELD_WIDTH) << "ini" << style::italic << " (internal)"
       << style::reset << endl;
  cout << setw(FIELD_WIDTH) << "update" << style::italic << " (internal)"
       << style::reset << endl;
  cout << setw(FIELD_WIDTH) << "beta" << style::italic << " (internal)"
       << style::reset << endl;
  cout << setw(FIELD_WIDTH) << "setup-python" << style::italic << " (internal)"
       << style::reset << endl;
#ifdef __linux__
  cout << setw(FIELD_WIDTH) << "service" << style::italic << " (internal)"
       << style::reset << endl;
#endif

  return 0;
}
