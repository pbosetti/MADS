/*
  _____ _ _ _                                     _
 |  ___(_) | |_ ___ _ __    __ _  __ _  ___ _ __ | |_
 | |_  | | | __/ _ \ '__|  / _` |/ _` |/ _ \ '_ \| __|
 |  _| | | | ||  __/ |    | (_| | (_| |  __/ | | | |_
 |_|   |_|_|\__\___|_|     \__,_|\__, |\___|_| |_|\__|
                                 |___/

This is a plugin-based general purpose filter for the Mads framework.
Actual work is done by the plugins, this executable is just a wrapper.
Author(s): Paolo Bosetti
*/
#include "../agent.hpp"
#include "../mads.hpp"
#include <cxxopts.hpp>
#include <filesystem>
#include <filter.hpp>
#include <pugg/Kernel.h>

using namespace std;
using namespace cxxopts;
using namespace Mads;
using json = nlohmann::json;
namespace fs = std::filesystem;

using FilterJ = Filter<json, json>;
using FilterDriverJ = FilterDriver<json, json>;

int main(int argc, char *argv[]) {
  string settings_uri = SETTINGS_URI;
  string plugin_name, plugin_file, agent_name;
  size_t count = 0, count_err = 0;

  // CLI options
  Options options(argv[0]);
  options.add_options()
    ("plugin", "Plugin to load", value<string>())
    ("n,name", "Agent name (default to plugin name)", value<string>())
    ("i,agent_id", "Agent ID to be added to JSON frames", value<string>());
  options.parse_positional({"plugin"});
  options.positional_help("plugin");
  SETUP_OPTIONS(options, Agent);

  if (options_parsed.count("plugin") == 0) {
    cout << options.help() << endl << "No plugin specified" << endl;
    exit(1);
  }
  plugin_file = options_parsed["plugin"].as<string>();
  if (!fs::exists(plugin_file)) {
    fs::path parent_dir = THIS_EXEC_DIR.parent_path();
    plugin_file = (parent_dir / "lib" / plugin_file).string();
  }
  plugin_name = fs::path(plugin_file).stem().string();
  if (options_parsed.count("name") != 0) {
    agent_name = options_parsed["name"].as<string>();
  } else {
    agent_name = plugin_name;
  }

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

  // Core stuff
  Agent agent(agent_name, settings_uri);
  try {
    agent.init();
  } catch (const std::exception &e) {
    std::cout << fg::red << "Error initializing agent: " << e.what()
              << fg::reset << endl;
    exit(EXIT_FAILURE);
  }
  agent.enable_remote_control();
  agent.connect();
  agent.info();
  cout << "  Plugin:           " << style::bold << plugin_file << " (loaded as "
       << agent_name << ")" << style::reset << endl;

  // Copy agent settings as plugin parameters
  json settings = agent.get_settings();
  if (options_parsed.count("agent_id")) {
    settings["agent_id"] = options_parsed["agent_id"].as<string>();
    agent.set_agent_id(options_parsed["agent_id"].as<string>());
  }
  filter->set_params((void *)&settings);
  for (auto &[k, v] : filter->info()) {
    cout << "  " << left << setw(18) << k << style::bold << v << style::reset 
         << endl;
  }

  // Main loop
  agent.register_event(event_type::startup);
  cout << fg::green << "Filter plugin process started" << fg::reset << endl;
  agent.loop([&]() {
    message_type type = agent.receive();
    auto msg = agent.last_message();
    agent.remote_control();
    if (type == message_type::json && agent.last_topic() != "control") {
      json in = json::parse(get<1>(msg));
      json out;
      double timecode = in["timecode"];
      filter->load_data(in);
      return_type processed = filter->process(out);
      if (processed != return_type::success) {
        out = {{"error", filter->error()}};
        count_err++;
      }
      out["timecode"] = timecode;
      agent.publish(out);
      cout << "\r\x1b[0KMessages processed: " << fg::green << count++
           << fg::reset << " total, " << fg::red << count_err << fg::reset
           << " with errors";
      cout.flush();
    }
  });
  cout << fg::green << "Filter plugin process stopped" << fg::reset << endl;

  // Cleanup
  agent.register_event(event_type::shutdown);
  agent.disconnect();
  delete filter;
  kernel.clear_drivers();

  if (agent.restart()) {
    cout << "Restarting..." << endl;
    execvp(argv[0], argv);
  }
  return 0;
}