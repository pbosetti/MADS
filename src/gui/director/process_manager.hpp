#pragma once

#include "director_config.hpp"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

class PlatformProcess;

struct ProcessRuntimeView {
  std::string name;
  std::string base_name;
  std::string command;
  bool running = false;
  bool ever_started = false;
  int pid = -1;
  int exit_code = 0;
  bool enabled = true;
  bool relaunch = false;
  bool tty = false;
  std::vector<std::string> dependencies;
  std::deque<std::string> logs;
  std::string live_line;
  double cpu_percent_per_core = 0.0;
  std::uint32_t thread_count = 0;
  std::uint64_t ram_bytes = 0;
  std::uint32_t cpu_count = 1;
  std::vector<float> cpu_percent_per_core_history;
};

class ProcessManager {
 public:
  ProcessManager();
  ~ProcessManager();
  ProcessManager(const ProcessManager&) = delete;
  ProcessManager& operator=(const ProcessManager&) = delete;
  ProcessManager(ProcessManager&&) = delete;
  ProcessManager& operator=(ProcessManager&&) = delete;

  bool initialize(const Mads::DirectorConfig& config, std::string* out_error);
  bool reload(const Mads::DirectorConfig& config, std::string* out_error);

  void launch_ready_processes();
  void tick();
  void stop_all();

  std::size_t process_count() const;
  const ProcessRuntimeView* process_at(std::size_t index) const;

  bool start_process(std::size_t index, std::string* out_error);
  bool stop_process(std::size_t index, std::string* out_error);
  bool restart_process(std::size_t index, std::string* out_error);
  bool send_siginfo(std::size_t index, std::string* out_error);
  bool send_input(std::size_t index, const std::string& input, std::string* out_error);
  bool attach_process(std::size_t index, std::string* out_error);
  bool open_external_attach(std::size_t index, const std::string& executable_path, std::string* out_error);

 private:
  struct ManagedProcess;
  struct ExternalAttachSession {
    std::size_t process_index = 0;
#ifdef _WIN32
    std::uintptr_t listen_handle = 0;
    std::uintptr_t client_handle = 0;
#else
    int listen_fd = -1;
    int client_fd = -1;
#endif
    std::string socket_path;
    bool active = false;
  };

  bool can_start(const ManagedProcess& process) const;
  bool start_process_internal(std::size_t index, std::string* out_error);
  void sample_metrics_if_due();
  void sample_process_metrics(ManagedProcess* process);
  void capture_output(ManagedProcess* process);
  void append_log(ManagedProcess* process, const std::string& text);
  void append_log_line(ManagedProcess* process, const std::string& text);
  void poll_external_attach();
  void close_external_attach(const std::string& reason);
  void enforce_log_limit(ManagedProcess* process);

  std::vector<ManagedProcess> _processes;
  ExternalAttachSession _external_attach;
  std::optional<std::string> _terminal_command;
  double _sample_rate_seconds = 2.0;
  std::uint64_t _last_sample_tick_ms = 0;
};
