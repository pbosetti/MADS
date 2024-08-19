#include "src/watcher.hpp"


int main(int argc, char *argv[]) {
  std::cout << "Watching " << argv[1] << " for changes" << std::endl;
  Mads::Watcher watcher(argv[1], 100ms);
  bool running = true;
  watcher.watch(&running, [](const std::string &file_name) {
    std::cout << file_name << " has been modified" << std::endl;
  });
  return 0;
}
