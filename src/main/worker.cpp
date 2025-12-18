/*
 __        __         _                                      _   
 \ \      / /__  _ __| | _____ _ __    __ _  __ _  ___ _ __ | |_ 
  \ \ /\ / / _ \| '__| |/ / _ \ '__|  / _` |/ _` |/ _ \ '_ \| __|
   \ V  V / (_) | |  |   <  __/ |    | (_| | (_| |  __/ | | | |_ 
    \_/\_/ \___/|_|  |_|\_\___|_|     \__,_|\__, |\___|_| |_|\__|
                                            |___/                
This is a plugin-based general purpose worker for the Mads framework.
Actual work is done by the plugins, this executable is just a wrapper.
Author(s): Paolo Bosetti
*/
#include "../agent.hpp"
#include "../mads.hpp"
#include "../worker.hpp"
#include "../exec_path.hpp"
#include <cxxopts.hpp>
#include <filesystem>
#include <pugg/Kernel.h>
#include <filter.hpp>
#define PLUGIN_NAME "worker"
#define PLUGIN_DEFAULT "bridge.plugin"
#define AGENT_NAME_DEFAULT "bridge"

using namespace std;
using namespace cxxopts;
using namespace Mads;
using json = nlohmann::json;
namespace fs = std::filesystem;

using FilterJ = Filter<json, json>;
using FilterDriverJ = FilterDriver<json, json>;

int main(int argc, char *argv[]) {
  string settings_uri = SETTINGS_URI;
  string plugin_name, plugin_file = PLUGIN_DEFAULT, agent_name = AGENT_NAME_DEFAULT;
  size_t count = 0, count_err = 0;
  bool crypto = false;
  filesystem::path key_dir(Mads::exec_dir() + "/../etc");
  string client_key_name = "client";
  string server_key_name = "broker";
  auth_verbose auth_verbose = auth_verbose::off;

  // CLI options
  Options options(argv[0]);
  // clang-format off
  options.add_options()
    ("plugin", "Plugin to load (must be a filter!)", value<string>())
    ("n,name", "Agent name (default to plugin name)", value<string>())
    ("i,agent-id", "Agent ID to be added to JSON frames", value<string>());
  options.parse_positional({"plugin"});
  options.positional_help("<Plugin to load (must be a filter!)>");
  // clang-format on
  SETUP_OPTIONS(options, Agent);
  if (options_parsed.count("plugin") != 0) {
    plugin_file = options_parsed["plugin"].as<string>();
    agent_name = fs::path(plugin_file).stem().string();
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

  if (options_parsed.count("crypto") != 0) {
    crypto = true;
    if (options_parsed.count("keys_dir") != 0) {
      key_dir = options_parsed["keys_dir"].as<string>();
    }
    if (options_parsed.count("key_server") != 0) {
      server_key_name = options_parsed["key_server"].as<string>();
    }
    if (options_parsed.count("key_client") != 0) {
      client_key_name = options_parsed["key_client"].as<string>();
    }
    if (options_parsed.count("auth_verbose") != 0) {
      auth_verbose = auth_verbose::on;
    }
  }
  
  // Core stuff
  Worker agent(agent_name, settings_uri);
  if (crypto) {
    agent.set_key_dir(key_dir);
    agent.client_key_name = client_key_name;
    agent.server_key_name = server_key_name;
    agent.auth_verbose = auth_verbose;
  }
  try {
    agent.init(crypto);
  } catch (const std::exception &e) {
    std::cout << fg::red << "Error initializing agent: " << e.what()
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

  agent.info(cerr);


  // Loading plugin
  pugg::Kernel kernel;
  kernel.add_server<Filter<>>();
  kernel.load_plugin(plugin_file);
  FilterDriverJ *filter_driver =
      kernel.get_driver<FilterDriverJ>(FilterJ::server_name(), plugin_name);
  if (filter_driver == nullptr) {
    cout << fg::red << "Error: cannot find plugin driver " << plugin_name
         << " in plugin at " << plugin_file << fg::reset << endl;
    auto drivers = kernel.get_all_drivers<FilterDriverJ>(FilterJ::server_name());
    cout << "Available drivers:" << endl;
    for (auto &d : drivers) {
      cout << "- " << d->name() << endl;
    }
    exit(1);
  }
  
  // Create the class from the plugin:
  FilterJ *filter = filter_driver->create();
  filter->set_params((void *)&settings);
  for (auto &[k, v] : filter->info()) {
    cout << "  " << left << setw(18) << k << style::bold << v << style::reset 
         << endl;
  }

  cout << "  Plugin:           " << style::bold << plugin_file << " (loaded as "
       << agent_name << ")" << style::reset << endl;

  // Main loop
  agent.register_event(event_type::startup);
  cout << fg::green << "Filter plugin process started" << fg::reset << endl;
  agent.loop([&]() {
    json payload = agent.pull();
    return_type rt;
    if (payload.empty() && !Mads::running) return;
    message_type type = agent.receive();
    agent.remote_control();
    json out;
    rt = filter->load_data(payload, agent.last_topic());
    if (rt != return_type::success) {
      out = {{"error", filter->error()}};
      count_err++;
    } else {
      rt = filter->process(out);
      if (rt != return_type::success) {
        out = {{"error", filter->error()}};
        count_err++;
      }
    }
    agent.publish(out);
    cout << "\r\x1b[0KMessages processed: " << fg::green << ++count
         << fg::reset << " total, " << fg::red << count_err << fg::reset
         << " with errors";
    cout.flush();
  });
  cout << fg::green << "Filter plugin process stopped" << fg::reset << endl;

  // Cleanup
  agent.register_event(event_type::shutdown);
  agent.disconnect();
  delete filter;
  kernel.clear_drivers();

  if (agent.restart()) {
    auto cmd = string(MADS_PREFIX) + argv[0];
    cout << "Restarting " << cmd << "..." << endl;
    execvp(cmd.c_str(), argv);
  }
  return 0;
}