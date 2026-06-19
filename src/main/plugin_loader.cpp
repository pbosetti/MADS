/*
  ____  _             _         _                 _
 |  _ \| |_   _  __ _(_)_ __   | | ___   __ _  __| | ___ _ __
 | |_) | | | | |/ _` | | '_ \  | |/ _ \ / _` |/ _` |/ _ \ '__|
 |  __/| | |_| | (_| | | | | | | | (_) | (_| | (_| |  __/ |
 |_|   |_|\__,_|\__, |_|_| |_| |_|\___/ \__,_|\__,_|\___|_|
                |___/

This is a plugin-based general purpose filter for the Mads framework.
Actual work is done by the plugins, this executable is just a wrapper.
Author(s): Paolo Bosetti
*/
#include "../agent_app.hpp"
#include "../exec_path.hpp"
#include "../mads.hpp"
#include <cxxopts.hpp>
#include <filesystem>
#include <memory>
#include <pugg/Kernel.h>
#include <regex>

#if defined(PLUGIN_LOADER_SOURCE)
#include <chrono>
#include <source.hpp>
#define PLUGIN_CLASS Source
#define PLUGIN_NAME "Source"
#define PLUGIN_DEFAULT "publish.plugin"
#define AGENT_NAME_DEFAULT "publish"
#elif defined(PLUGIN_LOADER_FILTER)
#include <filter.hpp>
#define PLUGIN_CLASS Filter
#define PLUGIN_NAME "Filter"
#define PLUGIN_DEFAULT "bridge.plugin"
#define AGENT_NAME_DEFAULT "bridge"
#elif defined(PLUGIN_LOADER_SINK)
#include <sink.hpp>
#define PLUGIN_CLASS Sink
#define PLUGIN_NAME "Sink"
#define PLUGIN_DEFAULT "feedback.plugin"
#define AGENT_NAME_DEFAULT "feedback"
#else
#error "No plugin type defined"
#endif

#define MADS_PLUGIN_MIN_PROTOCOL 7

using namespace std;
using namespace cxxopts;
using namespace Mads;
using json = nlohmann::json;
namespace fs = std::filesystem;

#if defined(PLUGIN_LOADER_SOURCE)
using Plugin = Source<json>;
using PluginDriver = SourceDriver<json>;
#elif defined(PLUGIN_LOADER_FILTER)
using Plugin = Filter<json, json>;
using PluginDriver = FilterDriver<json, json>;
#elif defined(PLUGIN_LOADER_SINK)
using Plugin = Sink<json>;
using PluginDriver = SinkDriver<json>;
#endif

#include <cctype>
#include <locale>
#include <nlohmann/json.hpp>
#include <string>

using namespace std;
using json = nlohmann::json;

json str_to_num(const string &s) {
  string str = s;

  // Trim leading/trailing whitespace
  auto ltrim = [](string &s) {
    s.erase(s.begin(), find_if(s.begin(), s.end(),
                               [](unsigned char ch) { return !isspace(ch); }));
  };
  auto rtrim = [](string &s) {
    s.erase(find_if(s.rbegin(), s.rend(),
                    [](unsigned char ch) { return !isspace(ch); })
                .base(),
            s.end());
  };
  ltrim(str);
  rtrim(str);

  // Try bool
  if (str == "true" || str == "TRUE")
    return true;
  else if (str == "false" || str == "FALSE")
    return false;

  // Try integer
  try {
    size_t pos;
    long long val = std::stoll(str, &pos);
    if (pos == str.size()) {
      return val; // fully parsed as int
    }
  } catch (...) {
    // ignore
  }

  // Try float/double
  try {
    size_t pos;
    double val = std::stod(str, &pos);
    if (pos == str.size()) {
      return val; // fully parsed as float
    }
  } catch (...) {
    // ignore
  }

  // Fallback: return string
  return str;
}

int main(int argc, char *argv[]) {
  string plugin_name, plugin_file = PLUGIN_DEFAULT,
                      agent_name = AGENT_NAME_DEFAULT;
  size_t count = 0, count_err = 0;
  size_t delay = 0;
  bool silent = false;

  // CLI options
  AgentApp agent(argv[0], SETTINGS_URI);
  // clang-format off
  agent.options()
    ("plugin", "Plugin to load", value<string>())
    ("n,name", "Agent name (default to plugin name)", value<string>())
    ("i,agent-id", "Agent ID to be added to JSON frames", value<string>())
    ("d,delay", "Initial delay before forst message in ms (default 0)", value<size_t>())
    ("o,option", "Additional plugin options (may be repeated)", value<vector<string>>())
    ("silent", "Silent mode (don't print status line)");
  #if defined(PLUGIN_LOADER_SOURCE) or defined(PLUGIN_LOADER_FILTER)
  agent.options()
    ("p,period", "Sampling period (default 100 ms)", value<size_t>());
  #endif
  #if defined(PLUGIN_LOADER_FILTER) || defined(PLUGIN_LOADER_SINK)
  agent.options()
    ("b,dont-block", "don't block on read");
  #endif
  // clang-format on
  agent.add_common_options();

  agent.raw_options().parse_positional({"plugin"});
  agent.raw_options().positional_help("<name.plugin>");
  auto options_parsed = agent.parse_options(argc, argv);
  if (int rc = AgentApp::handle_standard_exit_options<AgentApp>(
          options_parsed, agent.raw_options(), argv);
      rc >= 0) {
    return rc;
  }

  if (options_parsed.count("plugin") != 0) {
    plugin_file = options_parsed["plugin"].as<string>();
    if (!plugin_file.ends_with(".plugin")) {
      plugin_file += ".plugin";
    }
    agent_name = fs::path(plugin_file).stem().string();
  }
  if (options_parsed.count("name") != 0) {
    agent_name = options_parsed["name"].as<string>();
  }
  if (options_parsed.count("delay") != 0) {
    delay = options_parsed["delay"].as<size_t>();
  }

  if (options_parsed.count("silent") != 0) {
    silent = true;
  }
  // Core stuff
  agent.set_agent_name(agent_name);
  try {
    agent.init(options_parsed);
  } catch (const AgentError &e) {
    cerr << fg::red << "Error initializing agent: " << e.what() << fg::reset
         << endl;
    exit(EXIT_FAILURE);
  } catch (const std::exception &e) {
    cerr << fg::red << "Runtime error initializing agent: " << e.what()
         << fg::reset << endl;
    exit(EXIT_FAILURE);
  }
  #if defined(PLUGIN_LOADER_SINK) or defined(PLUGIN_LOADER_FILTER)
  agent.enable_remote_control();
  #else
  agent.enable_threaded_remote_control();
  #endif

  // Copy agent settings as plugin parameters
  json settings = agent.settings_json();
  settings["agent_name"] = agent_name;
  if (options_parsed.count("agent-id")) {
    settings["agent_id"] = options_parsed["agent-id"].as<string>();
  }
  settings["prefix"] = Mads::prefix();
  // settings override: -o <key>=<value> patches the plugin-facing settings.
  if (options_parsed.count("option")) {
    auto re = regex(R"((.+?)=(.*))");
    smatch match;
    for (auto &v : options_parsed["option"].as<vector<string>>()) {
      if (regex_match(v, match, re)) {
        settings[match[1].str()] = str_to_num(match[2].str());
      }
    }
  }
  // Re-apply core agent settings that init() already consumed into member
  // variables, so that -o <key>=<value> overrides them too (not just plugin
  // params). Mirrors the string->enum mapping used in Agent::init().
  if (settings["receive_timeout"].is_number()) {
    agent.set_receive_timeout(settings["receive_timeout"].get<int>());
  }
  if (settings["wire_format"].is_string()) {
    string wf = settings["wire_format"].get<string>();
    agent.set_wire_format((wf == "msgpack" || wf == "MsgPack")
                              ? WireFormat::MsgPack
                              : WireFormat::Json);
  }
  if (settings["compression"].is_string()) {
    string comp = settings["compression"].get<string>();
    if (comp == "none")
      agent.set_compression(Compression::None);
    else if (comp == "snappy")
      agent.set_compression(Compression::Snappy);
    else
      agent.set_compression(Compression::Auto);
  }
  // deprecated queue size option:
  if (!settings["high_watermark"].is_null()) {
    agent.set_high_watermark(settings.value("high_watermark", 1000));
  }

#if defined(PLUGIN_LOADER_SOURCE) or defined(PLUGIN_LOADER_FILTER)
  chrono::milliseconds time{0};
  cerr << "  Sampling period:  " << style::bold;
  if (options_parsed.count("p") != 0) {
    time = chrono::milliseconds(options_parsed["p"].as<size_t>());
    cerr << fg::yellow << time.count() << " ms"
         << " (from -p option)" << fg::reset << style::reset << endl;
  } else if (!settings["period"].is_null()) {
    time = chrono::milliseconds(settings["period"].get<size_t>());
    cerr << time.count() << " ms"
         << " (from settings)" << style::reset << endl;
  } else {
    cerr << fg::red << "free run" << fg::reset << style::reset << " (default)"
         << endl;
  }
#endif

  agent.info(cerr);
  if (!settings["high_watermark"].is_null()) {
    cerr << fg::yellow 
         << "Warning: high_watermark setting is deprecated, use queue_size" 
         << fg::reset << endl;
  }
  agent.connect();

#if defined(PLUGIN_LOADER_FILTER) || defined(PLUGIN_LOADER_SINK)
  bool dont_block = settings.value("dont-block", false);
  dont_block = settings.value("dont_block", dont_block);
  if (!settings["dont-block"].is_null()) {
    cerr << fg::yellow
        << "Warning: dont-block setting is deprecated, use dont_block"
        << fg::reset << endl;
  }
  if (options_parsed.count("dont-block") != 0) {
    dont_block = true;
  }
  if (dont_block) {
    cerr << fg::yellow << "Running in non-blocking mode" << fg::reset << endl;
  }

#endif

  if (options_parsed.count("plugin") != 0) {
    if (!fs::exists(plugin_file)) {
      cerr << style::italic 
           << "  Searching for installed plugin in the default location "
           << style::reset;
#ifdef _WIN32
      cerr << Mads::exec_dir("../bin/") << endl;
      plugin_file = Mads::exec_dir("../bin/" + plugin_file);
#else
      cerr << Mads::exec_dir("../lib/") << endl;
      plugin_file = Mads::exec_dir("../lib/" + plugin_file);
#endif
    }
    if (!fs::exists(plugin_file)) {
      cerr << fg::red << "Error: cannot find plugin file " << plugin_file
           << " (extension .plugin is required!)" << fg::reset << endl;
      exit(1);
    }
  } else if (!agent.attachment_path().empty()) {
    plugin_file = agent.attachment_path().string();
  }
  plugin_name = fs::path(plugin_file).stem().string();

  // Loading plugin
  pugg::Kernel kernel;
  kernel.add_server<PLUGIN_CLASS<>>();
  kernel.load_plugin(plugin_file);
  PluginDriver *plugin_driver =
      kernel.get_driver<PluginDriver>(Plugin::server_name(), plugin_name);
  if (plugin_driver == nullptr) {
    cerr << fg::red << "Error: cannot find plugin driver " << plugin_name
         << " in plugin at " << plugin_file << fg::reset << endl;
    auto drivers = kernel.get_all_drivers<PluginDriver>(Plugin::server_name());
    cerr << "Available drivers:" << endl;
    for (auto &d : drivers) {
      cerr << "- " << d->name() << endl;
    }
    exit(1);
  }
  // Create the class from the plugin:
  auto plugin = std::unique_ptr<Plugin>(plugin_driver->create());

  cerr << style::bold << "Plugin settings:" << style::reset << endl
       << "  Plugin:           " << style::bold << plugin_file 
       << " (loaded as " << agent_name  
       << " prot. v" << plugin->version << ")" << style::reset << endl;
  
  if (plugin->version < MADS_PLUGIN_MIN_PROTOCOL) {
    cerr << style::bold << fg::red 
         << "Fatal error: unsupported plugin protocol version. Minimum is "
         << plugin->version << ", loaded plugin protocol is version "
         << MADS_PLUGIN_MIN_PROTOCOL << ".\n"
         << "Recompile the plugin (see https://github.com/pbosetti/mads_plugin)"
         << fg::reset << style::reset << endl;
    return EXIT_FAILURE;
  }

  plugin->set_params(settings);
  for (auto &[k, v] : plugin->info()) {
    cerr << "  " << left << setw(18) << k << style::bold << v << style::reset
         << endl;
  }
#if defined(PLUGIN_LOADER_SOURCE)
// TODO: in a forthcoming plugin protocol version, add blob_format to 
// FILTERS too, and adapt the agent_loop for filters accordingly
  string out_format = plugin->blob_format();
  cerr << "  Blob format:      " << style::bold << out_format << style::reset
       << endl;
#endif

  // Initial delay
  if (delay > 0) {
    this_thread::sleep_for(chrono::milliseconds(delay));
  }

  // Main loop
  agent.register_event(event_type::startup, settings, "modified_settings");
  cerr << fg::green << PLUGIN_NAME " plugin started" << fg::reset << endl;

#if defined(PLUGIN_LOADER_SOURCE)
  json out, err;
  return_type rt;
  vector<unsigned char> blob;
  agent.loop([&]() -> chrono::milliseconds {
    out.clear();
    blob.clear();
    rt = plugin->get_output(out, &blob);
    switch (rt) {
    case return_type::warning:
      try {
        out["warning"] = {{"get_output", plugin->error()}};
      } catch (...) {
        cerr << fg::yellow << "Warning getting data: " << plugin->error()
              << " (could not add to output JSON)" << fg::reset << endl;
      }
      [[fallthrough]];
    case return_type::success:
      if (out.empty()) {
        out["warning"]["get_output"] = "Plugin did not return any output";
      }
      if (blob.size() > 0) {
        if (!out.contains("format"))
          out["format"] = out_format;
        auto topic = out.value("topic", "");
        agent.publish(blob, std::move(out), topic);
      } else if (!out.empty()) {
        auto topic = out.value("topic", "");
        agent.publish(std::move(out), topic);
      }
      break;
    case return_type::retry:
      return 0ms;
    case return_type::error:
      err = {{"error", {"get_output", plugin->error()}}};
      agent.register_event(event_type::message, err);
      count_err++;
      break;
    case return_type::critical:
      // cerr << fg::red << "Critical error getting data: " << plugin->error()
      //      << fg::reset << endl;
      count_err++;
      Mads::running = false;
      throw std::runtime_error(string("Critical error in getting data: ") + plugin->error());
      return 0ms;
    }
    if (!silent) {
      cerr << "\r\x1b[0KMessages processed: " << fg::green << ++count
            << fg::reset << " total, " << fg::red << count_err << fg::reset
            << " with errors ";
      cerr.flush();
    }
    return plugin->next_loop_duration;
  }, time);

#elif defined(PLUGIN_LOADER_FILTER)
  json in, out = {}, err;
  return_type rt;
  message_type type;
  tuple<string, string, vector<unsigned char>> msg_blob;
  vector<unsigned char> blob{};
  agent.loop([&]() -> chrono::milliseconds {
    type = message_type::none;
    in.clear();
    out.clear();
    err.clear();
    blob.clear();
    try {
      type = agent.receive(dont_block);
    } catch (const AgentError &e) {
      cerr << fg::red << "Error receiving message: " << e.what()
            << fg::reset << endl;
    }
    if (agent.last_topic() == agent.pub_topic()) {
      cerr << fg::yellow << "Warning: received message on the same topic (even partial) as the plugin published to, skipping to avoid loops" << fg::reset << endl;
      return 0ms; // dont use my own messages
    }
    // loading data into plugin
    if (type == message_type::json) {
      // agent.remote_control();
      if (agent.last_topic() == "control") {
        return 0ms; // Control message, already handled
      }
      // Object fast path: avoids the agent re-dumping (MsgPack) and a second
      // parse here. Parsing happens at most once, inside last_json().
      try {
        in = get<1>(agent.last_json());
      } catch (json::parse_error &e) {
        cerr << fg::red << e.what() << endl
             <<"Error parsing message content:" << fg::reset
             << endl << get<1>(agent.last_message()) << endl;
        return 0ms;
      }
      rt = plugin->load_data(in, agent.last_topic());
    } else if (type == message_type::blob) {
      msg_blob = agent.last_blob();
      in = json::parse(get<1>(msg_blob));
      rt = plugin->load_data(in, agent.last_topic(), &get<2>(msg_blob));
    } else {
      if (dont_block) {
        goto process_output;
      } else {
        return plugin->next_loop_duration; // No message received, wait for next iteration
      }
    }

    switch (rt) {
    case return_type::warning:
      err["warning"]["load_data"] = plugin->error();
      agent.register_event(event_type::message, err);
      [[fallthrough]];
    case return_type::success:
      break; // next step
    case return_type::retry:
      return 0ms; // next iteration
    case return_type::error:
      err["error"]["load_data"] = plugin->error();
      agent.register_event(event_type::message, err);
      count_err++;
      goto status_line;
    case return_type::critical:
      cerr << fg::red << "Critical error loading data: " << plugin->error()
            << fg::reset << endl;
      err["error"]["load_data"] = plugin->error();
      agent.register_event(event_type::message, err);
      Mads::running = false;
      return 0ms;
    }
    // processing data in the plugin
  process_output:
    rt = plugin->process(out, &blob);
    if (!err.empty()) out.merge_patch(err);
    switch (rt) {
    case return_type::warning:
      out["warning"]["process"] = plugin->error();
      [[fallthrough]];
    case return_type::success:
      if (out.empty()) {
        out["warning"]["process"] = "Plugin did not return any output";
      }
      break; // next step
    case return_type::retry:
      return 0ms; // next iteration
    case return_type::error:
      err["error"]["process"] = plugin->error();
      agent.register_event(event_type::message, err);
      count_err++;
      goto status_line;
    case return_type::critical:
      cerr << fg::red
            << "Critical error processing data: " << plugin->error()
            << fg::reset << endl;
      err["error"]["process"] = plugin->error();
      agent.register_event(event_type::message, err);
      Mads::running = false;
      return 0ms;
    }
    // publishing data
    if (!blob.empty()) {
      if (!out.contains("format"))
        out["format"] = "raw";
      auto topic = out.value("topic", "");
      agent.publish(blob, std::move(out), topic);
    } else {
      auto topic = out.value("topic", "");
      agent.publish(std::move(out), topic);
    }
  status_line:
    if (!silent) {
      cerr << "\r\x1b[0KMessages processed: " << fg::green << ++count
            << fg::reset << " total, " << fg::red << count_err << fg::reset
            << " with errors ";
      cerr.flush();
    }
    return plugin->next_loop_duration;
  }, time);
#elif defined(PLUGIN_LOADER_SINK)
  message_type type;
  json in, err;
  return_type rt;
  tuple<string, string, vector<unsigned char>> msg_blob;
  agent.loop([&]() -> chrono::milliseconds {
    type = message_type::none;
    in.clear();
    err.clear();
    try {
      type = agent.receive();
    } catch (const AgentError &e) {
      std::cerr << fg::red << "Error receiving message: " << e.what() 
                << fg::reset << endl;
    }
    if (type == message_type::none) {
      return 0ms; // No message received
    } else if (type == message_type::blob) {
      msg_blob = agent.last_blob();
      in = json::parse(get<1>(msg_blob));
      rt = plugin->load_data(in, agent.last_topic(), &get<2>(msg_blob));
    } else {
      // agent.remote_control();
      if (agent.last_topic() == "control") {
        return 0ms; // Control message, already handled
      }
      // Object fast path (see filter loop): no re-dump / no double parse.
      try {
        in = get<1>(agent.last_json());
      } catch (json::parse_error &e) {
        cerr << fg::red << e.what() << endl
             <<"Error parsing message content:" << fg::reset
             << endl << get<1>(agent.last_message()) << endl;
        return 0ms;
      }
      rt = plugin->load_data(in, agent.last_topic());
    }
    switch (rt) {
    case return_type::warning:
      cerr << fg::yellow << "Warning loading data: " << plugin->error()
           << fg::reset << endl;
      err = {{"warning", {"load_data", plugin->error()}}};
      agent.register_event(event_type::message, err);
      [[fallthrough]];
    case return_type::success:
    case return_type::retry:
      break;
    case return_type::error:
      cerr << fg::red << "Error loading data: " << plugin->error() << fg::reset
           << endl;
      err = {{"error", {"load_data", plugin->error()}}};
      agent.register_event(event_type::message, err);
      count_err++;
      return 0ms;
    case return_type::critical:
      cerr << fg::red << "Critical error loading data: " << plugin->error()
           << fg::reset << endl;
      json e_msg = {{"error", {"load_data", plugin->error()}}};
      agent.register_event(event_type::message, e_msg);
      count_err++;
      Mads::running = false;
      return 0ms;
    }

    if (!silent) {
      cerr << "\r\x1b[0KMessages processed: " << fg::green << ++count
           << fg::reset << " total, " << fg::red << count_err << fg::reset
           << " with errors ";
      cerr.flush();
    }
    return 0ms;
  });
#endif
  cerr << fg::green << "\n" PLUGIN_NAME " plugin stopped" << fg::reset << endl;

  // Cleanup
  agent.register_event(event_type::shutdown);
  agent.disconnect();
  plugin.reset();
  kernel.clear_drivers();

  agent.restart_if_requested(argv);
  return 0;
}
