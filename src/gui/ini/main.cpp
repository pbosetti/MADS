#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include "ini_app.hpp"

#include "ini_cli.hpp"

#include <filesystem>
#include <optional>

namespace {

int run_gui(std::optional<std::filesystem::path> file) {
  return MadsIni::IniApp(file).run();
}

} // namespace

int main(int argc, char **argv) {
  return Mads::Ini::run_cli(argc, argv, run_gui);
}
