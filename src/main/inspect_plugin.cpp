#include <common.hpp>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
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
  std::string json_version; ///< nlohmann/json version the plugin was built with
};

constexpr const char *kSourceServerName = "SourceServer";
constexpr const char *kFilterServerName = "FilterServer";
constexpr const char *kSinkServerName = "SinkServer";

// The nlohmann/json version this inspector (and therefore the MADS loaders) was
// compiled against — the version a plugin must match for its driver RTTI to be
// recognized by the loaders. Two-level stringify expands the integer macros.
#define MADS_STRINGIFY_HELPER(x) #x
#define MADS_STRINGIFY(x) MADS_STRINGIFY_HELPER(x)
const std::string kJsonExpected = MADS_STRINGIFY(NLOHMANN_JSON_VERSION_MAJOR) "." \
    MADS_STRINGIFY(NLOHMANN_JSON_VERSION_MINOR) "." \
    MADS_STRINGIFY(NLOHMANN_JSON_VERSION_PATCH);
#undef MADS_STRINGIFY
#undef MADS_STRINGIFY_HELPER

// Parse the "MAJOR_MINOR_PATCH" of an nlohmann inline-namespace tag that starts at
// `pos` (the 'j' of "json_abi_v") inside a mangled type name. The patch number
// cannot be read greedily: in Itanium mangling the tag is immediately followed by
// the next component's own length prefix (e.g. `...json_abi_v3_11_3` + `10basic_json`),
// so "3_11_3" would be misread as "3_11_310". We therefore use the source-name
// length prefix (the decimal digits right before the tag) to find the exact end of
// the component; for manglings without such a prefix (e.g. MSVC decorated names,
// where the tag is followed by a separator) we read the trailing [0-9_] run.
std::string parse_json_abi_at(const std::string &data, std::size_t pos) {
  constexpr std::size_t kKeyLen = 10; // strlen("json_abi_v")
  const std::size_t vstart = pos + kKeyLen;
  std::size_t vend;

  std::size_t prefix_begin = pos;
  while (prefix_begin > 0 &&
         std::isdigit(static_cast<unsigned char>(data[prefix_begin - 1])))
    --prefix_begin;
  if (prefix_begin < pos) { // Itanium length prefix present
    const long len =
        std::strtol(data.substr(prefix_begin, pos - prefix_begin).c_str(),
                    nullptr, 10);
    if (len <= static_cast<long>(kKeyLen) ||
        pos + static_cast<std::size_t>(len) > data.size())
      return "";
    vend = pos + static_cast<std::size_t>(len);
  } else { // no length prefix: read until a non [0-9_] separator
    vend = vstart;
    while (vend < data.size() &&
           (std::isdigit(static_cast<unsigned char>(data[vend])) ||
            data[vend] == '_'))
      ++vend;
  }

  std::vector<std::string> parts;
  std::string cur;
  for (std::size_t i = vstart; i <= vend; ++i) {
    const char c = (i < vend) ? data[i] : '_';
    if (c == '_') {
      if (cur.empty())
        return "";
      parts.push_back(cur);
      cur.clear();
    } else if (std::isdigit(static_cast<unsigned char>(c))) {
      cur += c;
    } else {
      return "";
    }
  }
  if (parts.size() != 3)
    return "";
  return parts[0] + "." + parts[1] + "." + parts[2];
}

// Discover which nlohmann/json version a plugin was compiled against by scanning
// its binary for the inline-namespace tag `json_abi_vMAJOR_MINOR_PATCH`, embedded
// in the mangled names of the Source/Filter/SinkDriver<json> types every MADS
// plugin exports. Prefer the tag bound to a driver type (the exact type the
// loaders cast to), falling back to any occurrence. Independent of whether the
// plugin actually loads; returns "" if no tag is found.
std::string detect_plugin_json_version(const std::string &plugin_path) {
  std::ifstream file(plugin_path, std::ios::binary);
  if (!file)
    return "";
  const std::string data((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());

  for (const char *kind : {"SourceDriver", "FilterDriver", "SinkDriver"}) {
    for (std::size_t dp = data.find(kind); dp != std::string::npos;
         dp = data.find(kind, dp + 1)) {
      const std::size_t jp = data.find("json_abi_v", dp);
      if (jp != std::string::npos && jp - dp < 48) {
        const std::string v = parse_json_abi_at(data, jp);
        if (!v.empty())
          return v;
      }
    }
  }
  for (std::size_t jp = data.find("json_abi_v"); jp != std::string::npos;
       jp = data.find("json_abi_v", jp + 10)) {
    const std::string v = parse_json_abi_at(data, jp);
    if (!v.empty())
      return v;
  }
  return "";
}

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
  result.json_version = detect_plugin_json_version(plugin_path);
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
    output["json_expected"] = kJsonExpected;
    output["json_found"] =
        probe.json_version.empty() ? "unknown" : probe.json_version;
    output["json_match"] =
        !probe.json_version.empty() && probe.json_version == kJsonExpected;
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

    std::cout << "json_expected: " << style::bold << kJsonExpected
              << style::reset << std::endl;
    std::cout << "json_found: " << style::bold
              << (probe.json_version.empty() ? "unknown" : probe.json_version)
              << style::reset << std::endl;
    if (!probe.json_version.empty() && probe.json_version != kJsonExpected) {
      std::cout << fg::yellow
                << "warning: plugin built against nlohmann/json "
                << probe.json_version << " but MADS uses " << kJsonExpected
                << "; ABI mismatch can prevent loading with pugg >= 1.1.0."
                << fg::reset << std::endl;
    }

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
