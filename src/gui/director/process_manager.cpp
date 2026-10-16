#include "process_manager.hpp"

#include "attach_launcher.hpp"
#include "pm.hpp"
#include "platform_process.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cerrno>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <memory>
#include <sstream>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace {

constexpr std::size_t kMaxLogLines = 600;
constexpr std::size_t kCpuHistorySamples = 48;

struct ProcessDefinition {
  std::string name;
  std::string base_name;
  std::string command;
  std::string workdir;
  bool enabled = true;
  bool relaunch = false;
  bool tty = false;
  std::vector<std::string> dependencies;
};

// The MadsCore parser (src/director_config.hpp) has already expanded `scale`,
// substituted ${PWD}/${ID}, resolved workdirs and dependency instance names,
// and rejected dependency cycles: this only maps its output.
std::vector<ProcessDefinition> build_process_definitions(const Mads::DirectorConfig& config) {
  std::vector<ProcessDefinition> definitions;
  definitions.reserve(config.processes.size());
  for (const auto& process : config.processes) {
    ProcessDefinition definition;
    definition.name = process.name;
    definition.base_name = process.base_name;
    definition.command = process.command;
    definition.workdir = process.workdir;
    definition.enabled = process.enabled;
    definition.relaunch = process.relaunch;
    definition.tty = process.tty;
    definition.dependencies = process.after;
    definitions.push_back(std::move(definition));
  }
  return definitions;
}

#ifndef _WIN32
ssize_t send_without_sigpipe(int fd, const char* data, std::size_t size) {
#if defined(MSG_NOSIGNAL)
  return send(fd, data, size, MSG_NOSIGNAL);
#else
  return write(fd, data, size);
#endif
}
#endif

}  // namespace

struct ProcessManager::ManagedProcess {
  ProcessRuntimeView view;
  std::string command;
  std::string workdir;
  std::unique_ptr<PlatformProcess> process;
  std::unique_ptr<mads::process_metrics> metrics;
  bool scheduler_start_allowed = true;
  bool launched_by_scheduler = false;
  bool externally_attached = false;
  std::string pending_line;
  std::size_t pending_cursor = std::string::npos;
};

ProcessManager::ProcessManager() = default;

ProcessManager::~ProcessManager() {
  stop_all();
}

bool ProcessManager::initialize(const Mads::DirectorConfig& config, std::string* out_error) {
  (void)out_error;
  stop_all();
  _processes.clear();
  _terminal_command.reset();
  _sample_rate_seconds = config.sample_rate_seconds;
  _last_sample_tick_ms = 0;
  if (config.terminal.has_value() && !config.terminal->empty()) {
    _terminal_command = config.terminal;
  }
  const std::vector<ProcessDefinition> definitions = build_process_definitions(config);

  for (const auto& definition : definitions) {
    ManagedProcess managed;
    managed.view.name = definition.name;
    managed.view.base_name = definition.base_name;
    managed.view.enabled = definition.enabled;
    managed.view.relaunch = definition.relaunch;
    managed.view.tty = definition.tty;
    managed.workdir = definition.workdir;
    managed.view.command = definition.command;
    managed.command = definition.command;
    managed.view.dependencies = definition.dependencies;
    managed.process = create_platform_process();
    _processes.push_back(std::move(managed));
  }

  return true;
}

bool ProcessManager::reload(const Mads::DirectorConfig& config, std::string* out_error) {
  (void)out_error;
  const std::vector<ProcessDefinition> definitions = build_process_definitions(config);

  if (_external_attach.active) {
    close_external_attach("Configuration reloaded.");
  }

  _terminal_command.reset();
  if (config.terminal.has_value() && !config.terminal->empty()) {
    _terminal_command = config.terminal;
  }
  _sample_rate_seconds = config.sample_rate_seconds;
  _last_sample_tick_ms = 0;

  std::unordered_map<std::string, std::size_t> existing_by_name;
  existing_by_name.reserve(_processes.size());
  for (std::size_t i = 0; i < _processes.size(); ++i) {
    existing_by_name[_processes[i].view.name] = i;
  }

  std::vector<ManagedProcess> reloaded;
  reloaded.reserve(definitions.size());
  std::vector<bool> retained(_processes.size(), false);

  for (const auto& definition : definitions) {
    const auto existing = existing_by_name.find(definition.name);
    if (existing != existing_by_name.end()) {
      ManagedProcess managed = std::move(_processes[existing->second]);
      retained[existing->second] = true;
      managed.view.base_name = definition.base_name;
      managed.view.enabled = definition.enabled;
      managed.view.relaunch = definition.relaunch;
      managed.view.tty = definition.tty;
      managed.view.command = definition.command;
      managed.command = definition.command;
      managed.workdir = definition.workdir;
      managed.view.dependencies = definition.dependencies;
      managed.scheduler_start_allowed = false;
      reloaded.push_back(std::move(managed));
      continue;
    }

    ManagedProcess managed;
    managed.view.name = definition.name;
    managed.view.base_name = definition.base_name;
    managed.view.enabled = definition.enabled;
    managed.view.relaunch = definition.relaunch;
    managed.view.tty = definition.tty;
    managed.view.command = definition.command;
    managed.command = definition.command;
    managed.workdir = definition.workdir;
    managed.view.dependencies = definition.dependencies;
    managed.process = create_platform_process();
    managed.scheduler_start_allowed = false;
    reloaded.push_back(std::move(managed));
  }

  for (std::size_t i = 0; i < _processes.size(); ++i) {
    if (retained[i]) {
      continue;
    }
    auto& process = _processes[i];
    if (process.view.running) {
      process.process->stop();
    }
  }

  _processes = std::move(reloaded);
  return true;
}

void ProcessManager::launch_ready_processes() {
  std::string ignored_error;
  for (std::size_t i = 0; i < _processes.size(); ++i) {
    auto& process = _processes[i];
    if (!process.view.enabled) {
      continue;
    }
    if (!process.scheduler_start_allowed) {
      continue;
    }
    if (process.view.running || process.view.ever_started) {
      continue;
    }
    if (!can_start(process)) {
      continue;
    }

    if (start_process_internal(i, &ignored_error)) {
      process.launched_by_scheduler = true;
    }
  }
}

void ProcessManager::tick() {
  poll_external_attach();

  for (std::size_t i = 0; i < _processes.size(); ++i) {
    auto& process = _processes[i];
    if (!process.externally_attached) {
      capture_output(&process);
    }

    if (!process.view.running) {
      continue;
    }

    if (process.process->is_running()) {
      process.view.pid = process.process->process_id();
      continue;
    }

    process.view.running = false;
    process.view.pid = -1;
    process.view.exit_code = process.process->exit_code();
    process.metrics.reset();
    process.view.cpu_percent_per_core = 0.0;
    process.view.thread_count = 0;
    process.view.ram_bytes = 0;

    std::ostringstream line;
    line << "Process exited with status " << process.view.exit_code << ".";
    append_log_line(&process, line.str());

    if (process.view.relaunch && process.view.exit_code != 0) {
      append_log_line(&process, "Relaunch enabled and exit status non-zero. Restarting process.");
      std::string error;
      if (!start_process_internal(i, &error)) {
        append_log_line(&process, "Restart failed: " + error);
      }
    }
  }

  sample_metrics_if_due();
}

void ProcessManager::stop_all() {
  close_external_attach("Director shutting down.");

  for (auto& process : _processes) {
    if (!process.view.running) {
      continue;
    }
    process.process->stop();
    process.view.running = false;
    process.view.pid = -1;
    process.metrics.reset();
    process.view.cpu_percent_per_core = 0.0;
    process.view.thread_count = 0;
    process.view.ram_bytes = 0;
  }
}

std::size_t ProcessManager::process_count() const {
  return _processes.size();
}

const ProcessRuntimeView* ProcessManager::process_at(std::size_t index) const {
  if (index >= _processes.size()) {
    return nullptr;
  }
  return &_processes[index].view;
}

bool ProcessManager::start_process(std::size_t index, std::string* out_error) {
  if (index >= _processes.size()) {
    *out_error = "Invalid process index.";
    return false;
  }

  auto& process = _processes[index];
  if (!can_start(process)) {
    *out_error = "Dependencies are not started yet.";
    return false;
  }

  return start_process_internal(index, out_error);
}

bool ProcessManager::stop_process(std::size_t index, std::string* out_error) {
  if (index >= _processes.size()) {
    *out_error = "Invalid process index.";
    return false;
  }

  auto& process = _processes[index];
  if (!process.view.running) {
    *out_error = "Process is not running.";
    return false;
  }

  process.process->stop();
  process.view.running = false;
  process.view.pid = -1;
  append_log_line(&process, "Process stopped by user.");
  return true;
}

bool ProcessManager::restart_process(std::size_t index, std::string* out_error) {
  if (index >= _processes.size()) {
    *out_error = "Invalid process index.";
    return false;
  }

  auto& process = _processes[index];
  if (process.view.running) {
    process.process->stop();
    process.view.running = false;
    process.view.pid = -1;
  }

  return start_process_internal(index, out_error);
}

bool ProcessManager::send_siginfo(std::size_t index, std::string* out_error) {
  if (index >= _processes.size()) {
    *out_error = "Invalid process index.";
    return false;
  }

  auto& process = _processes[index];
  if (!process.view.running || process.view.pid <= 0) {
    *out_error = "Process is not running.";
    return false;
  }

#ifdef _WIN32
  (void)process;
  *out_error = "SIGINFO is not available on Windows.";
  return false;
#else
#ifdef SIGINFO
  if (kill(static_cast<pid_t>(process.view.pid), SIGINFO) != 0) {
    *out_error = std::string("Failed to send SIGINFO: ") + std::strerror(errno);
    return false;
  }

  append_log_line(&process, "SIGINFO sent.");
  return true;
#else
  *out_error = "SIGINFO is not supported on this platform.";
  return false;
#endif
#endif
}

bool ProcessManager::send_input(std::size_t index, const std::string& input, std::string* out_error) {
  if (index >= _processes.size()) {
    *out_error = "Invalid process index.";
    return false;
  }

  auto& process = _processes[index];
  if (!process.view.running) {
    *out_error = "Process is not running.";
    return false;
  }

  if (!process.process->write_input(input)) {
    *out_error = "Failed to send input. This process may not accept stdin in current mode.";
    return false;
  }

  return true;
}

bool ProcessManager::attach_process(std::size_t index, std::string* out_error) {
  if (index >= _processes.size()) {
    *out_error = "Invalid process index.";
    return false;
  }

  auto& process = _processes[index];
  if (!process.view.running) {
    *out_error = "Process is not running.";
    return false;
  }

  if (!process.view.tty) {
    *out_error = "Attach is only available for processes with tty=true.";
    return false;
  }

  append_log_line(&process, "Attach session started.");
  if (!process.process->attach_until_detach(out_error)) {
    append_log_line(&process, "Attach failed: " + *out_error);
    return false;
  }
  append_log_line(&process, "Attach session ended.");
  return true;
}

bool ProcessManager::open_external_attach(std::size_t index, const std::string& executable_path, std::string* out_error) {
#ifdef _WIN32
  if (index >= _processes.size()) {
    *out_error = "Invalid process index.";
    return false;
  }

  if (_external_attach.active) {
    *out_error = "An external attach session is already active.";
    return false;
  }

  auto& process = _processes[index];
  if (!process.view.running) {
    *out_error = "Process is not running.";
    return false;
  }
  if (!process.view.tty) {
    *out_error = "External attach is only available for tty=true processes.";
    return false;
  }

  const std::string pipe_name = "\\\\.\\pipe\\director_attach_" + std::to_string(GetCurrentProcessId()) + "_" +
                                std::to_string(index) + "_" +
                                std::to_string(static_cast<unsigned long long>(time(nullptr)));
  const HANDLE pipe = CreateNamedPipeA(pipe_name.c_str(), PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT, 1, 4096, 4096, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) {
    *out_error = "CreateNamedPipe failed.";
    return false;
  }

  if (!launch_attach_terminal(executable_path, pipe_name, _terminal_command, out_error)) {
    CloseHandle(pipe);
    return false;
  }

  _external_attach.active = true;
  _external_attach.process_index = index;
  _external_attach.listen_handle = reinterpret_cast<std::uintptr_t>(pipe);
  _external_attach.client_handle = 0;
  _external_attach.socket_path = pipe_name;

  append_log_line(&process, "Waiting for external attach client...");
  return true;
#else
  if (index >= _processes.size()) {
    *out_error = "Invalid process index.";
    return false;
  }

  if (_external_attach.active) {
    *out_error = "An external attach session is already active.";
    return false;
  }

  auto& process = _processes[index];
  if (!process.view.running) {
    *out_error = "Process is not running.";
    return false;
  }
  if (!process.view.tty) {
    *out_error = "External attach is only available for tty=true processes.";
    return false;
  }

  const int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    *out_error = std::string("socket failed: ") + std::strerror(errno);
    return false;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  const std::string socket_path = "/tmp/director_attach_" + std::to_string(getpid()) + "_" + std::to_string(index) +
                                  "_" + std::to_string(static_cast<unsigned long long>(time(nullptr))) + ".sock";
  if (socket_path.size() >= sizeof(addr.sun_path)) {
    close(listen_fd);
    *out_error = "Attach socket path is too long.";
    return false;
  }
  std::strcpy(addr.sun_path, socket_path.c_str());
  unlink(socket_path.c_str());

  if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    const std::string error = std::string("bind failed: ") + std::strerror(errno);
    close(listen_fd);
    *out_error = error;
    return false;
  }
  if (listen(listen_fd, 1) != 0) {
    const std::string error = std::string("listen failed: ") + std::strerror(errno);
    close(listen_fd);
    unlink(socket_path.c_str());
    *out_error = error;
    return false;
  }

  const int flags = fcntl(listen_fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);
  }

  if (!launch_attach_terminal(executable_path, socket_path, _terminal_command, out_error)) {
    close(listen_fd);
    unlink(socket_path.c_str());
    return false;
  }

  _external_attach.active = true;
  _external_attach.process_index = index;
  _external_attach.listen_fd = listen_fd;
  _external_attach.client_fd = -1;
  _external_attach.socket_path = socket_path;

  append_log_line(&process, "Waiting for external attach client...");
  return true;
#endif
}

bool ProcessManager::can_start(const ManagedProcess& process) const {
  for (const auto& dep_name : process.view.dependencies) {
    const auto iter = std::find_if(_processes.begin(), _processes.end(),
                                   [&](const ManagedProcess& candidate) { return candidate.view.name == dep_name; });

    if (iter == _processes.end()) {
      return false;
    }

    if (!iter->view.ever_started) {
      return false;
    }
  }

  return true;
}

bool ProcessManager::start_process_internal(std::size_t index, std::string* out_error) {
  auto& process = _processes[index];
  if (process.view.running) {
    return true;
  }

  std::string start_error;
  if (!process.process->start(process.command, process.workdir, process.view.tty, &start_error)) {
    *out_error = "Failed to start '" + process.view.name + "': " + start_error;
    append_log_line(&process, *out_error);
    return false;
  }

  process.view.running = true;
  process.view.ever_started = true;
  process.view.pid = process.process->process_id();
  process.view.exit_code = 0;
  process.view.cpu_percent_per_core = 0.0;
  process.view.thread_count = 0;
  process.view.ram_bytes = 0;
  process.view.cpu_percent_per_core_history.clear();
  if (process.view.pid > 0) {
    process.metrics = std::make_unique<mads::process_metrics>(
        static_cast<mads::process_metrics::pid_type>(process.view.pid));
    process.view.cpu_count = process.metrics->cpu_count();
    process.metrics->sample();
  } else {
    process.metrics.reset();
    process.view.cpu_count = 1;
  }

  append_log_line(&process, "Process started.");
  append_log_line(&process, "Command: " + process.command);
  append_log_line(&process, "Working directory: " + process.workdir);
  if (process.view.tty) {
    append_log_line(&process, "TTY mode enabled. Use GUI attach controls. Detach with Ctrl-] in attached terminal.");
  }

  return true;
}

void ProcessManager::sample_metrics_if_due() {
  if (_processes.empty()) {
    return;
  }

  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const auto now_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
  const auto period_ms = static_cast<std::uint64_t>(_sample_rate_seconds * 1000.0);
  const std::uint64_t clamped_period_ms = std::max<std::uint64_t>(1, period_ms);

  if (_last_sample_tick_ms != 0 && now_ms - _last_sample_tick_ms < clamped_period_ms) {
    return;
  }
  _last_sample_tick_ms = now_ms;

  for (auto& process : _processes) {
    sample_process_metrics(&process);
  }
}

void ProcessManager::sample_process_metrics(ManagedProcess* process) {
  if (!process->view.running || process->view.pid <= 0) {
    process->metrics.reset();
    process->view.cpu_percent_per_core = 0.0;
    process->view.thread_count = 0;
    process->view.ram_bytes = 0;
    if (process->view.cpu_percent_per_core_history.empty() ||
        process->view.cpu_percent_per_core_history.back() != 0.0f) {
      process->view.cpu_percent_per_core_history.push_back(0.0f);
    }
    if (process->view.cpu_percent_per_core_history.size() > kCpuHistorySamples) {
      process->view.cpu_percent_per_core_history.erase(process->view.cpu_percent_per_core_history.begin());
    }
    return;
  }

  const auto pid = static_cast<mads::process_metrics::pid_type>(process->view.pid);
  if (process->metrics == nullptr || process->metrics->pid() != pid) {
    process->metrics = std::make_unique<mads::process_metrics>(pid);
    process->view.cpu_count = process->metrics->cpu_count();
    process->metrics->sample();
  }

  process->metrics->sample();
  process->view.cpu_percent_per_core = process->metrics->cpu_percent_per_core();
  process->view.thread_count = process->metrics->thread_count();
  process->view.ram_bytes = process->metrics->ram_bytes();
  process->view.cpu_count = process->metrics->cpu_count();
  process->view.cpu_percent_per_core_history.push_back(static_cast<float>(process->view.cpu_percent_per_core));
  if (process->view.cpu_percent_per_core_history.size() > kCpuHistorySamples) {
    process->view.cpu_percent_per_core_history.erase(process->view.cpu_percent_per_core_history.begin());
  }
}

void ProcessManager::capture_output(ManagedProcess* process) {
  std::string chunk;
  while (process->process->read_available(&chunk)) {
    append_log(process, chunk);
    chunk.clear();
  }
}

void ProcessManager::append_log(ManagedProcess* process, const std::string& text) {
  auto commit_line = [process]() {
    process->pending_cursor = std::string::npos;
    process->view.logs.push_back(process->pending_line);
    process->pending_line.clear();
    process->view.live_line.clear();
  };

  auto write_char = [process](char c) {
    if (process->pending_cursor == std::string::npos) {
      process->pending_line.push_back(c);
      return;
    }

    if (process->pending_cursor < process->pending_line.size()) {
      process->pending_line[process->pending_cursor] = c;
    } else {
      process->pending_line.push_back(c);
    }
    ++process->pending_cursor;
  };

  auto parse_csi_param = [](const std::string& params, int default_value) {
    if (params.empty()) {
      return default_value;
    }
    const std::size_t last_sep = params.rfind(';');
    const std::string token = params.substr(last_sep == std::string::npos ? 0 : (last_sep + 1));
    if (token.empty()) {
      return default_value;
    }
    return std::atoi(token.c_str());
  };

  std::size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];

    if (c == '\r') {
      process->pending_cursor = 0;
      ++i;
      continue;
    }

    if (c == '\n') {
      commit_line();
      ++i;
      continue;
    }

    if (c == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
      std::size_t j = i + 2;
      while (j < text.size() && (text[j] < '@' || text[j] > '~')) {
        ++j;
      }
      if (j < text.size()) {
        const char command = text[j];
        const std::string params = text.substr(i + 2, j - (i + 2));

        if (command == 'A') {
          int lines_up = parse_csi_param(params, 1);
          if (lines_up < 0) {
            lines_up = 0;
          }
          const std::size_t cursor =
            (process->pending_cursor == std::string::npos) ? process->pending_line.size() : process->pending_cursor;
          for (int step = 0; step < lines_up; ++step) {
            if (process->view.logs.empty()) {
              break;
            }
            process->pending_line = process->view.logs.back();
            process->view.logs.pop_back();
          }
          process->pending_cursor = std::min(cursor, process->pending_line.size());
          i = j + 1;
          continue;
        }

        if (command == 'K') {
          int mode = parse_csi_param(params, 0);
          std::size_t cursor =
            (process->pending_cursor == std::string::npos) ? process->pending_line.size() : process->pending_cursor;
          if (mode == 1) {
            if (cursor <= process->pending_line.size()) {
              process->pending_line.erase(0, cursor);
            } else {
              process->pending_line.clear();
            }
            process->pending_cursor = 0;
          } else if (mode == 2) {
            process->pending_line.clear();
            process->pending_cursor = 0;
          } else {
            if (cursor < process->pending_line.size()) {
              process->pending_line.erase(cursor);
            }
          }

          i = j + 1;
          continue;
        }
      }
    }

    write_char(c);
    ++i;
  }

  process->view.live_line = process->pending_line;

  enforce_log_limit(process);
}

void ProcessManager::append_log_line(ManagedProcess* process, const std::string& text) {
  append_log(process, text);
  process->view.logs.push_back(process->pending_line);
  process->pending_line.clear();
  process->pending_cursor = std::string::npos;
  process->view.live_line.clear();
  enforce_log_limit(process);
}

void ProcessManager::poll_external_attach() {
#ifdef _WIN32
  if (!_external_attach.active) {
    return;
  }

  if (_external_attach.process_index >= _processes.size()) {
    close_external_attach("External attach session closed.");
    return;
  }

  auto& process = _processes[_external_attach.process_index];
  if (!process.view.running) {
    close_external_attach("Target process exited.");
    return;
  }

  const HANDLE pipe = reinterpret_cast<HANDLE>(_external_attach.listen_handle);
  if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE) {
    close_external_attach("External attach session closed.");
    return;
  }

  if (_external_attach.client_handle == 0) {
    if (ConnectNamedPipe(pipe, nullptr)) {
      _external_attach.client_handle = _external_attach.listen_handle;
      process.externally_attached = true;
      append_log_line(&process, "External attach connected.");
    } else {
      const DWORD error = GetLastError();
      if (error == ERROR_PIPE_CONNECTED) {
        _external_attach.client_handle = _external_attach.listen_handle;
        process.externally_attached = true;
        append_log_line(&process, "External attach connected.");
      } else if (error != ERROR_PIPE_LISTENING && error != ERROR_NO_DATA) {
        close_external_attach("External attach disconnected.");
      }
    }
  }

  if (_external_attach.client_handle == 0) {
    return;
  }

  std::string chunk;
  while (process.process->read_available(&chunk)) {
    append_log(&process, chunk);
    DWORD written = 0;
    if (!WriteFile(pipe, chunk.data(), static_cast<DWORD>(chunk.size()), &written, nullptr)) {
      close_external_attach("External attach disconnected.");
      return;
    }
    chunk.clear();
  }

  DWORD available = 0;
  if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
    close_external_attach("External attach disconnected.");
    return;
  }
  if (available == 0) {
    return;
  }

  char input[512];
  DWORD to_read = available > sizeof(input) ? static_cast<DWORD>(sizeof(input)) : available;
  DWORD bytes_read = 0;
  if (!ReadFile(pipe, input, to_read, &bytes_read, nullptr) || bytes_read == 0) {
    close_external_attach("External attach disconnected.");
    return;
  }

  std::string payload(input, input + bytes_read);
  std::string ignored;
  (void)send_input(_external_attach.process_index, payload, &ignored);
#else
  if (!_external_attach.active) {
    return;
  }
  if (_external_attach.process_index >= _processes.size()) {
    close_external_attach("External attach session closed.");
    return;
  }

  auto& process = _processes[_external_attach.process_index];
  if (!process.view.running) {
    close_external_attach("Target process exited.");
    return;
  }

  if (_external_attach.client_fd < 0) {
    const int client_fd = accept(_external_attach.listen_fd, nullptr, nullptr);
    if (client_fd >= 0) {
      const int flags = fcntl(client_fd, F_GETFL, 0);
      if (flags >= 0) {
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
      }
#if defined(__APPLE__) && defined(SO_NOSIGPIPE)
      int no_sigpipe = 1;
      (void)setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
      _external_attach.client_fd = client_fd;
      process.externally_attached = true;
      append_log_line(&process, "External attach connected.");
    }
  }

  if (_external_attach.client_fd < 0) {
    return;
  }

  std::string chunk;
  while (process.process->read_available(&chunk)) {
    append_log(&process, chunk);
    const ssize_t written = send_without_sigpipe(_external_attach.client_fd, chunk.data(), chunk.size());
    if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      close_external_attach("External attach disconnected.");
      return;
    }
    chunk.clear();
  }

  char input[512];
  const ssize_t bytes = read(_external_attach.client_fd, input, sizeof(input));
  if (bytes == 0) {
    close_external_attach("External attach disconnected.");
    return;
  }
  if (bytes > 0) {
    std::string payload(input, static_cast<std::size_t>(bytes));
    std::string ignored;
    (void)send_input(_external_attach.process_index, payload, &ignored);
  } else if (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
    close_external_attach("External attach disconnected.");
    return;
  }
#endif
}

void ProcessManager::close_external_attach(const std::string& reason) {
#ifdef _WIN32
  if (!_external_attach.active) {
    return;
  }

  if (_external_attach.process_index < _processes.size()) {
    auto& process = _processes[_external_attach.process_index];
    process.externally_attached = false;
    if (!reason.empty()) {
      append_log_line(&process, reason);
    }
  }

  const HANDLE pipe = reinterpret_cast<HANDLE>(_external_attach.listen_handle);
  if (pipe != nullptr && pipe != INVALID_HANDLE_VALUE) {
    if (_external_attach.client_handle != 0) {
      (void)DisconnectNamedPipe(pipe);
    }
    CloseHandle(pipe);
  }

  _external_attach = ExternalAttachSession{};
#else
  if (!_external_attach.active) {
    return;
  }

  if (_external_attach.process_index < _processes.size()) {
    auto& process = _processes[_external_attach.process_index];
    process.externally_attached = false;
    if (!reason.empty()) {
      append_log_line(&process, reason);
    }
  }

  if (_external_attach.client_fd >= 0) {
    close(_external_attach.client_fd);
  }
  if (_external_attach.listen_fd >= 0) {
    close(_external_attach.listen_fd);
  }
  if (!_external_attach.socket_path.empty()) {
    unlink(_external_attach.socket_path.c_str());
  }

  _external_attach = ExternalAttachSession{};
#endif
}

void ProcessManager::enforce_log_limit(ManagedProcess* process) {
  while (process->view.logs.size() > kMaxLogLines) {
    process->view.logs.pop_front();
  }
}
