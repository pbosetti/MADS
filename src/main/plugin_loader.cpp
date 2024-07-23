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
#include "../mads.hpp"
#include "../exec_path.hpp"
#include <cxxopts.hpp>
#include <filesystem>
#include <pugg/Kernel.h>

#if defined(PLUGIN_LOADER_SOURCE)
  #include <source.hpp>
  #include <chrono>
  #define PLUGIN_CLASS Source
  #define PLUGIN_NAME "Source"
  #define PLUGIN_DEFAULT "publish.plugin"
#elif defined(PLUGIN_LOADER_FILTER)
  #include <filter.hpp>
  #define PLUGIN_CLASS Filter
  #define PLUGIN_NAME "Filter"
  #define PLUGIN_DEFAULT "bridge.plugin"
#elif defined(PLUGIN_LOADER_SINK)
  #include <sink.hpp>
  #define PLUGIN_CLASS Sink
  #define PLUGIN_NAME "Sink"
  #define PLUGIN_DEFAULT "feedback.plugin"
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
  string plugin_name, plugin_file, agent_name;
  size_t count = 0, count_err = 0;
  chrono::milliseconds time{0};

  // CLI options
  Options options(argv[0]);
  options.add_options()
    ("plugin", "Plugin to load", value<string>())
    ("n,name", "Agent name (default to plugin name)", value<string>())
    ("i,agent-id", "Agent ID to be added to JSON frames", value<string>());
  #if defined(PLUGIN_LOADER_SOURCE)
  options.add_options()
    ("p,period", "Sampling period (default 100 ms)", value<size_t>());
  #endif
  options.parse_positional({"plugin"});
  options.positional_help("plugin");
  SETUP_OPTIONS(options, Agent);

  if (options_parsed.count("plugin") == 0) {
    if (string("none") == PLUGIN_DEFAULT) {
      cerr << options.help() << endl << "No plugin specified" << endl;
      options.show_positional_help();
      exit(1);
    }
    plugin_file = PLUGIN_DEFAULT;
  } else {
    plugin_file = options_parsed["plugin"].as<string>();
  }
  if (!fs::exists(plugin_file)) {
    plugin_file = Mads::exec_dir("../lib/" + plugin_file);
  }
  plugin_name = fs::path(plugin_file).stem().string();
  if (options_parsed.count("name") != 0) {
    agent_name = options_parsed["name"].as<string>();
  } else {
    agent_name = plugin_name;
  }

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

  // Core stuff
  Agent agent(agent_name, settings_uri);
  try {
    agent.init();
  } catch (const std::exception &e) {
    cerr << fg::red << "Error initializing agent: " << e.what()
         << fg::reset << endl;
    exit(EXIT_FAILURE);
  }
  agent.enable_remote_control();
  agent.connect();
  agent.info(cerr);
  cerr << "  Plugin:           " << style::bold << plugin_file << " (loaded as "
       << agent_name << ")" << style::reset << endl;

  // Copy agent settings as plugin parameters
  json settings = agent.get_settings();
  if (options_parsed.count("agent-id")) {
    settings["agent_id"] = options_parsed["agent-id"].as<string>();
    agent.set_agent_id(options_parsed["agent-id"].as<string>());
  }
#if defined(PLUGIN_LOADER_SOURCE)
  cerr << "  Sampling period:  " << style::bold;
  if (options_parsed.count("p") != 0) {
    time = chrono::milliseconds(options_parsed["p"].as<size_t>());
    cerr << fg::yellow << time.count() << " ms" << " (from -p option)" 
         << fg::reset << style::reset << endl;
  } else if (!settings["period"].is_null()) {
    time = chrono::milliseconds(settings["period"].get<size_t>());
    cerr << time.count() << " ms" << " (from settings)" 
         << style::reset << endl;
  }  else {
    cerr << fg::red << "free run" << fg::reset << style::reset 
         << " (default)" << endl;
  }
#endif

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

  // Main loop
  agent.register_event(event_type::startup);
  cerr << fg::green << PLUGIN_NAME " plugin started" << fg::reset << endl;

#if defined(PLUGIN_LOADER_SOURCE)
  agent.loop([&]() {
    json out;
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
      json msg{
        {"error", plugin->error()}
      };
      this_thread::sleep_for(chrono::milliseconds(1000));
      agent.register_event(event_type::marker, msg);
      return;
    } else if (result == return_type::error) {
      count_err++;
    } else if (result == return_type::retry) {
      return;
    }
    count++;
    cerr << "\r\x1b[0KMessages processed: " << fg::green << count
          << fg::reset << " total, " << fg::red << count_err << fg::reset
          << " with errors ";
    cerr.flush();
  }, time);
  
#elif defined(PLUGIN_LOADER_FILTER)
  agent.loop([&]() {
    message_type type = agent.receive();
    auto msg = agent.last_message();
    agent.remote_control();
    if (type == message_type::json && agent.last_topic() != "control") {
      json in = json::parse(get<1>(msg));
      json out;
      return_type rt;
      double timecode = in.value("timecode", 0.0);
      rt = plugin->load_data(in, agent.last_topic());
      if (rt != return_type::success) {
        out = {{"error", plugin->error()}};
        count_err++;
      } else {
        rt = plugin->process(out);
        if (rt != return_type::success) {
          out = {{"error", plugin->error()}};
          count_err++;
        }
        out["timecode"] = timecode;
      }
      agent.publish(out);
      cerr << "\r\x1b[0KMessages processed: " << fg::green << count++
           << fg::reset << " total, " << fg::red << count_err << fg::reset
           << " with errors ";
      cerr.flush();
    }
  });
#elif defined(PLUGIN_LOADER_SINK)
  agent.loop([&]() {
    message_type type = agent.receive();
    auto msg = agent.last_message();
    agent.remote_control();
    if (type == message_type::json && agent.last_topic() != "control") {
      json in = json::parse(get<1>(msg));
      json out;
      return_type processed = plugin->load_data(in, agent.last_topic());
      if (processed != return_type::success) {
        out = {{"error", plugin->error()}};
        count_err++;
      }
      cerr << "\r\x1b[0KMessages processed: " << fg::green << count++
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
    cerr << "Restarting..." << endl;
    execvp(argv[0], argv);
  }
  return 0;
}