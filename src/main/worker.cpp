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
#include "../worker.hpp"
#include "../agent_app.hpp"
#include <filesystem>
#include <pugg/Kernel.h>
#include <filter.hpp>
#define PLUGIN_NAME "worker"
#define PLUGIN_DEFAULT "bridge.plugin"
#define AGENT_NAME_DEFAULT "bridge"

using namespace std;
using namespace Mads;
using json = nlohmann::json;
namespace fs = std::filesystem;

using FilterJ = Filter<json, json>;
using FilterDriverJ = FilterDriver<json, json>;

int main(int argc, char *argv[]) {
  string plugin_name, plugin_file = PLUGIN_DEFAULT, agent_name = AGENT_NAME_DEFAULT;
  size_t count = 0, count_err = 0;

  // CLI options
  AgentAppFor<Worker> agent(argv[0], SETTINGS_URI);
  // clang-format off
  agent.options()
    ("plugin", "Plugin to load (must be a filter!)", cxxopts::value<string>())
    ("n,name", "Agent name (default to plugin name)", cxxopts::value<string>())
    ("i,agent-id", "Agent ID to be added to JSON frames", cxxopts::value<string>());
  agent.raw_options().parse_positional({"plugin"});
  agent.raw_options().positional_help("<Plugin to load (must be a filter!)>");
  // clang-format on
  // Also brings in --room, so the broker can be located by service discovery.
  agent.add_common_options();

  auto options_parsed = agent.parse_options(argc, argv);
  if (int rc = AgentApp::handle_standard_exit_options<AgentAppFor<Worker>>(
          options_parsed, agent.raw_options(), argv);
      rc >= 0) {
    return rc;
  }

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
  // Settings are looked up by agent name, so it has to be set before init().
  // init() re-applies --name on top, which keeps its precedence.
  agent.set_agent_name(agent_name);

  // Core stuff
  try {
    agent.init(options_parsed);
  } catch (const std::exception &e) {
    std::cout << fg::red << "Error initializing agent: " << e.what()
              << fg::reset << endl;
    exit(EXIT_FAILURE);
  }
  agent.enable_remote_control();
  // Worker::connect() wires up the PULL socket and defaults to no settle delay;
  // pass it explicitly, since AgentApp's own default is 250 ms.
  agent.connect(0ms);


  // Copy agent settings as plugin parameters (--agent-id is already applied to
  // the agent itself by init()).
  json settings = agent.settings_json();
  if (options_parsed.count("agent-id")) {
    settings["agent_id"] = options_parsed["agent-id"].as<string>();
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
  if (!kernel.load_plugin(plugin_file)) {
    cerr << fg::red << "Error: cannot load plugin file " << plugin_file
         << fg::reset << endl;
    exit(1);
  }
  FilterDriverJ *filter_driver =
      kernel.get_driver<FilterDriverJ>(FilterJ::server_name(), plugin_name);
  if (filter_driver == nullptr) {
    cerr << fg::red << "Error: cannot find plugin driver " << plugin_name
         << " in plugin at " << plugin_file << fg::reset << endl;
    auto drivers = kernel.get_all_drivers<FilterDriverJ>(FilterJ::server_name());
    cerr << "Available drivers:" << endl;
    for (auto &d : drivers) {
      cerr << "- " << d->name() << endl;
    }
    exit(1);
  }

  // Create the class from the plugin (P8: create() returns a unique_ptr):
  auto filter = filter_driver->create();
  filter->set_params(settings);
  for (auto &[k, v] : filter->info()) {
    cout << "  " << left << setw(18) << k << style::bold << v << style::reset
         << endl;
  }

  cout << "  Plugin:           " << style::bold << plugin_file << " (loaded as "
       << agent_name << ")" << style::reset << endl;

  // Main loop
  // Startup is registered here rather than via enable_events(): the plugin
  // checks above exit(1) on failure, and an agent that never loaded its plugin
  // should not report itself as started.
  agent.register_event(event_type::startup);
  cout << fg::green << "Filter plugin process started" << fg::reset << endl;
  agent.loop([&]() -> chrono::milliseconds {
    json payload = agent.pull();
    return_type rt;
    if (payload.empty() && !agent.runtime()->running()) return 0ms;
    // TODO: verify if we need to check return type
    // message_type type = agent.receive();
    agent.receive();
    // agent.remote_control();
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
    return 0ms;
  });
  cout << fg::green << "Filter plugin process stopped" << fg::reset << endl;

  // Cleanup
  agent.register_event(event_type::shutdown);
  agent.disconnect();
  filter.reset();
  kernel.clear_drivers();

  agent.restart_if_requested(argv);
  return 0;
}
