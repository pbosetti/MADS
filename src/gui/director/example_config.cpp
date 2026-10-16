#include "example_config.hpp"

#include <fstream>
#include <sstream>
#include <system_error>

std::string example_config_text() {
  std::ostringstream out;
  out
      << "# director example configuration\n"
      << "# Notes:\n"
      << "# - [director] is optional global settings.\n"
      << "# - Each top-level section name is the process name (mandatory).\n"
      << "# - All options are shown below with defaults and purpose.\n\n"
      << "[director] # optional: global director settings\n"
      << "terminal = \"\" # optional, default=\"\": terminal executable for detached attach windows\n"
      << "sample_rate = 2.0 # optional, default=2.0: process metrics sampling period in seconds (fractions allowed)\n\n"
#ifdef MADS_DIRECTOR_EXAMPLE_CONFIG
      << "[broker] # process section name (mandatory)\n"
      << "command = \"mads broker\" # mandatory: command line to execute\n"
      << "# command supports variables: ${PWD}=resolved workdir, ${ID}=instance index (0-based)\n"
      << "after = \"\" # optional, default=\"\": start after this process name\n"
      << "workdir = \"\" # optional, default=\"\": working directory; default and relative paths resolve against the director.toml directory\n"
      << "enabled = true # optional, default=true: when false, skip auto-start (manual start still allowed)\n"
      << "scale = 1 # optional, default=1: number of instances to launch\n"
      << "relaunch = true # optional, default=false: restart on non-zero exit status\n"
      << "tty = true # optional, default=false: enable TTY/attach interaction\n"
      << "# if workdir/.venv exists, director exposes it to launched Python commands\n\n"

      << "[mongo]\n"
      << "command = \"docker run --rm -t -v${PWD}/db:/data/db mongo\"\n"
      << "after = \"broker\"\n"
      << "workdir = \"\"\n"
      << "enabled = false\n"
      << "scale = 1\n"
      << "relaunch = true\n"
      << "tty = false\n\n"

      << "[logger]\n"
      << "command = \"mads logger\"\n"
      << "after = \"broker\"\n"
      << "workdir = \"\"\n"
      << "enabled = false\n"
      << "scale = 1\n"
      << "relaunch = true\n"
      << "tty = false\n\n"

      << "[feedback]\n"
      << "command = \"mads feedback\"\n"
      << "after = \"broker\"\n"
      << "workdir = \"\"\n"
      << "enabled = true\n"
      << "scale = 1\n"
      << "relaunch = false\n"
      << "tty = false\n\n"

      << "[perf_assess]\n"
      << "command = \"mads perf_assess -p10 -i${ID}\"\n"
      << "after = \"broker\"\n"
      << "workdir = \"\"\n"
      << "enabled = false\n"
      << "scale = 2\n"
      << "relaunch = false\n"
      << "tty = false\n\n";
#else
      << "[db] # process section name (mandatory)\n"
      << "command = \"./bin/db --port 5432 --wd=${PWD} --id=${ID}\" # mandatory: command line to execute\n"
      << "# command supports variables: ${PWD}=resolved workdir, ${ID}=instance index (0-based)\n"
      << "after = \"\" # optional, default=\"\": start after this process name\n"
      << "workdir = \"\" # optional, default=\"\": working directory; default and relative paths resolve against the director.toml directory\n"
      << "enabled = true # optional, default=true: when false, skip auto-start (manual start still allowed)\n"
      << "scale = 1 # optional, default=1: number of instances to launch\n"
      << "relaunch = false # optional, default=false: restart on non-zero exit status\n"
      << "tty = false # optional, default=false: enable TTY/attach interaction\n"
      << "# if workdir/.venv exists, director exposes it to launched Python commands\n\n"

      << "[api] # process section name (mandatory)\n"
      << "command = \"./bin/api_server --port 8080 --wd=${PWD} --id=${ID}\" # mandatory: command line to execute\n"
      << "# command supports variables: ${PWD}=resolved workdir, ${ID}=instance index (0-based)\n"
      << "after = \"db\" # optional, default=\"\": dependency by process name\n"
      << "workdir = \"services/api\" # optional, default=\"\": working directory (relative to the director.toml directory)\n"
      << "enabled = true # optional, default=true: when false, skip auto-start (manual start still allowed)\n"
      << "scale = 1 # optional, default=1: number of instances to launch\n"
      << "relaunch = false # optional, default=false: restart on non-zero exit status\n"
      << "tty = false # optional, default=false: enable TTY/attach interaction\n";
#endif
  return out.str();
}

bool write_example_config(const std::filesystem::path& path, bool overwrite, std::string* out_error) {
  std::error_code ec;
  if (!overwrite && std::filesystem::exists(path, ec)) {
    if (out_error != nullptr) {
      *out_error = "File already exists: " + path.string();
    }
    return false;
  }

  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    if (out_error != nullptr) {
      *out_error = "Cannot open for writing: " + path.string();
    }
    return false;
  }
  const std::string text = example_config_text();
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
  file.close();
  if (!file) {
    if (out_error != nullptr) {
      *out_error = "Failed to write: " + path.string();
    }
    return false;
  }
  return true;
}
