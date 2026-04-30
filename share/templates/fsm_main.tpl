/*
  _____ ____  __  __   __  __       _       
 |  ___/ ___||  \/  | |  \/  | __ _(_)_ __  
 | |_  \___ \| |\/| | | |\/| |/ _` | | '_ \ 
 |  _|  ___) | |  | | | |  | | (_| | | | | |
 |_|   |____/|_|  |_| |_|  |_|\__,_|_|_| |_|
                                            
Warning: this is a bare-bones template for a FSM-based agent. It is not meant to be used as-is, but rather to be adapted to the specific needs of the user. 

To compile it with CMake, use the following CMakeLists.txt:
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find the installed MADS package
find_package(Mads REQUIRED)
message(STATUS "Found Mads: ${Mads_DIR}")

add_executable(fsm path/to/main.cpp)
target_link_libraries(fsm PRIVATE Mads::Mads)
*/
#include <mads.hpp>
#include <agent.hpp>
#include <agent_app.hpp>
#include <thread>
#include <chrono>
#include <filesystem>
#include <nlohmann/json.hpp>
#include "fsm.hpp"

using namespace chrono_literals;
using json = nlohmann::json;

struct FsmData {
  std::unique_ptr<Mads::AgentApp> agent;
};

int main(int argc, char *argv[]) {
  std::filesystem::path exec = argv[0];
  std::string agent_name = exec.stem().string();
  std::string settings_path = "tcp://localhost:9092";
  std::chrono::duration loop_period = 100ms;
  std::chrono::duration receive_timeout = 50ms;
  bool non_blocking = false;

  if (argc > 1) {
    settings_path = argv[1];
  }
  FsmData data = {std::make_unique<Mads::AgentApp>(agent_name, settings_path)};
  // If crypto is needed, properly load keys and enable it
  data.agent->add_common_options();
  data.agent->options()
    ("my-option", "An additional option flag");
 
  auto parsed = data.agent->parse_options(argc, argv);
  if (int rc = Mads::AgentApp::handle_standard_exit_options<Mads::AgentApp>(
          parsed, data.agent->raw_options(), argv);
      rc >= 0) {
    return rc;
  }
 
  data.agent->init(parsed);
  data.agent->enable_events();
  data.agent->connect();
  data.agent->enable_remote_control();
  data.agent->info();

  auto settings = data.agent->get_settings();
  if (settings.contains("period")) {
    loop_period = std::chrono::milliseconds(settings["period"].get<int>());
  }
  if (settings.contains("receive_timeout")) {
    receive_timeout = std::chrono::milliseconds(settings["receive_timeout"].get<int>());
  }
  if (settings.contains("non_blocking")) {
    non_blocking = settings["non_blocking"].get<bool>();
  }
  data.agent->set_receive_timeout(receive_timeout);
  // Deal with further settings as needed

  // Initialize FSM
  auto fsm = {{ namespace }}::FiniteStateMachine(&data);
  fsm.set_timing_function([&]() {
    std::this_thread::sleep_for(loop_period);
  });
  fsm.run([&](FsmData &s) {
    // here put everything that shall run at each loop iteration
    data.agent->receive(non_blocking);
    data.agent->remote_control(get<1>(data.agent->last_message()));
  });

  // Shutdown procedure
  data.agent->disconnect();
  data.agent->restart_if_requested(argv);
}