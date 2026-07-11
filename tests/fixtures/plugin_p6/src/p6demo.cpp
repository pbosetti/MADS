// Fake P6-protocol plugin source used as migration input by
// tests/test_plugin_migrate.cpp. The method signatures below deliberately
// mirror the *pre*-P6->P7 shapes described in
// share/plugin_migrations/P6-P7.json, plus a couple of tricky lexical cases
// (a comment and a string literal that contain '(' right next to a real
// signature, and a signature split across several lines).
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using json = nlohmann::json;
using namespace std;

class P6demoPlugin {
public:
  const char *kind() override { return "source"; }

  // set_params(...) used to take an opaque pointer (pre-P7 ABI)
  std::string debug_msg = "about to call set_params(...) with a raw pointer";
  void set_params(void *params) override {
    _params.merge_patch(*(json *)params);
  }

  // load_data(...) reads the next sample (raw pointer form used pre-P7)
  bool load_data(
      json const &input
  ) override {
    _last = input;
    return true;
  }

  bool process() override {
    /* process() takes no arguments yet (pre-P7 signature) */
    return true;
  }

  bool get_output(json &out) override {
    out = _last;
    return true;
  }

private:
  json _params;
  json _last;
};

INSTALL_SOURCE_DRIVER(P6demoPlugin, json)
