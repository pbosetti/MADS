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
{% if datastore %}
#include <datastore.hpp>
{% endif %}
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
  // Don't forget to inherit the constructors of the parent class
  // If you really need to implement your own constructor, make sure to call 
  // the parent constructor. Normally, use set_params() to set the parameters
  // instead of the constructor.
  using {{parent}}::{{parent}}; // inherit constructors

  // Typically, no need to change this: PLUGIN_NAME is defined by the build
  // system from the target name, and the host looks the driver up by that name.
  // Since protocol P8 kind() no longer selects the settings section (the agent
  // name does); it is checked against the driver name and a mismatch is a
  // startup warning.
  string kind() override { return PLUGIN_NAME; }

  // Implement the actual functionality here
  // Return types (full per-host semantics:
  //  .claude/skills/mads-plugin/reference/return-types.md):
  // return_type::success: the result is published
  // return_type::retry: go to next loop, nothing is published
  // return_type::warning: content of _error is added to result before publishing
  // return_type::error: _error is traced via register_event, don't publish
  // return_type::critical: execution stops
  return_type get_output(json &out, vector<unsigned char> *blob = nullptr) override {
    out.clear();

    // load the data as necessary and set the fields of the json out variable

    // This sets the agent_id field in the output json object, only when it is
    // not empty
    if (!_agent_id.empty()) out["agent_id"] = _agent_id;
    return return_type::success;
  }
{% if zero_copy %}
  // Zero-copy output (protocol P9). When this agent has `shm = true` in its
  // settings and its broker runs on this host, the host calls
  // get_output_shared() instead of get_output(), handing out a slot of the
  // agent's shared-memory ring: write the blob straight into it, and only a
  // small reference travels through the broker. get_output() above stays
  // mandatory: the host falls back to it when shared memory is off, and for
  // a blob that does not fit the slot.
  bool supports_shared_output() const override { return true; }

  // Fill `out` as in get_output(), write at most buf.capacity bytes of blob
  // into buf.data and set buf.size (0 = no blob). If the blob does not fit:
  // write nothing, set buf.size to the size it needs and return retry, without
  // consuming the pending data -- the host then calls get_output() for it.
  // buf.data is valid only during this call.
  return_type get_output_shared(json &out, BlobBuffer &buf) override {
    out.clear();

    // e.g. size_t n = produce_frame_into(buf.data, buf.capacity);
    buf.size = 0; // bytes written; 0 = no blob

    if (!_agent_id.empty()) out["agent_id"] = _agent_id;
    return return_type::success;
  }
{% else %}
  // Shared-memory zero-copy output (protocol P9): scaffold the plugin with
  // `mads plugin --zero-copy` to get the get_output_shared() hook.
{% endif %}

  void set_params(const json &params) override {
    // Call the parent class method to set the common parameters 
    // (e.g. agent_id, etc.)
    Source::set_params(params);

    // provide sensible defaults for the parameters by setting e.g.
    _params["some_field"] = "default_value";
    // more here...

    // then merge the defaults with the actually provided parameters
    // params needs to be cast to json
    _params.merge_patch(params);
    {% if datastore %}
    // prepare the datastore. Change its name if needed, .json ext is added if missing
    _datastore.prepare(kind());

    // use _datastore.data() to obtain the whole json object
    // use _datastore["key"] for read/write access a given key
    // use _datastore.save() for force saving; saving is automatic on destruction.
    {% endif %}
  }

  // Implement this method if you want to provide additional information
  map<string, string> info() override { 
    // return a map of strings with additional information about the plugin
    // it is used to print the information about the plugin when it is loaded
    // by the agent
    {% if datastore %}
    return {
      {"Datastore", _datastore.path()}
    };
    {% else %}
    return {};
    {% endif %}
  };

private:
  // Define the fields that are used to store internal resources
  {% if datastore %}
  Datastore _datastore;
  {% endif %}
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
MADS_REGISTER_PLUGINS({{class_name}})


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
  plugin.set_params(params);

  // Process data
  plugin.get_output(output);

  // Produce output
  cout << "Output: " << output << endl;
{% if zero_copy %}
  // Zero-copy path, with a local buffer standing in for the ring slot
  vector<unsigned char> slot(1024);
  BlobBuffer buf{slot.data(), slot.size(), 0};
  plugin.get_output_shared(output, buf);
  cout << "Shared output: " << output << " (" << buf.size << " blob bytes)"
       << endl;
{% endif %}
  return 0;
}
