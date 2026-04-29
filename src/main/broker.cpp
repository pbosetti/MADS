/*
  ____            _
 | __ ) _ __ ___ | | _____ _ __
 |  _ \| '__/ _ \| |/ / _ \ '__|
 | |_) | | | (_) |   <  __/ |
 |____/|_|  \___/|_|\_\___|_|

The broker executable. It redirects all messages from the frontend socket to
the backend socket.
The endpoints are defined in the settings file.

Author(s): Paolo Bosetti
*/
// clang-format off
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#endif
#include "../exec_path.hpp"
#include "../mads.hpp"
#include "../watcher.hpp"
#include "../curve.hpp"
#include "../keypress.hpp"
#include "../goback.hpp"
#include "../service_discovery.hpp"
#include <cstring>
#include <cxxopts.hpp>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <vector>
#include <map>
#include <rang.hpp>
#include <regex>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <toml++/toml.hpp>
#include <zmqpp/proxy.hpp>
#include <zmqpp/proxy_steerable.hpp>
#include <zmqpp/zmqpp.hpp>
#ifdef _WIN32
#include <iphlpapi.h>
#include <signal.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <sys/ioctl.h>
#include <termios.h>
#endif



#if defined(__linux__)
#include <linux/if.h>
#elif defined(__APPLE__)
#include <net/if.h>
#endif
// clang-format on

using namespace std::string_view_literals;
using namespace std;
using namespace cxxopts;
using namespace rang;
using namespace Mads;

/*
  _   _ _   _ _ _ _
 | | | | |_(_) (_) |_ _   _
 | | | | __| | | | __| | | |
 | |_| | |_| | | | |_| |_| |
  \___/ \__|_|_|_|\__|\__, |
                      |___/
*/

#ifdef _WIN32
void relaunch() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring cmd_line = GetCommandLineW();

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};

  std::wstring cmd_buffer = cmd_line;

  bool ok = CreateProcessW(path, cmd_buffer.data(), nullptr, nullptr, FALSE, 0,
                           nullptr, nullptr, &si, &pi);
  if (ok) {
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
  }
}
#endif

map<string, string> list_ip() {
  map<string, string> result;
#ifdef _WIN32
  ULONG out_buf_len = sizeof(IP_ADAPTER_INFO);
  PIP_ADAPTER_INFO adapter_info =
      static_cast<PIP_ADAPTER_INFO>(malloc(out_buf_len));
  if (adapter_info == nullptr) {
    cerr << "Error allocating memory needed to call GetAdaptersInfo" << endl;
    return result;
  }

  DWORD ret = GetAdaptersInfo(adapter_info, &out_buf_len);
  if (ret == ERROR_BUFFER_OVERFLOW) {
    free(adapter_info);
    adapter_info = static_cast<PIP_ADAPTER_INFO>(malloc(out_buf_len));
    if (adapter_info == nullptr) {
      cerr << "Error allocating memory needed to call GetAdaptersInfo" << endl;
      return result;
    }
    ret = GetAdaptersInfo(adapter_info, &out_buf_len);
  }

  if (ret == NO_ERROR) {
    for (PIP_ADAPTER_INFO adapter = adapter_info; adapter != nullptr;
         adapter = adapter->Next) {
      result[std::to_string(adapter->Index)] =
          adapter->IpAddressList.IpAddress.String;
    }
  } else {
    cerr << "GetAdaptersInfo failed with error: " << ret << endl;
  }

  free(adapter_info);
#else
  struct ifaddrs *ptr_ifaddrs = nullptr;
  if (getifaddrs(&ptr_ifaddrs) != 0) {
    cerr << "`getifaddrs()` failed: " << strerror(errno) << endl;
    return result;
  }

  for (struct ifaddrs *ptr_entry = ptr_ifaddrs; ptr_entry != nullptr;
       ptr_entry = ptr_entry->ifa_next) {
    if (ptr_entry->ifa_addr == nullptr ||
        ptr_entry->ifa_addr->sa_family != AF_INET) {
      continue;
    }

    char buffer[INET_ADDRSTRLEN] = {0};
    auto *addr = reinterpret_cast<struct sockaddr_in *>(ptr_entry->ifa_addr);
    if (inet_ntop(AF_INET, &addr->sin_addr, buffer, sizeof(buffer)) !=
        nullptr) {
      result[ptr_entry->ifa_name] = buffer;
    }
  }

  freeifaddrs(ptr_ifaddrs);
#endif
  return result;
}

map<string, string> settings_urls(string const &base_url) {
  map<string, string> urls;
  string port = base_url.substr(base_url.find_last_of(":") + 1);
  auto ips = list_ip();
  for (const auto &[name, address] : ips) {
    urls[name] = "tcp://" + address + ":" + port;
  }
  return urls;
}

void print_instructions() {
  cout << style::italic << "CTRL-C to immediate exit" << style::reset << endl;

  cout << fg::green
       << "Type P to pause, R to resume, I for information, Q to clean quit\n"
       << "N to show next IP address"
#ifndef _WIN32
       << ", X to restart and reload settings"
#endif
       << fg::reset << endl
       << endl;
}

string timestamp() {
  auto now = chrono::system_clock::now();
  auto in_time_t = chrono::system_clock::to_time_t(now);

  stringstream ss;
  ss << put_time(localtime(&in_time_t), "[%Y-%m-%d %H:%M:%S] ");
  return ss.str();
}

#ifdef __linux__
#include <algorithm> // std::reverse()
#include <endian.h>  // __BYTE_ORDER __LITTLE_ENDIAN

template <typename T> constexpr unsigned long long htonll(T value) noexcept {
#if __BYTE_ORDER == __LITTLE_ENDIAN
  char *ptr = reinterpret_cast<char *>(&value);
  std::reverse(ptr, ptr + sizeof(T));
#endif
  return (unsigned long long)value;
}
#endif

void proxy(zmqpp::socket &frontend, zmqpp::socket &backend,
           zmqpp::socket &ctrl) {
  zmqpp::proxy_steerable(frontend, backend, ctrl);
}

std::string &read_settings_file(std::string const &settings_path) {
  static std::string ini_table_content = "";
  std::ifstream t(settings_path);
  std::stringstream buffer;
  buffer << t.rdbuf();
  ini_table_content = buffer.str();
  return ini_table_content;
}

/*
  __  __       _
 |  \/  | __ _(_)_ __
 | |\/| |/ _` | | '_ \
 | |  | | (_| | | | | |
 |_|  |_|\__,_|_|_| |_|

*/

int main(int argc, char **argv) {
  bool running = true, reload = false;
  string settings_path = SETTINGS_PATH;
  filesystem::path prefix(Mads::exec_dir(".."));
  filesystem::path keys_dir = prefix / "etc";
  unique_ptr<Mads::CurveAuth> curve_auth_ptr = nullptr;
  Options options(argv[0]);
  string nic = "lo0";
  string key_name;
  bool crypto = false;
  bool daemon = false;
  vector<string> desc{"FRONTEND msg in   ", "FRONTEND bytes in ",
                      "FRONTEND msg out  ", "FRONTEND bytes out",
                      "BACKEND msg in    ", "BACKEND bytes in  ",
                      "BACKEND msg out   ", "BACKEND bytes out "};
  ServiceDiscovery discovery_service(MADS_SERVICE_PORT);
  ServiceDiscovery::ServiceInfo service_info{
      .room = MADS_SERVICE_ROOM, .note = "CURVE encryption disabled"};

  // clang-format off
  options.add_options()
    ("s,settings", "Settings file path", value<string>())
    ("d,daemon", "Run as daemon")
    ("crypto", "Enable CURVE encryption (requires proper setup)", value<string>()->implicit_value("broker"))
    ("keys_dir", "Directory containing CURVE keypairs",
      value<string>()->implicit_value(keys_dir.string()))
    ("r,room", "Service discovery room name", value<string>()->implicit_value(MADS_SERVICE_ROOM))
    ("v,version", "Print version")
    ("h,help", "Print usage");
  // clang-format on
  auto options_parsed = options.parse(argc, argv);

  if (options_parsed.count("help")) {
    cout << argv[0] << " ver. " << LIB_VERSION << endl;
    cout << options.help() << endl;
    return 0;
  }
  if (options_parsed.count("version")) {
    cout << LIB_VERSION << endl;
    return 0;
  }
  if (options_parsed.count("daemon")) {
    daemon = true;
  }
  if (options_parsed.count("settings") != 0) {
    settings_path = options_parsed["settings"].as<string>();
  } else {
    struct stat buf;
    if (stat(settings_path.c_str(), &buf) != 0)
      settings_path = Mads::exec_dir("../etc/" SETTINGS_PATH);
  }
  if (options_parsed.count("room") != 0) {
    service_info.room = options_parsed["room"].as<string>();
  }

  filesystem::path executable(argv[0]);
  string name = executable.stem().string();
  name = name.substr(name.find_last_of("-") + 1);
  cout << style::italic << "Reading settings from " << style::bold
       << settings_path << " [" << name << "]" << style::reset << endl;

  // Configurations
  toml::table config;

  try {
    config = toml::parse_file(settings_path);
    static_cast<void>(config);
  } catch (const toml::parse_error &err) {
    cout << fg::red << "Cannot open settings file " << settings_path << ", "
         << err << style::reset << endl;
    exit(EXIT_FAILURE);
  }

  if (options_parsed.count("crypto") != 0) {
    key_name = options_parsed["crypto"].as<string>();
    if (options_parsed.count("keys_dir") != 0) {
      keys_dir = filesystem::path(options_parsed["keys_dir"].as<string>());
    }
    cout << fg::cyan << "Enabling CURVE encryption for broker sockets" << endl
         << "  Searching for keys in " << style::bold << keys_dir.string()
         << style::reset << endl
         << fg::cyan << "  Broker key name: " << style::bold << key_name
         << "[.key|.pub]" << fg::reset << endl;
    crypto = true;
    service_info.note = "CURVE encryption enabled";
  }

  unsigned int timecode_fps =
      config["agents"]["timecode_fps"].value_or(MADS_FPS);

  string frontend_address, backend_address, settings_address;
  frontend_address = config[name]["frontend_address"].value_or(BROKER_FRONTEND);
  backend_address = config[name]["backend_address"].value_or(BROKER_BACKEND);
  settings_address = config[name]["settings_address"].value_or(BROKER_SETTINGS);

  service_info.ports = {
      {"frontend",
       stoi(frontend_address.substr(frontend_address.find_last_of(":") + 1))},
      {"backend",
       stoi(backend_address.substr(backend_address.find_last_of(":") + 1))},
      {"settings",
       stoi(settings_address.substr(settings_address.find_last_of(":") + 1))}};
  service_info.prefer_loopback_for_local_services =
      config[name]["prefer_loopback_for_local_services"].value_or(false);

  // Create broker sockets
  zmqpp::context context;
  zmqpp::socket frontend(context, zmqpp::socket_type::xsub);
  zmqpp::socket backend(context, zmqpp::socket_type::xpub);
  if (crypto) {
    auto whitelist = config[name]["ip_whitelist"].as_array();
    bool verbose = config[name]["auth_verbose"].value_or(false);
    curve_auth_ptr = make_unique<Mads::CurveAuth>(context);
    if (whitelist) {
      whitelist->for_each([&](const toml::node &n) {
        if (toml::is_string<decltype(n)>) {
          curve_auth_ptr->allowed_ips.push_back(n.as_string()->get());
        }
      });
    }
    curve_auth_ptr->setup_auth(verbose ? Mads::auth_verbose::on
                                       : Mads::auth_verbose::off);
    try {
      curve_auth_ptr->fetch_public_keys(keys_dir);
    } catch (const runtime_error &e) {
      cerr << fg::red << "Error setting up CURVE authentication: " << e.what()
           << fg::reset << endl;
      curve_auth_ptr = nullptr;
      frontend.close();
      backend.close();
      context.terminate();
      exit(EXIT_FAILURE);
    }
    try {
      curve_auth_ptr->setup_curve_server(frontend, key_name);
      curve_auth_ptr->setup_curve_server(backend, key_name);
    } catch (const runtime_error &e) {
      cerr << fg::red << e.what() << fg::reset << endl;
      curve_auth_ptr = nullptr;
      frontend.close();
      backend.close();
      context.terminate();
      exit(EXIT_FAILURE);
    }
  }

  try {
    std::cout << "Binding broker frontend (XSUB) at " << style::bold
              << frontend_address << style::reset << endl;
    frontend.bind(frontend_address);
    std::cout << "Binding broker backend (XPUB) at " << style::bold
              << backend_address << style::reset << endl;
    backend.bind(backend_address);
  } catch (const zmqpp::zmq_internal_exception &e) {
    cerr << fg::red << "ZMQ error, could not connect: " << e.what() << fg::reset
         << endl;
    exit(EXIT_FAILURE);
  }

  // Create Settings socket (Req/Rep)
  zmqpp::socket settings(context, zmqpp::socket_type::rep);
  if (crypto)
    curve_auth_ptr->setup_curve_server(settings, key_name);
  settings.bind(settings_address);
  settings.set(zmqpp::socket_option::receive_timeout, 1000);
  cout << "Binding broker shared settings (REP) at " << style::bold
       << settings_address << style::reset << endl;
  string ini_table = read_settings_file(settings_path);
  std::mutex ini_table_mutex;
  thread settings_thread([&]() {
    while (running) {
      zmqpp::message msg;
      zmqpp::message content;
      content << LIB_VERSION;
      if (settings.receive(msg)) {
        if (msg.parts() < 2) {
          cerr << goback(1, !daemon) << fg::red << timestamp()
               << "Received malformed message from agent, "
               << "expected at least 2 parts" << fg::reset << endl;
          continue;
        }
        string agent_version = msg.get(0);
        string cmd = msg.get(1);
        string agent_name = "unknown";
        if (msg.parts() == 3) {
          agent_name = msg.get(2);
        }
        if (cmd == "settings") {
          if (!Mads::check_version(agent_version)) {
            cerr << goback(1, !daemon) << fg::red << timestamp()
                 << "Received settings request from agent with wrong version: "
                 << agent_version << " (vs. " << LIB_VERSION << ")" << fg::reset
                 << endl;
          } else {
            cout << goback(1, !daemon) << timestamp()
                 << "Sending settings to agent " << agent_name << " ("
                 << agent_version << ")" << endl;
            {
              std::lock_guard<std::mutex> lock(ini_table_mutex);
              content << ini_table;
            }
            string attachment_path =
                config[agent_name]["attachment"].value_or("");
            if (!attachment_path.empty()) {
              if (filesystem::path(attachment_path).is_relative()) {
                attachment_path = Mads::exec_dir(attachment_path);
              }
              if (!filesystem::exists(attachment_path)) {
                cerr << goback(1, !daemon) << fg::red << timestamp()
                     << "Attachment path does not exist: " << attachment_path
                     << fg::reset << endl;
              } else {
                ifstream attachment_file(attachment_path,
                                         ios::in | ios::binary);
                stringstream attachment_content;
                cout << goback(1, !daemon) << fg::yellow << timestamp()
                     << "  Attaching binary object: " << style::bold
                     << attachment_path << " ("
                     << filesystem::file_size(attachment_path) << " bytes)"
                     << fg::reset << endl;
                attachment_content << attachment_file.rdbuf();
                content << attachment_content.str();
              }
            }
          }
          settings.send(content);
        } else if (cmd == "timecode") {
          chrono::system_clock::time_point now = chrono::system_clock::now();
          settings.send(to_string(Mads::timecode(now, timecode_fps)));
        } else {
          cerr << goback(1, !daemon) << fg::yellow << timestamp()
               << "Got unexpected command " << cmd << fg::reset << endl;
          settings.send(content);
        }
      }
    }
    settings.close();
  });

  cout << "Timecode FPS: " << style::bold << timecode_fps << style::reset
       << endl;

  // print settings URI for clients
  string port = settings_address.substr(settings_address.find_last_of(":") + 1);
  cout << "Settings are provided on " << style::bold
       << "tcp://127.0.0.1:" << port << style::reset << style::italic
       << " (loopback)" << style::reset << endl;

  discovery_service.start_advertising(
      service_info, std::chrono::milliseconds(
                        config[name]["discovery_interval_ms"].value_or(1000)));
  cout << "Advertising service on UDP discovery port " << MADS_SERVICE_PORT
       << " with room name '" << style::bold << service_info.room
       << style::reset << "'" << endl
       << "            note: " << service_info.note << endl;
  if (service_info.prefer_loopback_for_local_services) {
    cout << style::italic << "            (preferring loopback for local agents)"
         << style::reset;
  }
  cout << endl;
  thread([&]() {
    Mads::Watcher watcher(settings_path, 1s);
    string ini_tmp = "";
    watcher.watch([&](const std::string &file_name) {
      cout << goback(1, !daemon) << fg::yellow << timestamp()
           << "Reloading settings " << file_name << "... ";
      ini_tmp = read_settings_file(settings_path);
      try {
        auto i = toml::parse(ini_tmp);
        {
          std::lock_guard<std::mutex> lock(ini_table_mutex);
          ini_table = ini_tmp;
        }
        cout << " done." << fg::reset << endl;
      } catch (const exception &e) {
        cerr << endl
             << fg::red << timestamp() << " INI file read error: " << e.what()
             << " - skipping changes" << fg::reset << endl;
      }
    });
  }).detach();

  if (daemon) {
    cout << fg::yellow
#ifndef _WIN32
         << "Running as daemon with PID " << getpid()
#else
         << "Running as daemon with PID " << getpid
#endif
         << ", will watch for changes to " << settings_path << endl
         << fg::reset << endl;
    zmqpp::proxy(frontend, backend);
    cerr << "Proxy exited" << endl;
    discovery_service.stop_advertising();
    frontend.close();
    backend.close();
    context.terminate();
    exit(EXIT_SUCCESS);
  }

  // Run interactively as a steerable proxy
  else {
    zmqpp::socket controlled(context, zmqpp::socket_type::rep);
    controlled.bind("inproc://broker-ctrl");
    zmqpp::socket controller(context, zmqpp::socket_type::req);
    controller.connect("inproc://broker-ctrl");

    auto settings_url_list = settings_urls(settings_address);
    auto current_url = settings_url_list.begin();

    thread(proxy, ref(frontend), ref(backend), ref(controlled)).detach();
    print_instructions();

    while (running) {
      zmqpp::message msg;
      char c = getch();
      if (c == '\0')
        continue;
      switch (c) {
#ifndef _WIN32
      // On Windows, the execv() function detaches from terminal and the
      // key_press() function does not work anymore
      case 'x':
      case 'X':
        reload = true;
#endif
      case 'q':
      case 'Q':
        controller.send("TERMINATE");
        controller.receive(msg);
        running = false;
        break;
      case 'r':
      case 'R':
        // NOTE: there is a bug in zmqpp lib and PAUSE and RESUME commands
        // have inverted meanings
        controller.send("RESUME");
        controller.receive(msg);
        cout << "Resuming operation" << endl;
        break;
      case 'p':
      case 'P':
        controller.send("PAUSE");
        controller.receive(msg);
        cout << "Pausing operation" << endl;
        break;
      case 'i':
      case 'I': {
        controller.send("STATISTICS");
        controller.receive(msg);
        vector<uint64_t> stats;
        for (size_t i = 0; i < msg.parts(); i++) {
          stats.push_back(msg.get<uint64_t>(i));
        }
        cout << setw(13) << " " << style::bold << setw(13) << "FRONTEND"
             << setw(13) << "BACKEND" << style::reset << endl;
        cout << fg::green << setw(13) << "Messages in:" << setw(13)
             << htonll(stats[0]) << setw(13) << htonll(stats[4]) << fg::reset
             << endl;
        cout << fg::green << setw(13) << "Bytes in:" << setw(13)
             << htonll(stats[1]) << setw(13) << htonll(stats[5]) << fg::reset
             << endl;
        cout << fg::yellow << setw(13) << "Messages out:" << setw(13)
             << htonll(stats[2]) << setw(13) << htonll(stats[6]) << fg::reset
             << endl;
        cout << fg::yellow << setw(13) << "Bytes out:" << setw(13)
             << htonll(stats[3]) << setw(13) << htonll(stats[7]) << fg::reset
             << endl;
        break;
      }
      case 'n':
      case 'N': {
        cout << goback(5) << "Settings are provided on " << style::bold
             << current_url->second << style::reset << style::italic << " ("
             << current_url->first << ")" << style::reset << endl;
        if (++current_url == settings_url_list.end()) {
          current_url = settings_url_list.begin();
        }
        print_instructions();
        break;
      }
      default:
        break;
      }
    }

    discovery_service.stop_advertising();
    cout << fg::green << "Closing sockets..." << fg::reset << endl;
    running = false;
    settings_thread.join();
    controller.close();
    controlled.close();
    if (crypto)
      curve_auth_ptr = nullptr;
    frontend.close();
    backend.close();
    context.terminate();
    if (reload) {
      cout << fg::yellow << "Restarting..." << fg::reset << endl;
#ifdef _WIN32
      relaunch();
#else
      execv(Mads::exec_path().string().c_str(), argv);
#endif
    }
  }
  return 0;
}
