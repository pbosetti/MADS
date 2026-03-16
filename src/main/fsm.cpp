#include <iostream>
#include <gv2fsm.hpp>
#include <generator.hpp>
#include "../exec_path.hpp"
#include "../mads.hpp"

int main(int argc, const char *argv[]) {
  auto template_dir = Mads::exec_dir("../share/templates/");
  std::string error_msg;
  bool template_loaded = set_main_template(template_dir + "/fsm_main.tpl", &error_msg);
  if (!template_loaded) {
    std::cerr << "Error loading template: " << error_msg << std::endl;
    return 1;
  }
  gv2fsm::run(argc, const_cast<char **>(argv));
  return 0;
}