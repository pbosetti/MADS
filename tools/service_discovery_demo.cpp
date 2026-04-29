#include "service_discovery.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cxxopts.hpp>
#include <iostream>
#include <limits>
#include <thread>

using namespace std;
using namespace std::chrono_literals;
using namespace cxxopts;
using namespace Mads;

namespace {

atomic<bool> keep_running{true};

void stop_demo(int) {
  keep_running = false;
  Mads::running = false;
}

pair<string, uint16_t> parse_service_argument(const string &value) {
  const auto separator = value.find(':');
  if (separator == string::npos || separator == 0 || separator == value.size() - 1) {
    throw runtime_error("Invalid service specification `" + value + "`, expected name:port");
  }

  const auto name = value.substr(0, separator);
  const auto port_string = value.substr(separator + 1);

  try {
    size_t parsed_characters = 0;
    const auto port_value = stoul(port_string, &parsed_characters);
    if (parsed_characters != port_string.size()) {
      throw runtime_error("Invalid service port in `" + value + "`");
    }
    if (port_value == 0 || port_value > numeric_limits<uint16_t>::max()) {
      throw runtime_error("Service port out of range in `" + value + "`");
    }
    return {name, static_cast<uint16_t>(port_value)};
  } catch (const invalid_argument &) {
    throw runtime_error("Invalid service port in `" + value + "`");
  } catch (const out_of_range &) {
    throw runtime_error("Service port out of range in `" + value + "`");
  }
}

} // namespace

int main(int argc, char **argv) {
  Options options(argv[0], "UDP service discovery demo");
  // clang-format off
  options.add_options()
    ("provider", "Run as service provider")
    ("consumer", "Run as service consumer")
    ("r,room", "Room name", value<string>()->default_value("default"))
    ("p,discovery-port", "UDP discovery port", value<uint16_t>()->default_value(to_string(ServiceDiscovery::DEFAULT_DISCOVERY_PORT)))
    ("i,interval-ms", "Advertising interval in milliseconds", value<uint32_t>()->default_value("1000"))
    ("t,timeout-ms", "Discovery timeout in milliseconds (0 = wait indefinitely)", value<uint32_t>()->default_value("0"))
    ("s,service", "Service mapping as name:port", value<vector<string>>())
    ("h,help", "Show help");
  // clang-format on

  const auto parsed = options.parse(argc, argv);
  if (parsed.count("help") != 0) {
    cout << options.help() << endl;
    return 0;
  }

  const bool provider = parsed.count("provider") != 0;
  const bool consumer = parsed.count("consumer") != 0;
  if (provider == consumer) {
    cerr << "Choose exactly one mode: --provider or --consumer" << endl;
    return EXIT_FAILURE;
  }

  ServiceDiscovery discovery(parsed["discovery-port"].as<uint16_t>());
  const auto room = parsed["room"].as<string>();

  try {
    if (provider) {
      ServiceDiscovery::ServiceInfo service;
      service.room = room;

      if (parsed.count("service") != 0) {
        for (const auto &argument : parsed["service"].as<vector<string>>()) {
          pair<string, uint16_t> parsed_service;
          try {
            parsed_service = parse_service_argument(argument);
          } catch (const exception &e) {
            throw runtime_error("Invalid --service `" + argument + "`: " + e.what());
          }
          const auto &[name, port] = parsed_service;
          service.ports[name] = port;
        }
      } else {
        service.ports = {
          {"frontend", 9090},
          {"backend", 9091},
          {"settings", 9092}
        };
      }

      signal(SIGINT, stop_demo);
      signal(SIGTERM, stop_demo);

      discovery.start_advertising(
        service,
        chrono::milliseconds(parsed["interval-ms"].as<uint32_t>())
      );

      cout << "Advertising room `" << room << "` on UDP port "
           << discovery.discovery_port() << endl;
      for (const auto &[name, port] : service.ports) {
        cout << "  " << name << ": " << port << endl;
      }
      cout << "Press Ctrl-C to stop." << endl;

      while (keep_running) {
        this_thread::sleep_for(200ms);
      }

      discovery.stop_advertising();
      return EXIT_SUCCESS;
    }

    const auto timeout = chrono::milliseconds(parsed["timeout-ms"].as<uint32_t>());
    const auto service = discovery.discover(room, timeout);
    cout << service.to_json().dump(2) << endl;
    return EXIT_SUCCESS;
  } catch (const exception &e) {
    cerr << e.what() << endl;
    discovery.stop_advertising();
    return EXIT_FAILURE;
  }
}
