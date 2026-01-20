/*
  ____        _   _                                          _
 |  _ \ _   _| |_| |__   ___  _ __     __ _  __ _  ___ _ __ | |_
 | |_) | | | | __| '_ \ / _ \| '_ \   / _` |/ _` |/ _ \ '_ \| __|
 |  __/| |_| | |_| | | | (_) | | | | | (_| | (_| |  __/ | | | |_
 |_|    \__, |\__|_| |_|\___/|_| |_|  \__,_|\__, |\___|_| |_|\__|
        |___/                               |___/
An agent that runs Python3 scripts
Author(s): Paolo Bosetti
*/
#include "../python.hpp"
#include <cppy3/cppy3.hpp>
#include <cppy3/utils.hpp>
#include <cxxopts.hpp>

using namespace std;
using namespace cxxopts;
using namespace Mads;
using json = nlohmann::json;

int main(int argc, char *argv[]) {
  string settings_uri = SETTINGS_URI;
  chrono::milliseconds time{100};
  string agent_name;

  // Parse command line options ================================================
  Options options(argv[0]);
  // if needed, add here further CLI options
  // clang-format off
  options.add_options()
    ("p,period", "Sampling period (default 100 ms)", value<size_t>())
    ("m,module", "Python module to load", value<string>())
    ("n,name", "Agent name (default to 'python')", value<string>())
    ("i,agent-id", "Agent ID to be added to JSON frames", value<string>());
  SETUP_OPTIONS(options, Agent);
  // clang-format on
  if (options_parsed.count("name") != 0) {
    agent_name = options_parsed["name"].as<string>();
  } else {
    agent_name = "python";
  }

  // Initialize agent ==========================================================
  Agent agent(agent_name, settings_uri);
  try {
    agent.init();
  } catch (const std::exception &e) {
    std::cout << fg::red << "Error initializing agent: " << e.what()
              << fg::reset << endl;
    exit(EXIT_FAILURE);
  }
  agent.enable_remote_control();
  agent.connect();
  agent.register_event(event_type::startup);

  json settings = agent.get_settings();

  if (!agent.attachment_path().empty()) {
    auto dir = agent.attachment_path().parent_path();
    settings["search_paths"].push_back(dir.string());
    settings["python_module"] = agent.attachment_path().stem().string();
  }

  // CLI options overrides =====================================================
  if (options_parsed.count("module") != 0) {
    settings["python_module"] = options_parsed["module"].as<string>();
  }
  if (settings["python_module"].empty()) {
    cerr << fg::red << "Python module not specified in settings or command line"
         << fg::reset << endl;
    exit(EXIT_FAILURE);
  }
  if (!settings["period"].is_null()) {
    time = chrono::milliseconds(settings["period"].get<size_t>());
  }
  if (options_parsed.count("period") != 0) {
    time = chrono::milliseconds(options_parsed["period"].as<size_t>());
  }

  // Print info ================================================================
  agent.info(cerr);
  cerr << "  Loaded module:    " << style::bold
       << settings["python_module"].get<string>() << style::reset << endl;

  // Instantiate interpreter
  PythonInterpreter py(settings, settings["python_module"].get<string>());

  // Main loop =================================================================
  cout << fg::green << "Python process started" << fg::reset << endl;

  // SOURCE
  if (py.agent_type() == "source") {
    string result;
    json out;
    agent.loop([&]() {
      try {
        result =
            cppy3::WideToUTF8(cppy3::eval("mads.get_output()").toString());
        out = json::parse(result);
      } catch (cppy3::PythonException &e) {
        cerr << fg::red << "Error running get_output(): " << e.what()
              << fg::reset << endl;
        return;
      } catch (json::parse_error &e) {
        cerr << fg::red << "Error parsing JSON: " << e.what() << fg::reset
              << endl
              << "JSON was: " << result << endl;
        return;
      }
      if (!out.empty()) {
        agent.publish(out);
      }
    }, time);

    // FILTER
  } else if (py.agent_type() == "filter") {
    message_type type;
    auto msg = agent.last_message();
    json out;
    string result;
    string load_topic, load_data;
    agent.loop([&]() {
      try {
        type = agent.receive();
      } catch (const AgentError &e) {
        cerr << fg::red << "Error receiving message: " << e.what() << fg::reset
             << endl;
        return;
      }
      msg = agent.last_message();
      // agent.remote_control();
      if (type == message_type::json && agent.last_topic() != "control") {
        try {
          cppy3::exec("mads.topic = '" + agent.last_topic() + "'");
          cppy3::exec("mads.data = json.loads('" + get<1>(msg) + "')");
        } catch (cppy3::PythonException &e) {
          cerr << fg::red << "Error loading data: " << e.what() << fg::reset
               << endl;
          return;
        }
        try {
          result = cppy3::WideToUTF8(cppy3::eval("mads.process()").toString());
          out = json::parse(result);
        } catch (cppy3::PythonException &e) {
          cerr << fg::red << "Error running process(): " << e.what()
               << fg::reset << endl;
          return;
        } catch (json::parse_error &e) {
          cerr << fg::red << "Error parsing JSON: " << e.what() << fg::reset
               << endl
               << "JSON was: " << result << endl;
          return;
        }
        if (!out.empty()) {
          agent.publish(out);
        }
      }
    });

    // SINK
  } else if (py.agent_type() == "sink") {
    message_type type;
    auto msg = agent.last_message();
    json in;
    agent.loop([&]() {
      try {
        type = agent.receive();
      } catch (const AgentError &e) {
        cerr << fg::red << "Error receiving message: " << e.what() << fg::reset
             << endl;
        return;
      }
      msg = agent.last_message();
      // agent.remote_control();
      if (type == message_type::json && agent.last_topic() != "control") {
        try {
          cppy3::exec("mads.topic = '" + agent.last_topic() + "'");
          cppy3::exec("mads.data = json.loads('" + get<1>(msg) + "')");
        } catch (cppy3::PythonException &e) {
          cerr << fg::red << "Error loading data: " << e.what() << fg::reset
               << endl;
          return;
        }
        try {
          cppy3::exec("mads.deal_with_data()");
        } catch (cppy3::PythonException &e) {
          cerr << fg::red << "Error running deal_with_data(): " << e.what()
               << fg::reset << endl;
          return;
        }
      }
    });
  }
  cout << fg::green << "Python process stopped" << fg::reset << endl;

  // Cleanup ===================================================================
  agent.register_event(event_type::shutdown);
  agent.disconnect();
  if (agent.restart()) {
    auto cmd = string(MADS_PREFIX) + argv[0];
    cout << "Restarting " << cmd << "..." << endl;
    execvp(cmd.c_str(), argv);
  }
  return 0;
}