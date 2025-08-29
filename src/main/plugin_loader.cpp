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
    ("d,delay", "Initial delay before forst message in ms (default 0)", value<size_t>());
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
  json out;
  agent.loop(
      [&]() {
        vector<unsigned char> blob;
        return_type result = plugin->get_output(out, &blob);
        if (return_type::success == result) {
          agent.publish(out);
          if (blob.size() > 0) {
            json meta{{"format", out_format}};
            agent.publish(blob, meta);
          }
        } else if (result == return_type::critical) {
          Mads::running = false;
          json msg{{"error", plugin->error()}};
          this_thread::sleep_for(chrono::milliseconds(1000));
          agent.register_event(event_type::marker, msg);
          return;
        } else if (result == return_type::error) {
          count_err++;
          out["error"] = plugin->error();
          agent.publish(out);
        } else if (result == return_type::retry) {
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
  json in, out;
  return_type rt;
  message_type type;
  agent.loop(
      [&]() {
        try {
          type = agent.receive(dont_block);
        } catch (const AgentError &e) {
          cerr << fg::red << "Error receiving message: " << e.what()
               << fg::reset << endl;
        }
        if (type == message_type::none && !dont_block) {
          return; // No message received
        }
        auto msg = agent.last_message();
        agent.remote_control();
        if (agent.last_topic() != "control") {
          if (type == message_type::json) {
            in = json::parse(get<1>(msg));
            rt = plugin->load_data(in, agent.last_topic());
            if (rt != return_type::success) {
              out = {{"error", plugin->error()}};
              count_err++;
            }
          }
          rt = plugin->process(out);
          if (rt != return_type::success) {
            out = {{"error", plugin->error()}};
            count_err++;
          }
          agent.publish(out);
          if (!silent) {
            cerr << "\r\x1b[0KMessages processed: " << fg::green << ++count
                 << fg::reset << " total, " << fg::red << count_err << fg::reset
                 << " with errors ";
            cerr.flush();
          }
        }
      },
      time);
#elif defined(PLUGIN_LOADER_SINK)
  message_type type;
  json in;
  agent.loop([&]() {
    try {
      type = agent.receive();
    } catch (const AgentError &e) {
      cerr << fg::red << "Error receiving message: " << e.what() << fg::reset
           << endl;
    }
    auto msg = agent.last_message();
    agent.remote_control();
    if (type == message_type::json && agent.last_topic() != "control") {
      in = json::parse(get<1>(msg));
      return_type processed = plugin->load_data(in, agent.last_topic());
      if (processed == return_type::retry) {
        return;
      } else if (processed != return_type::success) {
        cerr << fg::red << "Error loading data: " << plugin->error() << fg::reset
             << endl;
        count_err++;
      }
      if (!silent) {
        cerr << "\r\x1b[0KMessages processed: " << fg::green << ++count
             << fg::reset << " total, " << fg::red << count_err << fg::reset
             << " with errors ";
        cerr.flush();
      }
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