#include "platform_process.hpp"

#ifdef _WIN32

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <string>
#include <tlhelp32.h>
#include <vector>

namespace {

using CreatePseudoConsoleFn = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
using ClosePseudoConsoleFn = void(WINAPI*)(HPCON);

bool resolve_conpty_api(CreatePseudoConsoleFn* out_create, ClosePseudoConsoleFn* out_close) {
  const HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
  if (kernel32 == nullptr) {
    return false;
  }

  const auto create_fn = reinterpret_cast<CreatePseudoConsoleFn>(GetProcAddress(kernel32, "CreatePseudoConsole"));
  const auto close_fn = reinterpret_cast<ClosePseudoConsoleFn>(GetProcAddress(kernel32, "ClosePseudoConsole"));
  if (create_fn == nullptr || close_fn == nullptr) {
    return false;
  }

  *out_create = create_fn;
  *out_close = close_fn;
  return true;
}

std::string normalize_command_for_cmd(const std::string& command) {
  std::size_t index = 0;
  while (index < command.size() && std::isspace(static_cast<unsigned char>(command[index])) != 0) {
    ++index;
  }
  if (index >= command.size()) {
    return command;
  }

  const bool quoted = command[index] == '"';
  std::size_t token_start = quoted ? index + 1 : index;
  std::size_t token_end = token_start;
  if (quoted) {
    token_end = command.find('"', token_start);
    if (token_end == std::string::npos) {
      return command;
    }
  } else {
    while (token_end < command.size() &&
           std::isspace(static_cast<unsigned char>(command[token_end])) == 0) {
      ++token_end;
    }
  }

  std::string token = command.substr(token_start, token_end - token_start);
  if (token.empty()) {
    return command;
  }

  // Allow Windows-style separators even when TOML uses POSIX slashes.
  if (token.find('/') == std::string::npos) {
    return command;
  }
  std::replace(token.begin(), token.end(), '/', '\\');

  std::string normalized = command;
  normalized.replace(token_start, token_end - token_start, token);
  return normalized;
}

DWORD find_direct_child_pid(DWORD parent_pid) {
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return 0;
  }

  PROCESSENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  if (!Process32First(snapshot, &entry)) {
    CloseHandle(snapshot);
    return 0;
  }

  DWORD child_pid = 0;
  do {
    if (entry.th32ParentProcessID != parent_pid) {
      continue;
    }
    // Keep the latest pid in case multiple direct children are present.
    if (entry.th32ProcessID > child_pid) {
      child_pid = entry.th32ProcessID;
    }
  } while (Process32Next(snapshot, &entry));

  CloseHandle(snapshot);
  return child_pid;
}

class WindowsProcess final : public PlatformProcess {
 public:
  ~WindowsProcess() override {
    stop();
    close_handles();
  }

  bool start(const std::string& command, const std::string& workdir, bool tty, std::string* out_error) override {
    stop();
    close_handles();

    _tty = tty;
    _exit_code = 0;

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = nullptr;
    attributes.bInheritHandle = TRUE;

    _job = CreateJobObjectA(nullptr, nullptr);
    if (_job != nullptr) {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
      limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      if (!SetInformationJobObject(_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        CloseHandle(_job);
        _job = nullptr;
      }
    }

    if (_tty) {
      if (!resolve_conpty_api(&_create_pseudo_console, &_close_pseudo_console)) {
        *out_error = "Windows ConPTY API is unavailable on this system.";
        return false;
      }
      if (!CreatePipe(&_pty_in_read, &_pty_in_write, &attributes, 0)) {
        *out_error = "CreatePipe for ConPTY input failed.";
        return false;
      }
      if (!SetHandleInformation(_pty_in_write, HANDLE_FLAG_INHERIT, 0)) {
        *out_error = "SetHandleInformation for ConPTY input failed.";
        return false;
      }

      if (!CreatePipe(&_pty_out_read, &_pty_out_write, &attributes, 0)) {
        *out_error = "CreatePipe for ConPTY output failed.";
        return false;
      }
      if (!SetHandleInformation(_pty_out_read, HANDLE_FLAG_INHERIT, 0)) {
        *out_error = "SetHandleInformation for ConPTY output failed.";
        return false;
      }

      const COORD size = {120, 40};
      const HRESULT hr = _create_pseudo_console(size, _pty_in_read, _pty_out_write, 0, &_pseudo_console);
      if (FAILED(hr)) {
        *out_error = "CreatePseudoConsole failed.";
        return false;
      }

      close_if_open(&_pty_in_read);
      close_if_open(&_pty_out_write);
    } else {
      if (!CreatePipe(&_stdout_read, &_stdout_write, &attributes, 0)) {
        *out_error = "CreatePipe for stdout failed.";
        return false;
      }
      if (!SetHandleInformation(_stdout_read, HANDLE_FLAG_INHERIT, 0)) {
        *out_error = "SetHandleInformation for stdout failed.";
        return false;
      }

      if (!CreatePipe(&_stdin_read, &_stdin_write, &attributes, 0)) {
        *out_error = "CreatePipe for stdin failed.";
        return false;
      }
      if (!SetHandleInformation(_stdin_write, HANDLE_FLAG_INHERIT, 0)) {
        *out_error = "SetHandleInformation for stdin failed.";
        return false;
      }
    }

    STARTUPINFOA startup{};
    STARTUPINFOEXA startup_ex{};
    startup.cb = sizeof(startup);
    startup_ex.StartupInfo.cb = sizeof(startup_ex);

    std::vector<unsigned char> attribute_buffer;
    bool use_startup_ex = false;
    DWORD creation_flags = 0;
    BOOL inherit_handles = TRUE;

    if (_tty) {
      SIZE_T attribute_size = 0;
      InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
      attribute_buffer.resize(attribute_size);
      startup_ex.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attribute_buffer.data());

      if (!InitializeProcThreadAttributeList(startup_ex.lpAttributeList, 1, 0, &attribute_size)) {
        *out_error = "InitializeProcThreadAttributeList failed.";
        return false;
      }

      if (!UpdateProcThreadAttribute(startup_ex.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                     _pseudo_console, sizeof(_pseudo_console), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(startup_ex.lpAttributeList);
        *out_error = "UpdateProcThreadAttribute for pseudoconsole failed.";
        return false;
      }

      use_startup_ex = true;
      creation_flags = EXTENDED_STARTUPINFO_PRESENT;
      inherit_handles = FALSE;
    } else {
      startup.dwFlags |= STARTF_USESTDHANDLES;
      startup.hStdInput = _stdin_read;
      startup.hStdOutput = _stdout_write;
      startup.hStdError = _stdout_write;
    }

    PROCESS_INFORMATION info{};
    std::string run_command = normalize_command_for_cmd(command);

    if (!workdir.empty()) {
      std::error_code ec;
      const std::filesystem::path venv_activate =
          std::filesystem::path(workdir) / ".venv" / "Scripts" / "activate.bat";
      if (std::filesystem::exists(venv_activate, ec)) {
        run_command = "call \"" + venv_activate.string() + "\" && " + run_command;
      }
    }

    std::string cmdline = "cmd.exe /S /C \"" + run_command + "\"";

    const char* current_dir = workdir.empty() ? nullptr : workdir.c_str();
    const BOOL started = use_startup_ex
                             ? CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, inherit_handles,
                                              creation_flags, nullptr, current_dir, &startup_ex.StartupInfo, &info)
                             : CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, inherit_handles,
                                              creation_flags, nullptr, current_dir, &startup, &info);

    if (use_startup_ex) {
      DeleteProcThreadAttributeList(startup_ex.lpAttributeList);
    }

    if (!started) {
      *out_error = "CreateProcess failed.";
      return false;
    }

    _process = info.hProcess;
    _thread = info.hThread;
    _process_id = info.dwProcessId;

    if (_job != nullptr) {
      (void)AssignProcessToJobObject(_job, _process);
    }

    if (!_tty) {
      CloseHandle(_stdout_write);
      _stdout_write = nullptr;
      CloseHandle(_stdin_read);
      _stdin_read = nullptr;
    }

    _running = true;
    return true;
  }

  void stop() override {
    if (!_running || _process == nullptr) {
      return;
    }

    if (_tty) {
      close_if_open(&_pty_in_write);
    } else {
      close_if_open(&_stdin_write);
    }

    if (_job != nullptr) {
      (void)TerminateJobObject(_job, 1);
    } else {
      TerminateProcess(_process, 1);
    }
    WaitForSingleObject(_process, 2000);

    DWORD code = 0;
    if (GetExitCodeProcess(_process, &code)) {
      _exit_code = static_cast<int>(code);
    }

    _running = false;
    _process_id = 0;
    close_handles();
  }

  bool is_running() override {
    if (!_running || _process == nullptr) {
      return false;
    }

    DWORD code = 0;
    if (!GetExitCodeProcess(_process, &code)) {
      _running = false;
      _process_id = 0;
      return false;
    }

    if (code == STILL_ACTIVE) {
      return true;
    }

    _exit_code = static_cast<int>(code);
    _running = false;
    _process_id = 0;
    close_handles();
    return false;
  }

  int exit_code() const override {
    return _exit_code;
  }

  int process_id() const override {
    if (_running && _process_id != 0) {
      const DWORD child_pid = find_direct_child_pid(_process_id);
      if (child_pid != 0) {
        return static_cast<int>(child_pid);
      }
      return static_cast<int>(_process_id);
    }
    return -1;
  }

  bool read_available(std::string* out_chunk) override {
    const HANDLE read_handle = _tty ? _pty_out_read : _stdout_read;
    if (read_handle == nullptr) {
      return false;
    }

    DWORD available = 0;
    if (!PeekNamedPipe(read_handle, nullptr, 0, nullptr, &available, nullptr) || available == 0) {
      return false;
    }

    const DWORD read_size = available > 4096 ? 4096 : available;
    char buffer[4096];
    DWORD bytes_read = 0;

    if (!ReadFile(read_handle, buffer, read_size, &bytes_read, nullptr) || bytes_read == 0) {
      return false;
    }

    out_chunk->assign(buffer, buffer + bytes_read);
    return true;
  }

  bool write_input(const std::string& data) override {
    const HANDLE write_handle = _tty ? _pty_in_write : _stdin_write;
    if (write_handle == nullptr) {
      return false;
    }

    DWORD written = 0;
    if (!WriteFile(write_handle, data.data(), static_cast<DWORD>(data.size()), &written, nullptr)) {
      return false;
    }

    return written == data.size();
  }

  bool attach_until_detach(std::string* out_error) override {
    *out_error = "Interactive attach is currently implemented only on POSIX terminals.";
    return false;
  }

 private:
  void close_if_open(HANDLE* handle) {
    if (*handle != nullptr) {
      CloseHandle(*handle);
      *handle = nullptr;
    }
  }

  void close_handles() {
    close_if_open(&_pty_in_read);
    close_if_open(&_pty_in_write);
    close_if_open(&_pty_out_read);
    close_if_open(&_pty_out_write);
    close_if_open(&_stdin_read);
    close_if_open(&_stdin_write);
    close_if_open(&_stdout_read);
    close_if_open(&_stdout_write);
    close_if_open(&_process);
    close_if_open(&_thread);
    close_if_open(&_job);
    if (_pseudo_console != nullptr && _close_pseudo_console != nullptr) {
      _close_pseudo_console(_pseudo_console);
      _pseudo_console = nullptr;
    }
    _create_pseudo_console = nullptr;
    _close_pseudo_console = nullptr;
  }

  HANDLE _pty_in_read = nullptr;
  HANDLE _pty_in_write = nullptr;
  HANDLE _pty_out_read = nullptr;
  HANDLE _pty_out_write = nullptr;
  HANDLE _stdin_read = nullptr;
  HANDLE _stdin_write = nullptr;
  HANDLE _stdout_read = nullptr;
  HANDLE _stdout_write = nullptr;
  HANDLE _process = nullptr;
  HANDLE _thread = nullptr;
  HANDLE _job = nullptr;
  HPCON _pseudo_console = nullptr;
  CreatePseudoConsoleFn _create_pseudo_console = nullptr;
  ClosePseudoConsoleFn _close_pseudo_console = nullptr;
  DWORD _process_id = 0;

  int _exit_code = 0;
  bool _running = false;
  bool _tty = false;
};

}  // namespace

std::unique_ptr<PlatformProcess> create_platform_process() {
  return std::make_unique<WindowsProcess>();
}

#endif
