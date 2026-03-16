/*
  MADS Smoke Test: C++ Custom Agent
  Validates that an external project can compile and link against Mads::Mads,
  create an agent, connect to the broker, publish messages, and disconnect.
  
  Usage: test_cpp_agent <settings_uri>
*/
#include <agent.hpp>
#include <mads.hpp>
#include <nlohmann/json.hpp>
#include <iostream>
#include <string>
#include <chrono>
#include <thread>

using namespace std;
using namespace Mads;
using json = nlohmann::json;

int main(int argc, char *argv[]) {
  string settings_uri = "tcp://localhost:19092";
  if (argc > 1) {
    settings_uri = argv[1];
  }

  try {
    // Create and initialize agent
    Agent agent("smoke_cpp", settings_uri);
    agent.set_settings_timeout(5000);
    // install_watchdog=false: the watchdog spawns a detached thread that can
    // crash on Windows/MSVC during process exit (CRT terminates detached threads
    // via ExitProcess, causing undefined behavior). Watchdog is only needed for
    // long-running agents using Agent::loop(), not short-lived test processes.
    agent.init(false, false);
    cout << "Agent created and initialized: " << agent.name() << endl;

    // Verify settings were loaded
    auto settings = agent.get_settings();
    cout << "Settings loaded: " << settings.dump() << endl;

    // Connect to broker
    agent.connect();
    cout << "Agent connected" << endl;

    // Publish 5 JSON messages
    for (int i = 0; i < 5; i++) {
      json msg = {
        {"source", "smoke_test_cpp"},
        {"sequence", i}
      };
      agent.publish(msg);
      cout << "Published message " << i << endl;
      this_thread::sleep_for(chrono::milliseconds(50));
    }

    // Disconnect
    agent.disconnect();
    cout << "Agent disconnected" << endl;
    cout << "PASS: C++ custom agent test completed successfully" << endl;
    return 0;
  } catch (const exception &e) {
    cerr << "FAIL: " << e.what() << endl;
    return 1;
  }
}
