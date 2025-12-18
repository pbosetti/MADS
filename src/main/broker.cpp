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
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "../exec_path.hpp"
#include "../keypress.hpp"
#include "../mads.hpp"
#include "../watcher.hpp"
#include "../curve.hpp"
#include <cstring>
#include <cxxopts.hpp>
#include <filesystem>
#include <iomanip>
#include <iostream>
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
#include <WinSock2.h>
#include <iphlpapi.h>
#include <signal.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <sys/ioctl.h>
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

/*
  _   _ _   _ _ _ _
 | | | | |_(_) (_) |_ _   _
 | | | | __| | | | __| | | |
 | |_| | |_| | | | |_| |_| |
  \___/ \__|_|_|_|\__|\__, |
                      |___/
*/

bool get_nic_ip(string &ip, const string nic) {
#ifdef _WIN32
  PIP_ADAPTER_INFO pAdapterInfo;
  PIP_ADAPTER_INFO pAdapter = NULL;
  DWORD dwRetVal = 0;
  ULONG ulOutBufLen = sizeof(IP_ADAPTER_INFO);

  pAdapterInfo = (IP_ADAPTER_INFO *)malloc(sizeof(IP_ADAPTER_INFO));
  if (pAdapterInfo == NULL) {
    cerr << "Error allocating memory needed to call GetAdaptersinfo" << endl;
    return false;
  }

  if (GetAdaptersInfo(pAdapterInfo, &ulOutBufLen) == ERROR_BUFFER_OVERFLOW) {
    free(pAdapterInfo);
    pAdapterInfo = (IP_ADAPTER_INFO *)malloc(ulOutBufLen);
    if (pAdapterInfo == NULL) {
      cerr << "Error allocating memory needed to call GetAdaptersinfo" << endl;
      return false;
    }
  }

  if ((dwRetVal = GetAdaptersInfo(pAdapterInfo, &ulOutBufLen)) == NO_ERROR) {
    pAdapter = pAdapterInfo;
    if (nic == "list") {
      cout << fg::yellow << "Available network adapters:" << fg::reset << endl;
      while (pAdapter) {
        cout << "[" << fg::yellow << setw(2) << pAdapter->Index << fg::reset
             << "] - " << pAdapter->Description << " -> " << fg::yellow
             << pAdapter->IpAddressList.IpAddress.String << fg::reset << endl;
        pAdapter = pAdapter->Next;
      }
    } else {
      while (pAdapter) {
        if (nic == std::to_string(pAdapter->Index)) {
          ip = pAdapter->IpAddressList.IpAddress.String;
          free(pAdapterInfo);
          return true;
        }
        pAdapter = pAdapter->Next;
      }
    }
  } else {
    cerr << "GetAdaptersInfo failed with error: " << dwRetVal << endl;
  }

  if (pAdapterInfo)
    free(pAdapterInfo);
  return false;
#else
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);

  if (nic == "list") {
    struct ifaddrs *ptr_ifaddrs = nullptr;

    auto result = getifaddrs(&ptr_ifaddrs);
    if (result != 0) {
      cout << fg::red << "`getifaddrs()` failed: " << strerror(errno)
           << fg::reset << endl;
      return false;
    }
    cout << fg::yellow << "Available network adapters:" << fg::reset << endl;
    for (struct ifaddrs *ptr_entry = ptr_ifaddrs; ptr_entry != nullptr;
         ptr_entry = ptr_entry->ifa_next) {
      string ipaddress_human_readable_form;
      string interface_name = string(ptr_entry->ifa_name);
      if (!ptr_entry->ifa_addr) {
        continue;
      }
      sa_family_t address_family = ptr_entry->ifa_addr->sa_family;
      if (address_family == AF_INET) {
        if (ptr_entry->ifa_addr != nullptr) {
          char buffer[INET_ADDRSTRLEN] = {0};
          inet_ntop(address_family,
                    &((struct sockaddr_in *)(ptr_entry->ifa_addr))->sin_addr,
                    buffer, INET_ADDRSTRLEN);
          ipaddress_human_readable_form = string(buffer);
        }

        cout << "[" << fg::yellow << setw(10) << interface_name << fg::reset
             << "] - " << ipaddress_human_readable_form << endl;
      }
    }
    freeifaddrs(ptr_ifaddrs);
    return true;
  }

  struct ifreq ifr {};
  strcpy(ifr.ifr_name, nic.c_str());
  if (ioctl(fd, SIOCGIFADDR, &ifr) == -1) {
    cerr << "ioctl error: " << string(strerror(errno)) << endl;
    return false;
  }
  close(fd);
  ip = inet_ntoa(((sockaddr_in *)&ifr.ifr_addr)->sin_addr);
  return true;
#endif
}

#ifdef __linux__
#include <algorithm> // std::reverse()
#include <endian.h> // __BYTE_ORDER __LITTLE_ENDIAN

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

void send_char(char c) {
#ifdef _WIN32
  INPUT ip;
  ip.type = INPUT_KEYBOARD;
  ip.ki.wScan = 0;
  ip.ki.time = 0;
  ip.ki.dwExtraInfo = 0;
  ip.ki.wVk = c;
  ip.ki.dwFlags = 0;
  SendInput(1, &ip, sizeof(INPUT));
  ip.ki.dwFlags = KEYEVENTF_KEYUP;
  SendInput(1, &ip, sizeof(INPUT));
#else
  ioctl(0, TIOCSTI, &c);
#endif
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
  string ip = "127.0.0.1";
  bool crypto = false;
  vector<string> desc{"FRONTEND msg in   ", "FRONTEND bytes in ",
                      "FRONTEND msg out  ", "FRONTEND bytes out",
                      "BACKEND msg in    ", "BACKEND bytes in  ",
                      "BACKEND msg out   ", "BACKEND bytes out "};
  // clang-format off
  options.add_options()
    ("n,nic", "Network interface name (-n list to list'em all)",
      value<string>())
    ("s,settings", "Settings file path", value<string>())
    ("d,daemon", "Run as daemon")
    ("docker", "Run as in container (don't check for file changes)")
    ("crypto", "Enable CURVE encryption (requires proper setup)", value<string>()->implicit_value("broker"))
    ("keys_dir", "Directory containing CURVE keypairs",
      value<string>()->implicit_value(keys_dir.string()))
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
  if (options_parsed.count("nic") && options_parsed["nic"].as<string>() == "list") {
    get_nic_ip(ip, "list");
    return 0;
  }

  if (options_parsed.count("settings") != 0) {
    settings_path = options_parsed["settings"].as<string>();
  } else {
    struct stat buf;
    if (stat(settings_path.c_str(), &buf) != 0)
      settings_path = Mads::exec_dir("../etc/" SETTINGS_PATH);
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
    if (options_parsed.count("keys_dir") != 0) {
      keys_dir = filesystem::path(options_parsed["keys_dir"].as<string>());
    }
    cout << fg::cyan << "Enabling CURVE encryption for broker sockets" 
         << endl;
    cout << "Searching for keys in " << style::bold
         << keys_dir.string() << style::reset << fg::reset << endl;
    crypto = true;
  }

  double timecode_fps = config["agents"]["timecode_fps"].value_or(MADS_FPS);

  string frontend_address, backend_address, settings_address;
  frontend_address = config[name]["frontend_address"].value_or(BROKER_FRONTEND);
  backend_address = config[name]["backend_address"].value_or(BROKER_BACKEND);
  settings_address = config[name]["settings_address"].value_or(BROKER_SETTINGS);
  nic = config[name]["nic"].value_or(nic);
  if (options_parsed.count("nic") != 0) {
    nic = options_parsed["nic"].as<string>();
    cout << "Using network interface " << style::bold << nic << style::reset
         << endl;
  }
  if (!get_nic_ip(ip, nic)) {
    cerr << fg::red << "Cannot get IP address for NIC " << nic << fg::reset
         << endl;
  }

  // Create broker sockets
  zmqpp::context context;
  zmqpp::socket frontend(context, zmqpp::socket_type::xsub);
  zmqpp::socket backend(context, zmqpp::socket_type::xpub);
  if (crypto) {
    auto whitelist = config[name]["ip_whitelist"].as_array();
    bool verbose = config[name]["auth_verbose"].value_or(false);
    string name = options_parsed["crypto"].as<string>();
    curve_auth_ptr = make_unique<Mads::CurveAuth>(context);
    if (whitelist) {
      whitelist->for_each([&](const toml::node& n) {
        if (toml::is_string<decltype(n)>) {
          curve_auth_ptr->allowed_ips.push_back(n.as_string()->get());
        }
      });
    }
    curve_auth_ptr->setup_auth(verbose ? Mads::auth_verbose::on : Mads::auth_verbose::off);
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
      curve_auth_ptr->setup_curve_server(frontend, name);
      curve_auth_ptr->setup_curve_server(backend, name);
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
    curve_auth_ptr->setup_curve_server(settings, "broker");
  settings.bind(settings_address);
  settings.set(zmqpp::socket_option::receive_timeout, 1000);
  cout << "Binding broker shared settings (REP) at " << style::bold
       << settings_address << style::reset << endl;
  thread settings_thread([&]() {
    zmqpp::message msg;
    std::ifstream t(settings_path);
    std::stringstream buffer;
    buffer << t.rdbuf();
    string ini_table = buffer.str();
    while (running) {
      zmqpp::message content;
      content << LIB_VERSION;
      if (settings.receive(msg)) {
        if (msg.parts() < 2) {
          cerr << fg::red << "Received malformed message from agent, "
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
            cerr << fg::red
                 << "Received settings request from agent with wrong version: "
                 << agent_version << " (vs. " << LIB_VERSION << ")" << fg::reset
                 << endl;
            continue;
          }
          cout << "Sending settings to agent " << agent_name << " ("
               << agent_version << ")" << endl;
          content << ini_table;
          string attachment_path = config[agent_name]["attachment"].value_or("");
          if (!attachment_path.empty()) {
            if (filesystem::path(attachment_path).is_relative()) {
              attachment_path = Mads::exec_dir(attachment_path);
            }
            if (!filesystem::exists(attachment_path)) {
              cerr << fg::red << "attachment path does not exist: "
                   << attachment_path << fg::reset << endl;
            } else {
              ifstream attachment_file(attachment_path, ios::in | ios::binary);
              stringstream attachment_content;
              cout << fg::yellow << "  Attaching binary object: "
                   << style::bold << attachment_path
                   << " (" << filesystem::file_size(attachment_path) << " bytes)"
                   << fg::reset << endl;
              attachment_content << attachment_file.rdbuf();
              content << attachment_content.str();
            }
          }
          settings.send(content);
        } else if (cmd == "timecode") {
          chrono::system_clock::time_point now = chrono::system_clock::now();
          settings.send(to_string(Mads::timecode(now, timecode_fps)));
        } else {
          cerr << fg::yellow << "Got unexpected command " << cmd << fg::reset
               << endl;
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
  cout << "Settings are provided via " << style::bold << "tcp://" << ip << ":"
       << port << style::reset << endl;

#ifndef _WIN32
  if (options_parsed.count("docker") != 0) {
    cout << fg::yellow
         << "Running in container mode, remember to restart if mads.ini changes"
         << fg::reset << endl;
    zmqpp::proxy(frontend, backend);
    exit(EXIT_SUCCESS);
  }
  thread watcher_thread;
  // Run as a daemon
  if (options_parsed.count("daemon") != 0) {
    thread watcher_thread([&]() {
      Mads::Watcher watcher(settings_path);
      watcher.watch(&running, [&](const std::string &file_name) {
        cout << fg::yellow << "Settings file " << file_name
             << " has been modified, exiting..." << fg::reset << endl;
        raise(SIGUSR1);
      });
    });
    cout << "Running as daemon with PID " << getpid()
         << ", will exit upon changes to " << settings_path << endl;
    zmqpp::proxy(frontend, backend);
    cerr << "Proxy exited" << endl;
  }
#else
  if (options_parsed.count("daemon") != 0) {
    cout << "Running as daemon with PID " << getpid << endl;
    zmqpp::proxy(frontend, backend);
    cerr << "Proxy exited" << endl;
  }
#endif

  // Run interactively as a steerable proxy
  else {
    zmqpp::socket controlled(context, zmqpp::socket_type::rep);
    controlled.bind("inproc://broker-ctrl");
    zmqpp::socket controller(context, zmqpp::socket_type::req);
    controller.connect("inproc://broker-ctrl");

#ifndef _WIN32
    // to stop this thread on Q, need implementing
    // https://stackoverflow.com/questions/60718561/clean-way-to-stop-terminating-a-thread-waiting-on-stdin-in-c
    watcher_thread = thread([&]() {
      Mads::Watcher watcher(settings_path, 1s);
      watcher.watch(&running, [&](const std::string &file_name) {
        cout << fg::yellow << "Settings file " << file_name
             << " has been modified, reloading..." << fg::reset << endl;
        send_char('x');
      });
    });
#endif

    thread proxy_thread(proxy, ref(frontend), ref(backend), ref(controlled));
    cout << style::italic << "CTRL-C to immediate exit" << style::reset << endl;
#ifdef _WIN32
    cout << fg::green
         << "Type P to pause, R to resume, I for information, Q to clean quit"
         << fg::reset << endl;
#else
    cout << fg::green
         << "Type P to pause, R to resume, I for information, Q to clean "
            "quit, X to restart and reload settings"
         << fg::reset << endl;
#endif

    while (running) {
      zmqpp::message msg;
      char c = key_press();
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
      default:
#ifdef _WIN32
        cout << fg::green
             << "Type P to pause, R to resume, I for information, Q to clean "
                "quit"
             << fg::reset << endl;
#else
        cout << fg::green
             << "Type P to pause, R to resume, I for information, Q to clean "
                "quit, X to restart and reload settings"
             << fg::reset << endl;
#endif
        break;
      }
    }

    cout << fg::green << "Closing sockets..." << fg::reset << endl;
    running = false;
    proxy_thread.join();
    settings_thread.join();
#ifndef _WIN32
    watcher_thread.join();
#endif
    controller.close();
    controlled.close();
    if (crypto) curve_auth_ptr = nullptr;
    frontend.close();
    backend.close();
    context.terminate();
    if (reload) {
      cout << fg::yellow << "Restarting..." << fg::reset << endl;
      execv(Mads::exec_path().string().c_str(), argv);
    }
  }
  return 0;
}