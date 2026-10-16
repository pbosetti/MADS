#include "attach_client.hpp"
#include "director_config.hpp"
#include "example_config.hpp"
#include "gui_app.hpp"
#include "mads.hpp"
#include "process_manager.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#ifndef DIRECTOR_CMD_PREFIX
#define DIRECTOR_CMD_PREFIX ""
#endif

namespace {

void print_usage(const std::string& program) {
  std::cout << "Usage: " << program << " [options] [config.toml]\n"
            << "MADS process director (GUI) for director.toml files.\n\n"
            << "Options:\n"
            << "  -c, --config <file>      director.toml to load (default: director.toml)\n"
            << "  -t, --template [file]    write a director.toml template (default: director.toml) and exit\n"
            << "  -v, --version            print version and exit\n"
            << "  -h, --help               print this help and exit\n"
            << "      --example            print the template to stdout and exit\n"
            << "If the config file does not exist, the GUI opens with an empty process list\n"
            << "and a \"Create template\" button.\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path = "director.toml";
  std::string attach_socket_path;
  std::string executable_path = argc > 0 ? argv[0] : "director";
  const std::string program =
      argc > 0 ? std::filesystem::path(argv[0]).filename().string() : std::string("mads-director");
  bool show_help = false;
  bool show_version = false;
  bool force_show_example = false;
  bool write_template = false;
  std::string template_path = "director.toml";
  if (!executable_path.empty()) {
    const std::filesystem::path executable_input(executable_path);
    // Keep bare command names (e.g. "mads-director") unchanged so external
    // attach shells resolve them through PATH. Only normalize explicit paths.
    if (executable_input.has_parent_path()) {
      std::error_code ec;
      const auto absolute = std::filesystem::absolute(executable_input, ec);
      if (!ec) {
        executable_path = absolute.string();
      }
    } else {
      const std::string cmd_prefix = DIRECTOR_CMD_PREFIX;
      if (!cmd_prefix.empty()) {
        const std::string required_prefix = cmd_prefix + "-";
        if (!executable_path.starts_with(required_prefix)) {
          executable_path = required_prefix + executable_path;
        }
      }
    }
  }

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      show_help = true;
      continue;
    }
    if (arg == "-v" || arg == "--version") {
      show_version = true;
      continue;
    }
    if (arg == "-t" || arg == "--template") {
      write_template = true;
      if (i + 1 < argc && !std::string(argv[i + 1]).starts_with('-')) {
        template_path = argv[++i];
      } else {
        template_path = "director.toml";
      }
      continue;
    }
    if (arg == "--example" || arg == "--example-config") {
      force_show_example = true;
      continue;
    }
    if (arg == "--attach-socket" && i + 1 < argc) {
      attach_socket_path = argv[++i];
      continue;
    }
    if ((arg == "-c" || arg == "--config") && i + 1 < argc) {
      config_path = argv[++i];
      continue;
    }
    config_path = arg;
  }

  if (show_help) {
    print_usage(program);
    return 0;
  }

  if (show_version) {
    std::cout << "mads-director " << LIB_VERSION << " (" << LIB_GIT_TAG << ")\n";
    return 0;
  }

  if (!attach_socket_path.empty()) {
    std::string error;
    const int code = run_attach_client(attach_socket_path, &error);
    if (code != 0 && !error.empty()) {
      std::cerr << "Attach client failed: " << error << "\n";
    }
    return code;
  }

  if (force_show_example) {
    std::cout << example_config_text();
    return 0;
  }

  if (write_template) {
    std::string error;
    if (!write_example_config(template_path, /*overwrite=*/false, &error)) {
      std::cerr << error << "\n";
      return 1;
    }
    std::cout << "Template written to " << template_path << "\n";
    return 0;
  }

  Mads::DirectorConfig config;
  std::string initial_status;
  std::string error;
  std::error_code ec;
  if (!std::filesystem::exists(config_path, ec)) {
    std::cerr << "No config file '" << config_path
              << "': opening an empty director (use \"Create template\" or --template).\n";
    config.base_dir = std::filesystem::current_path(ec);
    initial_status = "No config file '" + config_path + "'. Use \"Create template\" to write one.";
  } else {
    std::vector<std::string> warnings;
    auto loaded = Mads::load_director_config(config_path, &error, &warnings);
    for (const auto& warning : warnings) {
      std::cerr << "Warning: " << warning << "\n";
    }
    if (!loaded.has_value()) {
      std::cerr << "Failed to load config '" << config_path << "': " << error << "\n"
                << "Run with --template to write a valid example.\n";
      return 1;
    }
    config = std::move(*loaded);
  }

  ProcessManager manager;
  if (!manager.initialize(config, &error)) {
    std::cerr << "Failed to initialize process manager: " << error << "\n";
    return 1;
  }

  manager.launch_ready_processes();

  GuiApp gui;
  return gui.run(&manager, executable_path, config_path, initial_status);
}
