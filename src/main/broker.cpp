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
#include "../detail/socket_options.hpp"
#include "../detail/wire_format.hpp"
#include "../exec_path.hpp"
#include "../mads.hpp"
#include "../watcher.hpp"
#include "../curve.hpp"
#include "../keypress.hpp"
#include "../goback.hpp"
#include "../service_discovery.hpp"
#include <atomic>
#include <csignal>
#include <cstring>
#include <cxxopts.hpp>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <vector>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <rang.hpp>
#include <regex>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <toml++/toml.hpp>
#include <zmq.hpp>
#include <zmq_addon.hpp>
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

// (A local htonll() used to live here, for Linux only, where the platform
// headers do not provide one. Nothing byte-swaps the proxy statistics any
// more -- see the STATISTICS handler below -- so it has no callers left.)

// INVARIANT: the broker is payload-opaque. It forwards frames verbatim between
// the XSUB frontend and XPUB backend and never inspects, parses, or rewrites
// message payloads. This is what lets the wire payload format evolve (snappy,
// MsgPack, new compression — see REFACTOR.md §4.1 / MSGPACK.md) with zero broker
// changes. Do NOT add payload parsing here.
//
// `capture`, when given, is ZMQ_DEVELOPMENT.md §2.2's opt-in tap: it must be
// a PUB socket, never PULL/PAIR -- a failed send on the proxy's capture
// socket aborts zmq_proxy_steerable() entirely (libzmq's forward() does
// `rc = capture(...); if (rc < 0) return -1`), and PUB is the one socket type
// that drops silently instead of blocking or erroring.
void proxy(zmq::socket_t &frontend, zmq::socket_t &backend,
           zmq::socket_t &ctrl, zmq::socket_ref capture = zmq::socket_ref()) {
  zmq::proxy_steerable(frontend, backend, capture, ctrl);
}

// Send one steering command (TERMINATE/PAUSE/RESUME/STATISTICS) to the proxy
// and wait for its acknowledgement, which the REQ/REP pair requires before the
// next command can be sent.
zmq::multipart_t steer(zmq::socket_t &controller, std::string_view command) {
  controller.send(zmq::buffer(command), zmq::send_flags::none);
  zmq::multipart_t reply;
  reply.recv(controller);
  return reply;
}

// ZMQ_DEVELOPMENT.md §2.2: an opt-in, live view of which topics currently
// have subscribers, derived from the XPUB backend's subscription
// notifications and published as an ordinary MADS message on a new topic
// ("subscriptions") -- ready for `mads-echo subscriptions` or a future `mads
// doctor --graph` cross-check. Off by default: proxy()'s capture argument
// receives a copy of every message the broker forwards, not just
// subscription frames, so this has a real cost while running.
//
// Instantiated only when [broker] subscription_table = true, so the
// zero-cost claim in ZMQ_DEVELOPMENT.md holds: with the setting off, this
// class -- and the capture socket it owns -- never exist.
class SubscriptionTable {
public:
  // Creates and binds the PUB capture socket. Must be called (and the
  // returned ref passed as proxy()'s `capture` argument) before the proxy
  // thread starts; start() may happen any time after.
  zmq::socket_ref bind(zmq::context_t &context,
                       std::string const &capture_endpoint) {
    _capture = zmq::socket_t(context, zmq::socket_type::pub);
    // Bounded on purpose (see proxy()'s comment above): this must never
    // block, so a slow/absent reader just loses the oldest notifications.
    _capture.set(zmq::sockopt::sndhwm, 1000);
    _capture.bind(capture_endpoint);
    return _capture;
  }

  // Starts the reader thread (decodes subscribe/unsubscribe notifications
  // off the capture endpoint into a topic -> subscriber-count table) and the
  // publisher thread (emits that table on the "subscriptions" topic every
  // `publish_period`, through an ordinary PUB connected to the broker's own
  // frontend -- the same path any agent publishes through).
  void start(zmq::context_t &context, std::string const &capture_endpoint,
            std::string const &frontend_address,
            std::chrono::milliseconds publish_period) {
    _running = true;
    _reader_thread = thread([this, &context, capture_endpoint]() {
      zmq::socket_t reader(context, zmq::socket_type::sub);
      reader.set(zmq::sockopt::subscribe, "");
      reader.set(zmq::sockopt::rcvtimeo, 300);
      reader.connect(capture_endpoint);
      while (_running) {
        zmq::multipart_t msg;
        if (!msg.recv(reader)) continue;
        // A subscribe/unsubscribe notification is always exactly one frame:
        // a 0x01/0x00 flag byte followed by the topic. Every ordinary MADS
        // message the proxy also mirrors here has at least two frames
        // (topic + payload), so frame count alone tells them apart without
        // looking at payload content (the broker stays payload-opaque).
        if (msg.size() != 1) continue;
        const string part = msg.at(0).to_string();
        if (part.empty()) continue;
        const uint8_t flag = static_cast<uint8_t>(part[0]);
        const string topic = part.substr(1);
        std::lock_guard<std::mutex> lock(_mutex);
        if (flag == 1) {
          _counts[topic]++;
        } else if (flag == 0) {
          auto it = _counts.find(topic);
          if (it != _counts.end() && --it->second <= 0)
            _counts.erase(it);
        }
      }
      reader.close();
    });

    _publish_thread = thread([this, &context, frontend_address, publish_period]() {
      zmq::socket_t pub(context, zmq::socket_type::pub);
      pub.connect(frontend_address);
      while (_running) {
        std::this_thread::sleep_for(publish_period);
        if (!_running) break;
        nlohmann::json j = nlohmann::json::object();
        {
          std::lock_guard<std::mutex> lock(_mutex);
          for (auto const &[topic, count] : _counts)
            j[topic] = count;
        }
        zmq::multipart_t out;
        out.addstr("subscriptions");
        out.addstr(Mads::detail::make_wire_header(
            WireFormat::Json, Mads::detail::Comp::None, false));
        out.addstr(Mads::detail::encode_payload(j, WireFormat::Json));
        out.send(pub);
      }
      pub.close();
    });
  }

  // Joins both threads. Must be called before the context is closed.
  void stop() {
    _running = false;
    if (_reader_thread.joinable()) _reader_thread.join();
    if (_publish_thread.joinable()) _publish_thread.join();
    try { _capture.close(); } catch (...) {}
  }

  ~SubscriptionTable() { stop(); }

private:
  std::atomic<bool> _running{false};
  std::thread _reader_thread, _publish_thread;
  std::mutex _mutex;
  std::map<std::string, int> _counts;
  zmq::socket_t _capture;
};

// Install SIGINT/SIGTERM handlers that request a clean shutdown by stopping
// the process-wide run flag. Used in daemon mode so a `kill`/`systemctl stop` (or CTRL-C)
// unwinds the proxy and stops advertising instead of killing the process
// abruptly. Only async-signal-safe work is done here (a store to an atomic).
void install_signal_handlers() {
  std::signal(SIGINT, [](int) { Mads::Runtime::stop_process(); });
  std::signal(SIGTERM, [](int) { Mads::Runtime::stop_process(); });
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
  ServiceDiscovery::ServiceInfo service_info;
  service_info.room = MADS_SERVICE_ROOM;
  service_info.encrypted = false;

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

  ParseResult options_parsed;
  try {
    options_parsed = options.parse(argc, argv);
  } catch (const cxxopts::exceptions::exception &e) {
    cerr << fg::red << "Error parsing command line: " << e.what() 
         << style::reset << endl;
    std::exit(EXIT_FAILURE);
  }

  if (options_parsed.unmatched().size() > 0) {
    cerr << fg::red << "Unrecognized command line options: ";
    for (const auto &opt : options_parsed.unmatched()) {
      cerr << opt << " ";
    }
    cerr << fg::reset << endl;
    std::exit(EXIT_FAILURE);
  }

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
    std::exit(EXIT_FAILURE);
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
    service_info.encrypted = true;
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
      config[name]["prefer_loopback_for_local_services"].value_or(true);

  // ZMQ_DEVELOPMENT.md §1.2: one I/O thread is the libzmq default, so an
  // unedited mads.ini creates the context exactly as before. A bad value in
  // the ini file should not stop the broker from starting, so clamp rather
  // than throw.
  int io_threads = config[name]["io_threads"].value_or(1);
  if (io_threads < 1) {
    cerr << fg::yellow << "Invalid [broker] io_threads = " << io_threads
         << ", clamping to 1" << fg::reset << endl;
    io_threads = 1;
  }
  auto socket_options =
      Mads::detail::SocketOptions::resolve(config["agents"], config[name]);

  // ZMQ_DEVELOPMENT.md §2.2: off by default. See SubscriptionTable's comment
  // above for the cost this opts into.
  const bool subscription_table_enabled =
      config[name]["subscription_table"].value_or(false);

  // Create broker sockets
  zmq::context_t context(io_threads);
  zmq::socket_t frontend(context, zmq::socket_type::xsub);
  zmq::socket_t backend(context, zmq::socket_type::xpub);
  socket_options.apply(frontend);
  socket_options.apply(backend);
  if (subscription_table_enabled) {
    // VERBOSER, not just VERBOSE: VERBOSE only forwards every individual
    // *subscribe*, still collapsing unsubscribes down to the topic's final
    // 1->0 transition. That would silently under-decrement the refcount
    // whenever one of several subscribers to the same topic leaves while
    // others remain. VERBOSER forwards every individual subscribe AND
    // unsubscribe, which is what per-subscriber accounting needs.
    backend.set(zmq::sockopt::xpub_verboser, true);
  }
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
      context.close();
      std::exit(EXIT_FAILURE);
    }
    try {
      curve_auth_ptr->setup_curve_server(frontend, key_name);
      curve_auth_ptr->setup_curve_server(backend, key_name);
    } catch (const runtime_error &e) {
      cerr << fg::red << e.what() << fg::reset << endl;
      curve_auth_ptr = nullptr;
      frontend.close();
      backend.close();
      context.close();
      std::exit(EXIT_FAILURE);
    }
  }

  try {
    std::cout << "Binding broker frontend (XSUB) at " << style::bold
              << frontend_address << style::reset << endl;
    frontend.bind(frontend_address);
    std::cout << "Binding broker backend (XPUB) at " << style::bold
              << backend_address << style::reset << endl;
    backend.bind(backend_address);
  } catch (const zmq::error_t &e) {
    cerr << fg::red << "ZMQ error, could not connect: " << e.what() << fg::reset
         << endl;
    std::exit(EXIT_FAILURE);
  }

  // ZMQ_DEVELOPMENT.md §2.2: bound (not just constructed) before the proxy
  // thread starts below, so its socket_ref is ready wherever `proxy()` is
  // invoked. subscription_table itself stays unconstructed -- no capture
  // socket exists at all -- when the setting is off.
  unique_ptr<SubscriptionTable> subscription_table;
  zmq::socket_ref capture_ref;
  if (subscription_table_enabled) {
    // frontend_address is a bind address (e.g. "tcp://*:9090"), not
    // connectable, so the table's publisher needs an endpoint of its own to
    // join the frontend on. That used to be loopback TCP on the same port,
    // which silently published nothing whenever --crypto was on: with CURVE
    // the frontend is a CURVE *server*, and this publisher carries no client
    // keys, so libzmq dropped it during the ZMTP handshake -- before a
    // single frame was exchanged and without any error the broker could
    // report. A second, inproc bind fixes that for good: libzmq wires inproc
    // peers together as a direct pipe pair (socket_base_t::connect() creates
    // them with no engine and therefore no security mechanism), so this path
    // behaves identically with and without encryption, and skips a pointless
    // encrypt/decrypt round through the loopback interface either way.
    const string frontend_inproc = "inproc://mads-broker-frontend";
    frontend.bind(frontend_inproc);
    subscription_table = make_unique<SubscriptionTable>();
    capture_ref = subscription_table->bind(context, "inproc://mads-broker-capture");
    subscription_table->start(context, "inproc://mads-broker-capture",
                              frontend_inproc, 1000ms);
  }

  // ZMQ_DEVELOPMENT.md §3.6: ROUTER (external, CURVE-secured) <-proxy-> DEALER
  // (inproc) <- N worker REP sockets, replacing the single lockstep REP. The
  // handler reads plugin attachments off disk inside the loop, so one agent
  // fetching a large attachment used to block every other agent's settings
  // request behind it on the single REP; a worker pool removes that
  // head-of-line blocking. REQ<->ROUTER is a standard pairing and DEALER<->REP
  // preserves the envelope transparently, so unmigrated REQ agents are
  // unaffected -- the wire is identical to before.
  //
  // CURVE goes on the ROUTER only: the DEALER/REP hop is inproc, with no
  // handshake to secure.
  int settings_workers = config[name]["settings_workers"].value_or(2);
  if (settings_workers < 1) {
    cerr << fg::yellow << "Invalid [broker] settings_workers = "
         << settings_workers << ", clamping to 1" << fg::reset << endl;
    settings_workers = 1;
  }
  const string settings_workers_endpoint = "inproc://mads-broker-settings-workers";

  zmq::socket_t settings_router(context, zmq::socket_type::router);
  if (crypto)
    curve_auth_ptr->setup_curve_server(settings_router, key_name);
  socket_options.apply(settings_router);
  settings_router.bind(settings_address);
  cout << "Binding broker shared settings (ROUTER, " << settings_workers
       << " worker" << (settings_workers == 1 ? "" : "s") << ") at "
       << style::bold << settings_address << style::reset << endl;

  zmq::socket_t settings_dealer(context, zmq::socket_type::dealer);
  settings_dealer.bind(settings_workers_endpoint);

  // Steerable so shutdown can TERMINATE it deterministically, exactly like
  // the frontend/backend proxy() above.
  zmq::socket_t settings_proxy_controlled(context, zmq::socket_type::rep);
  settings_proxy_controlled.bind("inproc://mads-broker-settings-proxy-ctrl");
  zmq::socket_t settings_proxy_controller(context, zmq::socket_type::req);
  settings_proxy_controller.connect("inproc://mads-broker-settings-proxy-ctrl");
  thread settings_proxy_thread(proxy, ref(settings_router), ref(settings_dealer),
                               ref(settings_proxy_controlled), zmq::socket_ref());

  string ini_table = read_settings_file(settings_path);
  std::mutex ini_table_mutex;

  // Each worker's REP loop body is the pre-ROUTER handler verbatim: same
  // commands, same frame layout, same check_version() handling.
  auto settings_worker_body = [&]() {
    zmq::socket_t settings(context, zmq::socket_type::rep);
    settings.set(zmq::sockopt::rcvtimeo, 1000);
    settings.connect(settings_workers_endpoint);
    while (running) {
      zmq::multipart_t msg;
      zmq::multipart_t content;
      content.addstr(LIB_VERSION);
      if (msg.recv(settings)) {
        if (msg.size() < 2) {
          cerr << goback(1, !daemon) << fg::red << timestamp()
               << "Received malformed message from agent, "
               << "expected at least 2 parts" << fg::reset << endl;
          continue;
        }
        string agent_version = msg.at(0).to_string();
        string cmd = msg.at(1).to_string();
        string agent_name = "unknown";
        if (msg.size() == 3) {
          agent_name = msg.at(2).to_string();
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
              content.addstr(ini_table);
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
                     << fg::reset << style::reset << endl;
                attachment_content << attachment_file.rdbuf();
                content.addstr(attachment_content.str());
              }
            }
          }
          content.send(settings);
        } else if (cmd == "timecode") {
          chrono::system_clock::time_point now = chrono::system_clock::now();
          const string tc = to_string(Mads::timecode(now, timecode_fps));
          settings.send(zmq::buffer(tc), zmq::send_flags::none);
        } else {
          cerr << goback(1, !daemon) << fg::yellow << timestamp()
               << "Got unexpected command " << cmd << fg::reset << endl;
          content.send(settings);
        }
      }
    }
    settings.close();
  };

  vector<thread> settings_worker_threads;
  settings_worker_threads.reserve(settings_workers);
  for (int i = 0; i < settings_workers; ++i) {
    settings_worker_threads.emplace_back(settings_worker_body);
  }

  cout << "Timecode FPS: " << style::bold << timecode_fps << style::reset
       << endl;

  // In daemon mode, install the shutdown signal handlers before the (possibly
  // slow) discovery startup so that a SIGINT/SIGTERM arriving during early
  // startup is handled gracefully rather than terminating the process.
  if (daemon) {
    install_signal_handlers();
  }

  auto discovery_interval = std::chrono::milliseconds(
      config[name]["discovery_interval_ms"].value_or(1000));

  // Attempt to start advertising once. Returns true on success. On failure
  // (e.g. the network is not up yet) it returns false and, unless quiet, prints
  // a warning. start_advertising() throws when there are no broadcast-capable
  // interfaces, which is exactly the situation we want to keep retrying from.
  auto try_start_discovery = [&](bool quiet) -> bool {
    try {
      discovery_service.start_advertising(service_info, discovery_interval);
      cout << "Advertising service on UDP discovery port " << MADS_SERVICE_PORT
           << " with room name '" << style::bold << service_info.room
           << style::reset << "'" << endl;
      return true;
    } catch (const runtime_error &e) {
      if (!quiet) {
        cerr << fg::red << "Error starting service discovery: " << e.what()
             << endl;
      }
      return false;
    }
  };

  // Background retry: when started as a daemon (e.g. as a system service during
  // early boot) the network may not be up yet. Keep trying to open the
  // advertising sockets every 5 seconds until it succeeds, so that advertising
  // eventually starts on its own. Interactively we just warn once.
  thread discovery_retry_thread;
  constexpr auto discovery_retry_interval = std::chrono::seconds(5);
  if (!try_start_discovery(false)) {
    if (daemon) {
      cerr << style::bold << "Will keep retrying every "
           << discovery_retry_interval.count() << "s in the background"
           << style::reset << fg::reset << endl;
      discovery_retry_thread = thread([&, discovery_retry_interval]() {
        while (Mads::Runtime::process_running() && !discovery_service.is_advertising()) {
          // Interruptible sleep so shutdown is not delayed by up to 5s.
          for (auto waited = std::chrono::milliseconds::zero();
               waited < discovery_retry_interval && Mads::Runtime::process_running();
               waited += std::chrono::milliseconds(100)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
          }
          if (!Mads::Runtime::process_running()) {
            break;
          }
          if (try_start_discovery(true)) {
            break;
          }
        }
      });
    } else {
      cerr << style::bold << "Service discovery will be disabled" << style::reset
           << fg::reset << endl;
    }
  }

  // print settings URI for clients
  string port = settings_address.substr(settings_address.find_last_of(":") + 1);
  cout << "Settings are provided on " << style::bold
       << "tcp://127.0.0.1:" << port << style::reset << style::italic
       << " (loopback)" << style::reset << endl;

  Mads::Watcher settings_watcher(settings_path, 1s);
  thread settings_watcher_thread([&]() {
    string ini_tmp = "";
    settings_watcher.watch([&](const std::string &file_name) {
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
  });

  if (daemon) {
    cout << fg::yellow
#ifndef _WIN32
         << "Running as daemon with PID " << getpid()
#else
         << "Running as daemon with PID " << getpid
#endif
         << ", will watch for changes to " << settings_path << endl
         << fg::reset << endl;

    // Graceful shutdown: run the proxy in steerable mode so that SIGINT/SIGTERM
    // (stopping the process-wide run flag, see install_signal_handlers above) can unwind it
    // cleanly. The blocking zmq::proxy() cannot be interrupted by a signal, so
    // we drive a steerable proxy from a control socket and send TERMINATE once a
    // shutdown is requested.
    zmq::socket_t controlled(context, zmq::socket_type::rep);
    controlled.bind("inproc://broker-ctrl");
    zmq::socket_t controller(context, zmq::socket_type::req);
    controller.connect("inproc://broker-ctrl");

    thread proxy_thread(proxy, ref(frontend), ref(backend), ref(controlled),
                        capture_ref);

    while (Mads::Runtime::process_running()) {
      this_thread::sleep_for(200ms);
    }

    cout << fg::green << "Shutdown requested, stopping proxy..." << fg::reset
         << endl;
    steer(controller, "TERMINATE");
    proxy_thread.join();

    if (discovery_retry_thread.joinable()) {
      discovery_retry_thread.join();
    }
    discovery_service.stop_advertising();
    // Stop the settings workers before terminating the context, otherwise
    // their blocking receive() would throw "Context was terminated".
    running = false;
    for (auto &t : settings_worker_threads) t.join();
    steer(settings_proxy_controller, "TERMINATE");
    settings_proxy_thread.join();
    settings_watcher.stop();
    settings_watcher_thread.join();
    controller.close();
    controlled.close();
    settings_proxy_controller.close();
    settings_proxy_controlled.close();
    if (subscription_table) subscription_table->stop();
    frontend.close();
    backend.close();
    settings_router.close();
    settings_dealer.close();
    context.close();
    exit(EXIT_SUCCESS);
  }

  // Run interactively as a steerable proxy
  else {
    zmq::socket_t controlled(context, zmq::socket_type::rep);
    controlled.bind("inproc://broker-ctrl");
    zmq::socket_t controller(context, zmq::socket_type::req);
    controller.connect("inproc://broker-ctrl");

    auto settings_url_list = settings_urls(settings_address);
    auto current_url = settings_url_list.begin();

    thread(proxy, ref(frontend), ref(backend), ref(controlled), capture_ref)
        .detach();
    print_instructions();

    while (running) {
      zmq::multipart_t msg;
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
        msg = steer(controller, "TERMINATE");
        running = false;
        break;
      // NOTE: libzmq's steering commands are inverted, so the wire command
      // sent below is the opposite of the effect it produces. In
      // libzmq 4.3.5's src/proxy.cpp:
      //
      //   if (msiz == 5 && memcmp (command, "\x05PAUSE", 6))   state = active;
      //   else if (msiz == 6 && 0 == memcmp (command, "RESUME", 6))
      //                                                        state = paused;
      //
      // The PAUSE arm is missing its `0 ==` and compares six bytes (with a
      // stray \x05) against a five-byte command, so it is always true and sets
      // `active`; the RESUME arm matches correctly but sets `paused`. Sending
      // the opposite command is what makes these keys do what they say.
      // Verified by tests/test_broker_steering.cpp, which fails if a libzmq
      // upgrade ever fixes this upstream. (The old comment here blamed zmqpp;
      // it was passing the bug through, not causing it.)
      case 'r':
      case 'R':
        msg = steer(controller, "PAUSE"); // -> state = active
        cout << "Resuming operation" << endl;
        break;
      case 'p':
      case 'P':
        msg = steer(controller, "RESUME"); // -> state = paused
        cout << "Pausing operation" << endl;
        break;
      case 'i':
      case 'I': {
        msg = steer(controller, "STATISTICS");
        // zmq_proxy_steerable() writes each counter as a NATIVE-endian
        // uint64_t, so these are read straight out with no byte-order
        // conversion. (Until the cppzmq migration this went through zmqpp's
        // message::get<uint64_t>(), which applied ntohll() to every integer
        // part it read; the htonll() that used to wrap each value here was
        // undoing that, not converting anything. Both are gone -- applying
        // only one of the two swaps prints 19-digit garbage.)
        vector<uint64_t> stats;
        for (size_t i = 0; i < msg.size(); i++) {
          uint64_t v = 0;
          const size_t n =
              msg.at(i).size() < sizeof(v) ? msg.at(i).size() : sizeof(v);
          std::memcpy(&v, msg.at(i).data(), n);
          stats.push_back(v);
        }
        if (stats.size() < 8) {
          cerr << fg::red << "Broker returned " << stats.size()
               << " statistics counters, expected 8" << fg::reset << endl;
          break;
        }
        // Wide enough for a 20-digit uint64_t, so two large counters can
        // never run together into one unreadable number.
        constexpr int STAT_W = 22;
        cout << setw(13) << " " << style::bold << setw(STAT_W) << "FRONTEND"
             << setw(STAT_W) << "BACKEND" << style::reset << endl;
        cout << fg::green << setw(13) << "Messages in:" << setw(STAT_W)
             << stats[0] << setw(STAT_W) << stats[4] << fg::reset << endl;
        cout << fg::green << setw(13) << "Bytes in:" << setw(STAT_W)
             << stats[1] << setw(STAT_W) << stats[5] << fg::reset << endl;
        cout << fg::yellow << setw(13) << "Messages out:" << setw(STAT_W)
             << stats[2] << setw(STAT_W) << stats[6] << fg::reset << endl;
        cout << fg::yellow << setw(13) << "Bytes out:" << setw(STAT_W)
             << stats[3] << setw(STAT_W) << stats[7] << fg::reset << endl;
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
    for (auto &t : settings_worker_threads) t.join();
    steer(settings_proxy_controller, "TERMINATE");
    settings_proxy_thread.join();
    settings_watcher.stop();
    settings_watcher_thread.join();
    controller.close();
    controlled.close();
    settings_proxy_controller.close();
    settings_proxy_controlled.close();
    if (crypto)
      curve_auth_ptr = nullptr;
    if (subscription_table) subscription_table->stop();
    frontend.close();
    backend.close();
    settings_router.close();
    settings_dealer.close();
    context.close();
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
