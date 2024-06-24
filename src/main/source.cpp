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
#include <source.hpp>
#include <pugg/Kernel.h>
#include <chrono>

using namespace std;
using namespace cxxopts;
using namespace Mads;
using json = nlohmann::json;
namespace fs = std::filesystem;

using SourceJ = Source<json>;
using SourceDriverJ = SourceDriver<json>;

int main(int argc, char *argv[]) {
  string settings_uri = SETTINGS_URI;
  string plugin_name, plugin_file;
  size_t count = 0, count_err = 0;
  chrono::milliseconds time{0};

  // CLI options
  Options options(argv[0]);
  options.add_options()
    ("plugin", "Plugin to load", value<string>())
    ("i,agent_id", "Agent ID to be added to JSON frames", value<string>())
    ("p", "Sampling period (default 100 ms)", value<size_t>());
  options.parse_positional({"plugin"});
  options.positional_help("plugin");
  SETUP_OPTIONS(options, Agent);

  if (options_parsed.count("plugin") == 0) {
    cout << options.help() << endl << "No plugin specified" << endl;
    exit(1);
  }
  plugin_file = options_parsed["plugin"].as<string>();
  if (!fs::exists(plugin_file)) {
    fs::path parent_dir = fs::path(argv[0]).parent_path().parent_path();
    plugin_file = (parent_dir / "lib" / plugin_file).string();
  }
  plugin_name = fs::path(plugin_file).stem().string();

  // Loading plugin
  pugg::Kernel kernel;
  kernel.add_server<Source<>>();
  kernel.load_plugin(plugin_file);
  SourceDriverJ *source_driver =
      kernel.get_driver<SourceDriverJ>(SourceJ::server_name(), plugin_name);
  if (source_driver == nullptr) {
    cout << fg::red << "Error: cannot find plugin driver " << plugin_name
         << " in plugin at " << plugin_file << fg::reset << endl;
    auto drivers = kernel.get_all_drivers<SourceDriverJ>(SourceJ::server_name());
    cout << "Available drivers:" << endl;
    for (auto &d : drivers) {
      cout << "- " << d->name() << endl;
    }
    exit(1);
  }
  // Create the class from the plugin:
  SourceJ *source = source_driver->create();

  // Core stuff
  Agent agent(plugin_name, settings_uri);
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
       << plugin_name << ")" << style::reset << endl;

  // Copy agent settings as plugin parameters
  json settings = agent.get_settings();
  if (options_parsed.count("agent_id")) {
    settings["agent_id"] = options_parsed["agent_id"].as<string>();
    agent.set_agent_id(options_parsed["agent_id"].as<string>());
  }
  cout << "  Sampling period:  " << style::bold;
  if (options_parsed.count("p") != 0) {
    time = chrono::milliseconds(options_parsed["p"].as<size_t>());
    cout << fg::yellow << time.count() << " ms" << " (from -p option)" 
         << fg::reset << style::reset << endl;
  } else if (!settings["period"].is_null()) {
    time = chrono::milliseconds(settings["period"].get<size_t>());
    cout << time.count() << " ms" << " (from settings)" 
         << style::reset << endl;
  }  else {
    cout << fg::red << "free run" << fg::reset << style::reset 
         << " (default)" << endl;
  }


  source->set_params((void *)&settings);
  string out_format = source->blob_format();
  for (auto &[k, v] : source->info()) {
    cout << "  " << left << setw(18) << k << style::bold << v << style::reset 
         << endl;
  }
  cout << "  Blob format:      " << style::bold << out_format << style::reset
       << endl;

  // Main loop
  agent.register_event(event_type::startup);
  cout << fg::green << "Source plugin process started" << fg::reset << endl;
  agent.loop([&]() {
    json out;
    vector<unsigned char> blob;
    return_type result = source->get_output(out, &blob);
    if (return_type::success == result) {
      agent.publish(out);
      if (blob.size() > 0) {
        json meta{{"format", out_format}};
        agent.publish(blob, meta);
      }
    } else if (result == return_type::critical) {
      Mads::running = false;
      json msg{
        {"error", source->error()}
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
    cout << "\r\x1b[0KMessages processed: " << fg::green << count
          << fg::reset << " total, with " << fg::red << count_err << fg::reset
          << " errors";
    cout.flush();
  }, time);
  cout << fg::green << "Source plugin process stopped" << fg::reset << endl;

  // Cleanup
  agent.register_event(event_type::shutdown);
  agent.disconnect();
  delete source;
  kernel.clear_drivers();

  if (agent.restart()) {
    cout << "Restarting..." << endl;
    execvp(argv[0], argv);
  }
  return 0;
}