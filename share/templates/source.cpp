/*
  ____                                   _             _       
 / ___|  ___  _   _ _ __ ___ ___   _ __ | |_   _  __ _(_)_ __  
 \___ \ / _ \| | | | '__/ __/ _ \ | '_ \| | | | |/ _` | | '_ \ 
  ___) | (_) | |_| | | | (_|  __/ | |_) | | |_| | (_| | | | | |
 |____/ \___/ \__,_|_|  \___\___| | .__/|_|\__,_|\__, |_|_| |_|
                                  |_|            |___/         
{% include "header.tpl" %}
*/
// Mandatory included headers
#include <{{parent_header}}>
#include <nlohmann/json.hpp>
#include <pugg/Kernel.h>
// other includes as needed here

// Define the name of the plugin
#ifndef PLUGIN_NAME
#define PLUGIN_NAME "{{name}}"
#endif

// Load the namespaces
using namespace std;
using json = nlohmann::json;


// Plugin class. This shall be the only part that needs to be modified,
// implementing the actual functionality
class {{class_name}} : public {{parent}}<json> {

public:

  // Typically, no need to change this
  string kind() override { return PLUGIN_NAME; }

  // Implement the actual functionality here
  return_type get_output(json &out,
                         std::vector<unsigned char> *blob = nullptr) override {
    out.clear();

    // load the data as necessary and set the fields of the json out variable

    // This sets the agent_id field in the output json object, only when it is
    // not empty
    if (!_agent_id.empty()) out["agent_id"] = _agent_id;
    return return_type::success;
  }

  void set_params(void const *params) override {
    // Call the parent class method to set the common parameters 
    // (e.g. agent_id, etc.)
    Source::set_params(params);

    // provide sensible defaults for the parameters by setting e.g.
    _params["some_field"] = "default_value";
    // more here...

    // then merge the defaults with the actually provided parameters
    // params needs to be cast to json
    _params.merge_patch(*(json *)params);
  }

  // Implement this method if you want to provide additional information
  map<string, string> info() override { 
    // return a map of stringswith additional information about the plugin
    // it is used to print the information about the plugin when it is loaded
    // by the agent
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
INSTALL_{{driver_name}}_DRIVER({{class_name}}, json)


/*
                  _
  _ __ ___   __ _(_)_ __
 | '_ ` _ \ / _` | | '_ \
 | | | | | | (_| | | | | |
 |_| |_| |_|\__,_|_|_| |_|

For testing purposes, when directly executing the plugin
*/
int main(int argc, char const *argv[]) {
  {{class_name}} plugin;
  json output, params;

  // Set example values to params
  params["test"] = "value";

  // Set the parameters
  plugin.set_params(&params);

  // Process data
  plugin.get_output(output);

  // Produce output
  cout << "Output: " << output << endl;

  return 0;
}
