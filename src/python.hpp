/*
   ____
  / ___|___  _ __ ___  _ __ ___   ___  _ __
 | |   / _ \| '_ ` _ \| '_ ` _ \ / _ \| '_ \
 | |__| (_) | | | | | | | | | | | (_) | | | |
  \____\___/|_| |_| |_|_| |_| |_|\___/|_| |_|

Common header for source, filter and sink python plugins
*/
#include <cppy3/cppy3.hpp>
#include <cppy3/utils.hpp>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <iostream>
#include <rang.hpp>
#include "mads.hpp"
#include "exec_path.hpp"
#include "agent.hpp"

using namespace std;
using namespace filesystem;
using namespace rang;
using json = nlohmann::json;

namespace Mads {

class PythonInterpreter {
public:
  PythonInterpreter(json const &params, string module)
      : _params(params), _python_module(module) {
    setup_venv();
    cppy3::exec("import sys");
    for (auto &path : _default_paths) {
      cppy3::exec("sys.path.append('" + path + "')");
    }
    for (auto &path : _params["search_paths"]) {
      if (path.is_string()) {
        cppy3::exec("sys.path.append('" + path.get<string>() + "')");
      } else if (path.is_array()) {
        for (auto &p : path) {
          cppy3::exec("sys.path.append('" + p.get<string>() + "')");
        }
      }
    }
    try {
      cppy3::exec("import " + _python_module + " as mads");
    } catch (cppy3::PythonException &e) {
      cerr << "Error loading module: " << e.what();
      exit(EXIT_FAILURE);
    }
    cppy3::exec(R"(
import json
mads.data = {}
mads.state = {}
mads.topic = ''
    )");
    cppy3::exec("mads.params = json.loads('" + params.dump() + "')");
    try {
      cppy3::exec("mads.setup()");
    } catch (cppy3::PythonException &e) {
      cerr << "[Python] running setup(): " << e.what();
    }
    // Get Agent type
    try {
      const cppy3::Var at = cppy3::eval("mads.agent_type");
      _agent_type = cppy3::WideToUTF8(at.toString());
    } catch (cppy3::PythonException &e) {
      cerr << fg::yellow
                << "[Python] Agent type unspecified, using default" << fg::reset
                << endl;
    }
    if (_agent_type != "source" && _agent_type != "filter" &&
        _agent_type != "sink") {
      cerr << fg::yellow
                << "[Python] Agent type not recognized, using default"
                << fg::reset << endl;
      _agent_type = "source";
    }
    cerr << "[Python] Agent type: " << _agent_type << endl;
  }

  void setup_venv() {
    if (_params["venv"].is_null() ||
        _params["venv"].get<string>().empty()) {
      char *venv_path = getenv("VIRTUAL_ENV");
      if (venv_path && strlen(venv_path) > 0) {
        cerr
            << "[Python] Using virtual environment from VIRTUAL_ENV shell var: "
            << venv_path << endl;
        _params["venv"] = string(venv_path);
      } else {
        cerr << "[Python] No virtual environment specified, using system "
                     "Python libraries"
                  << endl;
        return;
      }
    }

    if (!exists(_params["venv"].get<path>())) {
      throw runtime_error("Virtual environment does not exist: " +
                               _params["venv"].get<string>());
    } else {
      path venv = _params["venv"].get<path>();
      path lib = venv / "lib";
      path site_packages;
      for (auto &entry : directory_iterator(lib)) {
        if (entry.is_directory() && entry.path().filename().string().find(
                                        "python") != string::npos) {
          site_packages = entry.path() / "site-packages";
          break;
        }
      }
      if (site_packages.empty()) {
        throw runtime_error(
            "Could not find site-packages in virtual environment: " +
            venv.string());
      }
      cppy3::exec("import site");
      cppy3::exec("site.addsitedir('" + site_packages.string() + "')");
    }
  }

  string agent_type() const { return _agent_type; }
  string python_module() const { return _python_module; }

protected:
  json _params;
  cppy3::PythonVM _python;
  string _python_module;
  string _agent_type = "source";
  // clang-format off
  vector<string> _default_paths = {
      "./python",      
      "./scripts",           
      "../python",
      "../scripts", 
      "../../python",
      "../../scripts", 
      Mads::prefix() + "/python", 
      Mads::prefix() + "/scripts"};
  // clang-format on
};

} // namespace Mads