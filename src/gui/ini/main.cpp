#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "ini_app.hpp"

#include "ini_cli.hpp"
#include "exec_path.hpp"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

namespace {

char **Argv = nullptr; // for the relaunch

// Moves the GUI off the launching terminal. Returns true in the process that
// must exit right away (the launcher), false in the one that runs the GUI.
bool detach() {
  // The relaunched process carries this marker and runs the GUI itself.
  if (std::getenv("MADS_INI_DETACHED") != nullptr) {
    return false;
  }
#if defined(_WIN32)
  SetEnvironmentVariableW(L"MADS_INI_DETACHED", L"1");
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::wstring cmd = GetCommandLineW();
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                      DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr,
                      nullptr, &si, &pi)) {
    return false; // run in the foreground instead
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
#else
  // fork + exec, not a bare fork: macOS frameworks break in a forked child.
  const std::string self = Mads::exec_path().string();
  const pid_t pid = fork();
  if (pid < 0) {
    return false; // run in the foreground instead
  }
  if (pid > 0) {
    return true;
  }
  setsid();
  // Keep stderr: a GUI that cannot start (no display) still says why.
  const int null_fd = open("/dev/null", O_RDWR);
  if (null_fd >= 0) {
    dup2(null_fd, STDIN_FILENO);
    dup2(null_fd, STDOUT_FILENO);
  }
  setenv("MADS_INI_DETACHED", "1", 1);
  execv(self.c_str(), Argv);
  _exit(127);
#endif
}

int run_gui(std::optional<std::filesystem::path> file) {
  if (detach()) {
    return 0;
  }
  return MadsIni::IniApp(file).run();
}

} // namespace

int main(int argc, char **argv) {
  Argv = argv;
  return Mads::Ini::run_cli(argc, argv, run_gui);
}
