/*
  _____ _ _ _                    _             _       
 |  ___(_) | |_ ___ _ __   _ __ | |_   _  __ _(_)_ __  
 | |_  | | | __/ _ \ '__| | '_ \| | | | |/ _` | | '_ \ 
 |  _| | | | ||  __/ |    | |_) | | |_| | (_| | | | | |
 |_|   |_|_|\__\___|_|    | .__/|_|\__,_|\__, |_|_| |_|
                          |_|            |___/         
Default filter plugin: acts as a bridge, printing to stdout the received input 
json and reading from stdin a json that is then published as output
*/
// Mandatory included headers
#include <filter.hpp>
#include <nlohmann/json.hpp>
#include <pugg/Kernel.h>
// other includes as needed here

// Define the name of the plugin
#ifndef PLUGIN_NAME
#define PLUGIN_NAME "bridge"
#endif

// Load the namespaces
using namespace std;
using json = nlohmann::json;


// Plugin class. This shall be the only part that needs to be modified,
// implementing the actual functionality
class BridgePlugin : public Filter<json, json> {

public:

  string kind() override { return PLUGIN_NAME; }

  return_type load_data(json const &input, string topic) override {
    json out;
    out["topic"] = topic;
    out["input"] = input;
    cout << out.dump() << endl;
    return return_type::success;
  }

  return_type process(json &out) override {
    out.clear();
    string line;
    getline(cin, line);
    try{
     out = json::parse(line);
    } catch (json::parse_error &e) {
      _error = e.what();
      return return_type::error;
    }    
    if (!_agent_id.empty()) out["agent_id"] = _agent_id;
    return return_type::success;
  }
  
  void set_params(void const *params) override {
    Filter::set_params(params);
    _params.merge_patch(*(json *)params);
  }

  map<string, string> info() override { 
    return {}; 
  };

private:
  // Define the fields that are used to store internal resources
};


/*
  ____  _             _             _      _
 |  _ \| |_   _  __ _(_)_ __     __| |_ __(_)_   _____ _ __
 | |_) | | | | |/ _` | | '_ \   / _` | '__| \ \ / / _ \ '__|
 |  __/| | |_| | (_| | | | | | | (_| | |  | |\ V /  __/ |
 |_|   |_|\__,_|\__, |_|_| |_|  \__,_|_|  |_| \_/ \___|_|
                |___/
Enable the class as plugin
*/
INSTALL_FILTER_DRIVER(BridgePlugin, json, json);




