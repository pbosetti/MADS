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
#include "../agent.hpp"
#include "../exec_path.hpp"
#include "../mads.hpp"
#include <cxxopts.hpp>
#include <filesystem>
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
  string settings_uri = SETTINGS_URI;
  string plugin_name, plugin_file = PLUGIN_DEFAULT,
                      agent_name = AGENT_NAME_DEFAULT;
  size_t count = 0, count_err = 0;
  size_t delay = 0;
  chrono::milliseconds time{0};

  // CLI options
  Options options(argv[0]);
  // clang-format off
  options.add_options()
    ("plugin", "Plugin to load", value<string>())
    ("n,name", "Agent name (default to plugin name)", value<string>())
    ("i,agent-id", "Agent ID to be added to JSON frames", value<string>())
    ("d,delay", "Initial delay before forst message in ms (default 0)", value<size_t>())
    ("o,option", "Additional plugin options (may be repeated)", value<vector<string>>());
  #if defined(PLUGIN_LOADER_SOURCE) or defined(PLUGIN_LOADER_FILTER)
  options.add_options()
    ("p,period", "Sampling period (default 100 ms)", value<size_t>());
  #endif
  #if defined(PLUGIN_LOADER_FILTER)
  options.add_options()
    ("b,dont-block", "don't block on read");
  #endif
  options.parse_positional({"plugin"});
  options.positional_help("<name.plugin>");
  // clang-format on
  SETUP_OPTIONS(options, Agent);

  if (options_parsed.count("plugin") != 0) {
    plugin_file = options_parsed["plugin"].as<string>();
    agent_name = fs::path(plugin_file).stem().string();
  }
  if (options_parsed.count("name") != 0) {
    agent_name = options_parsed["name"].as<string>();
  }
  if (options_parsed.count("delay") != 0) {
    delay = options_parsed["delay"].as<size_t>();
  }

  // Core stuff
  Agent agent(agent_name, settings_uri);
  try {
    agent.init();
  } catch (const AgentError &e) {
    cerr << fg::red << "Error initializing agent: " << e.what() << fg::reset
         << endl;
    exit(EXIT_FAILURE);
  } catch (const std::exception &e) {
    cerr << fg::red << "Runtime error initializing agent: " << e.what()
         << fg::reset << endl;
    exit(EXIT_FAILURE);
  }
  agent.enable_remote_control();
  agent.connect();

  // Copy agent settings as plugin parameters
  json settings = agent.get_settings();
  if (options_parsed.count("agent-id")) {
    settings["agent_id"] = options_parsed["agent-id"].as<string>();
    agent.set_agent_id(options_parsed["agent-id"].as<string>());
  }
  settings["prefix"] = Mads::prefix();
  bool silent = settings.value("silent", false);
  if (settings["receive_timeout"].is_number()) {
    agent.set_receive_timeout(settings["receive_timeout"].get<int>());
  }
  // settings override
  if (options_parsed.count("options")) {
    auto re = regex(R"((.+?)=(.*))");
    smatch match;
    for (auto &v : options_parsed["options"].as<vector<string>>()) {
      if (regex_match(v, match, re)) {
        settings[match[1].str()] = str_to_num(match[2].str());
      }
    }
  }
  agent.info(cerr);
#if defined(PLUGIN_LOADER_SOURCE) or defined(PLUGIN_LOADER_FILTER)
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

#if defined(PLUGIN_LOADER_FILTER)
  bool dont_block = false;
  if (options_parsed.count("dont-block") != 0 ||
      settings.value("dont_block", false)) {
    cerr << fg::yellow << "  Running in non-blocking mode" << fg::reset << endl;
    dont_block = true;
  }
#endif

  if (options_parsed.count("plugin") != 0) {
    plugin_file = options_parsed["plugin"].as<string>();
    if (!fs::exists(plugin_file)) {
      cerr << "Searching for installed plugin in the default location ";
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
  Plugin *plugin = plugin_driver->create();

  cerr << "  Plugin:           " << style::bold << plugin_file << " (loaded as "
       << agent_name << ")" << style::reset << endl;

  plugin->set_params((void *)&settings);
  for (auto &[k, v] : plugin->info()) {
    cerr << "  " << left << setw(18) << k << style::bold << v << style::reset
         << endl;
  }
#if defined(PLUGIN_LOADER_SOURCE)
  string out_format = plugin->blob_format();
  cerr << "  Blob format:      " << style::bold << out_format << style::reset
       << endl;
#endif

  // Initial delay
  if (delay > 0) {
    this_thread::sleep_for(chrono::milliseconds(delay));
  }

  // Main loop
  agent.register_event(event_type::startup);
  cerr << fg::green << PLUGIN_NAME " plugin started" << fg::reset << endl;

#if defined(PLUGIN_LOADER_SOURCE)
  json out, err;
  return_type rt;
  agent.loop(
      [&]() {
        vector<unsigned char> blob;
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
          agent.publish(out);
          if (blob.size() > 0) {
            json meta{{"format", out_format}};
            agent.publish(blob, meta);
          }
          break;
        case return_type::retry:
          return;
        case return_type::error:
          err = {{"error", {"get_output", plugin->error()}}};
          agent.register_event(event_type::message, err);
          count_err++;
          break;
        case return_type::critical:
          cerr << fg::red << "Critical error getting data: " << plugin->error()
               << fg::reset << endl;
          count_err++;
          Mads::running = false;
          return;
        }

        if (!silent) {
          cerr << "\r\x1b[0KMessages processed: " << fg::green << ++count
               << fg::reset << " total, " << fg::red << count_err << fg::reset
               << " with errors ";
          cerr.flush();
        }
      },
      time);

#elif defined(PLUGIN_LOADER_FILTER)
  json in, out = {}, err;
  return_type rt;
  message_type type;
  tuple<string, string> msg;
  agent.loop(
      [&]() {
        err.clear();
        try {
          type = agent.receive(dont_block);
        } catch (const AgentError &e) {
          cerr << fg::red << "Error receiving message: " << e.what()
               << fg::reset << endl;
        }
        agent.remote_control();
        if (agent.last_topic() == "control") {
          return; // Control message, already handled
        }

        // loading data into plugin
        if (type != message_type::none) {
          msg = agent.last_message();
          in = json::parse(get<1>(msg));
          rt = plugin->load_data(in, agent.last_topic());
        } else {
          goto process_output;
        }
        if (type != message_type::json) {
          return; // Not a JSON message
        }
        switch (rt) {
        case return_type::warning:
          err = {{"warning", {"load_data", plugin->error()}}};
          [[fallthrough]];
        case return_type::success:
          break; // next step
        case return_type::retry:
          return; // next iteration
        case return_type::error:
          err = {{"error", {"load_data", plugin->error()}}};
          agent.register_event(event_type::message, err);
          count_err++;
          goto status_line;
        case return_type::critical:
          cerr << fg::red << "Critical error loading data: " << plugin->error()
               << fg::reset << endl;
          err = {{"error", {"load_data", plugin->error()}}};
          agent.register_event(event_type::message, err);
          Mads::running = false;
          return;
        }
        // processing data in the plugin
      process_output:
        rt = plugin->process(out);
        if (!err.empty()) out.merge_patch(err);
        switch (rt) {
        case return_type::warning:
          out["warning"] = {{"process", plugin->error()}};
          [[fallthrough]];
        case return_type::success:
          break; // next step
        case return_type::retry:
          return; // next iteration
        case return_type::error:
          err = {{"error", {"process", plugin->error()}}};
          agent.register_event(event_type::message, err);
          count_err++;
          goto status_line;
        case return_type::critical:
          cerr << fg::red
               << "Critical error processing data: " << plugin->error()
               << fg::reset << endl;
          err = {{"error", {"process", plugin->error()}}};
          agent.register_event(event_type::message, err);
          Mads::running = false;
          return;
        }
        // publishing data
        agent.publish(out);
      status_line:
        if (!silent) {
          cerr << "\r\x1b[0KMessages processed: " << fg::green << ++count
               << fg::reset << " total, " << fg::red << count_err << fg::reset
               << " with errors ";
          cerr.flush();
        }
      },
      time);
#elif defined(PLUGIN_LOADER_SINK)
  message_type type;
  json in;
  return_type rt;
  agent.loop([&]() {
    try {
      type = agent.receive();
    } catch (const AgentError &e) {
      cerr << fg::red << "Error receiving message: " << e.what() << fg::reset
           << endl;
    }
    auto msg = agent.last_message();
    agent.remote_control();
    if (agent.last_topic() == "control") {
      return; // Control message, already handled
    }
    if (type != message_type::json) {
      return; // No message received
    }
    in = json::parse(get<1>(msg));
    rt = plugin->load_data(in, agent.last_topic());
    switch (rt) {
    case return_type::warning:
      cerr << fg::yellow << "Warning loading data: " << plugin->error()
           << fg::reset << endl;
      [[fallthrough]];
    case return_type::success:
    case return_type::retry:
      break;
    case return_type::error:
      cerr << fg::red << "Error loading data: " << plugin->error() << fg::reset
           << endl;
      count_err++;
      return;
    case return_type::critical:
      cerr << fg::red << "Critical error loading data: " << plugin->error()
           << fg::reset << endl;
      json msg = {{"error", {"load_data", plugin->error()}}};
      agent.register_event(event_type::message, msg);
      count_err++;
      Mads::running = false;
      return;
    }

    if (!silent) {
      cerr << "\r\x1b[0KMessages processed: " << fg::green << ++count
           << fg::reset << " total, " << fg::red << count_err << fg::reset
           << " with errors ";
      cerr.flush();
    }
  });
#endif
  cerr << fg::green << PLUGIN_NAME " plugin stopped" << fg::reset << endl;

  // Cleanup
  agent.register_event(event_type::shutdown);
  agent.disconnect();
  delete plugin;
  kernel.clear_drivers();

  if (agent.restart()) {
    auto cmd = string(MADS_PREFIX) + argv[0];
    cout << "Restarting " << cmd << "..." << endl;
    execvp(cmd.c_str(), argv);
  }
  return 0;
}