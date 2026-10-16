#pragma once

#include <string>

class ProcessManager;

class GuiApp {
 public:
  int run(ProcessManager* manager, const std::string& executable_path, std::string config_path,
          const std::string& initial_status = "");
};
