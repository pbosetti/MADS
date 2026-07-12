// Helper translation unit for the plugin_p6 fixture: exercises a
// set_params call site passing a raw pointer (pre-P7 calling convention),
// which the P6->P7 migration step rewrites to pass by reference instead.
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class P6demoPlugin;

void configure(P6demoPlugin &instance, json &params) {
  instance.set_params(&params);
}
