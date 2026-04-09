#include <common.hpp>

#include <iostream>
#include <limits>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <cxxopts.hpp>
#include "../mads.hpp"
#include <rang.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <pugg/Driver.h>
#include <pugg/Kernel.h>


using json = nlohmann::json;
using namespace cxxopts;
using namespace rang;

namespace {

enum class PluginType {
  unknown = 0,
  source,
  filter,
  sink
};

struct DriverInfo {
  PluginType type = PluginType::unknown;
  std::string name;
  int version = 0;
};

struct ProbeResult {
  bool library_loaded = false;
  std::string load_error;
  std::vector<DriverInfo> drivers;
};

constexpr const char *kSourceServerName = "SourceServer";
constexpr const char *kFilterServerName = "FilterServer";
constexpr const char *kSinkServerName = "SinkServer";

const char *type_to_string(PluginType type) {
  switch (type) {
  case PluginType::source:
    return "source";
  case PluginType::filter:
    return "filter";
  case PluginType::sink:
    return "sink";
  case PluginType::unknown:
    break;
  }
  return "unknown";
}

std::string join_types(const std::vector<DriverInfo> &drivers) {
  std::string joined;
  bool has_source = false;
  bool has_filter = false;
  bool has_sink = false;

  for (const auto &driver : drivers) {
    if (driver.type == PluginType::source) {
      has_source = true;
    } else if (driver.type == PluginType::filter) {
      has_filter = true;
    } else if (driver.type == PluginType::sink) {
      has_sink = true;
    }
  }

  if (has_source) {
    joined = "source";
  }
  if (has_filter) {
    joined += joined.empty() ? "filter" : ", filter";
  }
  if (has_sink) {
    joined += joined.empty() ? "sink" : ", sink";
  }
  if (joined.empty()) {
    joined = "unknown";
  }
  return joined;
}

void print_drivers(const std::vector<DriverInfo> &drivers) {
  if (drivers.empty()) {
    std::cout << "drivers: none" << std::endl;
    return;
  }

  std::cout << "drivers:" << std::endl;
  for (const auto &driver : drivers) {
    std::cout << "  - name: " << style::bold << driver.name << style::reset
              << ", type: " << style::bold << type_to_string(driver.type) 
              << style::reset
              << ", protocol_version: " << style::bold 
              << driver.version << style::reset << std::endl;
  }
}

std::string native_load_error(const std::string &plugin_path) {
#ifdef _WIN32
  HMODULE handle = LoadLibraryA(plugin_path.c_str());
  if (handle) {
    FreeLibrary(handle);
    return "";
  }

  const DWORD error_code = GetLastError();
  LPSTR message_buffer = nullptr;
  const DWORD size = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error_code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPSTR>(&message_buffer), 0, nullptr);
  std::string message;
  if (size > 0 && message_buffer) {
    message.assign(message_buffer, size);
    LocalFree(message_buffer);
  }
  return message;
#else
  dlerror();
  void *handle = dlopen(plugin_path.c_str(), RTLD_NOW);
  if (handle) {
    dlclose(handle);
    return "";
  }

  const char *error = dlerror();
  return error ? std::string(error) : std::string("unknown loader error");
#endif
}

template <typename TDriver>
void collect_drivers(pugg::Kernel &kernel, const char *server_name,
                     PluginType type, std::vector<DriverInfo> &out) {
  auto drivers = kernel.get_all_drivers<TDriver>(server_name);
  for (auto *driver : drivers) {
    out.push_back({type, driver->name(), driver->version()});
  }
}

ProbeResult inspect_plugin(const std::string &plugin_path) {
  ProbeResult result;
  pugg::Kernel kernel;
  kernel.add_server(kSourceServerName, std::numeric_limits<int>::min());
  kernel.add_server(kFilterServerName, std::numeric_limits<int>::min());
  kernel.add_server(kSinkServerName, std::numeric_limits<int>::min());

  result.load_error = native_load_error(plugin_path);
  result.library_loaded = kernel.load_plugin(plugin_path);
  collect_drivers<pugg::Driver>(kernel, kSourceServerName, PluginType::source,
                                result.drivers);
  collect_drivers<pugg::Driver>(kernel, kFilterServerName, PluginType::filter,
                                result.drivers);
  collect_drivers<pugg::Driver>(kernel, kSinkServerName, PluginType::sink,
                                result.drivers);
  kernel.clear_drivers();
  return result;
}

} // namespace


int main(int argc, char *argv[]) {
  bool json_output = false;

  Options options(argv[0]);
  // clang-format off
  options.add_options()
    ("plugin", "Plugin to load", value<std::string>())
    ("j,json", "Output results in JSON format")
    ("h,help", "Print usage");
  // clang-format on
  options.parse_positional({"plugin"});
  options.positional_help("<path/to/name.plugin>");
  auto options_parsed = options.parse(argc, argv);

  if (options_parsed.count("help")) {
    std::cout << argv[0] << " ver. " << LIB_VERSION << std::endl;
    std::cout << options.help() << std::endl;
    return 0;
  }
  if (options_parsed.count("json")) {
    json_output = true;
  }
  if (options_parsed.count("plugin") == 0) {
    std::cerr << fg::red << "Error: plugin path is required\n" << fg::reset;
    std::cerr << options.help() << std::endl;
    return 1;
  }

  const std::string plugin_path = options_parsed["plugin"].as<std::string>();
  const auto probe = inspect_plugin(plugin_path);
  const auto &all_drivers = probe.drivers;
  const bool library_loaded = probe.library_loaded;
  const auto &load_error = probe.load_error;

  const bool loadable = library_loaded && !all_drivers.empty();
  bool protocol_current = loadable;
  for (const auto &driver : all_drivers) {
    if (driver.version < PLUGIN_PROTOCOL_VERSION) {
      protocol_current = false;
      break;
    }
  }

  if (json_output) {
    json output;
    output["plugin"] = plugin_path;
    output["library_loaded"] = library_loaded;
    if (!load_error.empty()) {
      output["load_error"] = load_error;
    }
    output["drivers"] = json::array();
    for (const auto &driver : all_drivers) {
      json driver_json;
      driver_json["name"] = driver.name;
      driver_json["type"] = type_to_string(driver.type);
      driver_json["protocol_version"] = driver.version;
      output["drivers"].push_back(driver_json);
    }
    output["protocol_current"] = protocol_current;
    std::cout << output.dump(2) << std::endl;
    return protocol_current ? 0 : 2;
  } else {

    std::cout << "plugin: " << style::bold << plugin_path 
              << style::reset << std::endl;
    std::cout << "loadable: " << style::bold << (loadable ? "yes" : "no") 
              << style::reset << std::endl;

    if (!library_loaded) {
      std::cout << fg::yellow;
      std::cout << "protocol_current: no" << std::endl;
      std::cout << "type: unknown" << std::endl;
      if (!load_error.empty()) {
        std::cout << "reason: " << load_error << std::endl;
      } else {
        std::cout << "reason: failed to load shared library or missing "
                    "register_pugg_plugin symbol"
                  << std::endl;
      }
      std::cout << fg::reset;
      return 1;
    }

    if (all_drivers.empty()) {
      std::cout << fg::yellow;
      std::cout << "protocol_current: no" << std::endl;
      std::cout << "type: unknown" << std::endl;
      std::cout << "reason: plugin loaded but did not register any recognized "
                  "source/filter/sink drivers"
                << std::endl;
      std::cout << fg::reset;
      return 1;
    }

    std::cout << "protocol_current: " << style::bold 
              << (protocol_current ? "yes" : "no") 
              << style::reset << std::endl;
    std::cout << "type: " << style::bold << join_types(all_drivers) 
              << style::reset << std::endl;
    std::cout << "expected_protocol_version: " << style::bold 
              << PLUGIN_PROTOCOL_VERSION
              << style::reset << std::endl;

    if (!protocol_current) {
      std::cout << fg::yellow;
      std::cout << "reason: plugin driver version is older than the current "
                  "protocol"
                << std::endl;
      std::cout << fg::reset;
    }

    print_drivers(all_drivers);
  }
  return protocol_current ? 0 : 2;
}
