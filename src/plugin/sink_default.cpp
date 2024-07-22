/*
  ____  _       _            _             _       
 / ___|(_)_ __ | | __  _ __ | |_   _  __ _(_)_ __  
 \___ \| | '_ \| |/ / | '_ \| | | | |/ _` | | '_ \ 
  ___) | | | | |   <  | |_) | | |_| | (_| | | | | |
 |____/|_|_| |_|_|\_\ | .__/|_|\__,_|\__, |_|_| |_|
                      |_|            |___/         
Default sink plugin: provides feedback on the console
*/

// Mandatory included headers
#include <nlohmann/json.hpp>
#include <pugg/Kernel.h>
#include <sink.hpp>
#include <rang.hpp>
// other includes as needed here

// Define the name of the plugin
#ifndef PLUGIN_NAME
#define PLUGIN_NAME "feedback"
#endif

// Load the namespaces
using namespace std;
using json = nlohmann::json;
using namespace rang;


// Plugin class. This shall be the only part that needs to be modified,
// implementing the actual functionality
class FeedbackPlugin : public Sink<json> {

public:

  string kind() override { return PLUGIN_NAME; }

  return_type load_data(json const &input, string topic = "") override {
    if (topic == "logger_status") {
      return return_type::retry;
    }
    if (_print_width > 0) {
      cout << topic << ": " << input.dump().substr(0, _print_width) << "..."
           << endl;
    } else {
      cout << topic << ": " << input.dump(_indent_width) << endl;
    }
    return return_type::success;
  }

  void set_params(void const *params) override { 
    Sink::set_params(params);
    _params["print_width"] = 60;
    _params["indent_width"] = 0;
    _params.merge_patch(*(json *)params);

    _print_width = _params["print_width"].get<int>();
    _indent_width = _params["indent_width"].get<int>();
  }

  // Implement this method if you want to provide additional information
  map<string, string> info() override {
    return {{"print_width", to_string(_params["print_width"].get<int>())},
            {"indent_width", to_string(_params["indent_width"].get<int>())}};
  };

private:
  // Define the fields that are used to store internal resources
  int _print_width;
  int _indent_width;
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
INSTALL_SINK_DRIVER(FeedbackPlugin, json)


