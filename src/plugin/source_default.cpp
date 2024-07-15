/*
  ____                                   _             _       
 / ___|  ___  _   _ _ __ ___ ___   _ __ | |_   _  __ _(_)_ __  
 \___ \ / _ \| | | | '__/ __/ _ \ | '_ \| | | | |/ _` | | '_ \ 
  ___) | (_) | |_| | | | (_|  __/ | |_) | | |_| | (_| | | | | |
 |____/ \___/ \__,_|_|  \___\___| | .__/|_|\__,_|\__, |_|_| |_|
                                  |_|            |___/         
Default source plugin: reads from stdin a valid json string
*/
// Mandatory included headers
#include <source.hpp>
#include <nlohmann/json.hpp>
#include <pugg/Kernel.h>
// other includes as needed here

// Define the name of the plugin
#ifndef PLUGIN_NAME
#define PLUGIN_NAME "publish"
#endif

// Load the namespaces
using namespace std;
using json = nlohmann::json;


// Plugin class. This shall be the only part that needs to be modified,
// implementing the actual functionality
class PublishPlugin : public Source<json> {

public:

  string kind() override { return PLUGIN_NAME; }

  return_type get_output(json &out,
                         std::vector<unsigned char> *blob = nullptr) override {
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
    Source::set_params(params);
    _params.merge_patch(*(json *)params);
  }

  // Implement this method if you want to provide additional information
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
INSTALL_SOURCE_DRIVER(PublishPlugin, json)


