#include "platform_process.hpp"

#ifndef _WIN32

#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace {

void enable_python_venv_for_workdir(const std::string& workdir) {
  if (workdir.empty()) {
    return;
  }

  std::error_code ec;
  const std::filesystem::path venv_root = std::filesystem::path(workdir) / ".venv";
  const std::filesystem::path venv_bin = venv_root / "bin";
  if (!std::filesystem::is_directory(venv_bin, ec)) {
    return;
  }

  setenv("VIRTUAL_ENV", venv_root.string().c_str(), 1);
  const char* path = std::getenv("PATH");
  std::string new_path = venv_bin.string();
  if (path != nullptr && path[0] != '\0') {
    new_path += ":";
    new_path += path;
  }
  setenv("PATH", new_path.c_str(), 1);
}

[[noreturn]] void exec_child_command(const std::string& command, const std::string& workdir) {
  if (!workdir.empty()) {
    (void)chdir(workdir.c_str());
    enable_python_venv_for_workdir(workdir);
  }

  const char* shell = std::getenv("SHELL");
  if (shell == nullptr || shell[0] == '\0') {
    shell = "/bin/sh";
  }

  const char* shell_name = std::strrchr(shell, '/');
  if (shell_name != nullptr && shell_name[1] != '\0') {
    shell_name = shell_name + 1;
  } else {
    shell_name = shell;
  }

  execl(shell, shell_name, "-lc", command.c_str(), static_cast<char*>(nullptr));
  if (std::strcmp(shell, "/bin/sh") != 0) {
    execl("/bin/sh", "sh", "-lc", command.c_str(), static_cast<char*>(nullptr));
  } else {
    execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
  }
  _exit(127);
}

class PosixProcess final : public PlatformProcess {
 public:
  ~PosixProcess() override {
    stop();
    close_fds();
  }

  bool start(const std::string& command, const std::string& workdir, bool tty, std::string* out_error) override {
    stop();
    close_fds();

    _tty = tty;
    _exit_code = 0;

    int out_pipe[2] = {-1, -1};
    int in_pipe[2] = {-1, -1};

    if (_tty) {
      _pid = forkpty(&_pty_master, nullptr, nullptr, nullptr);
      if (_pid < 0) {
        *out_error = std::string("forkpty failed: ") + std::strerror(errno);
        return false;
      }

      if (_pid == 0) {
        exec_child_command(command, workdir);
      }

      _stdin_fd = _pty_master;
      _stdout_fd = _pty_master;
      set_nonblock(_stdout_fd);
      _running = true;
      return true;
    }

    if (pipe(out_pipe) != 0 || pipe(in_pipe) != 0) {
      *out_error = std::string("pipe failed: ") + std::strerror(errno);
      close_if_open(out_pipe[0]);
      close_if_open(out_pipe[1]);
      close_if_open(in_pipe[0]);
      close_if_open(in_pipe[1]);
      return false;
    }

    _pid = fork();
    if (_pid < 0) {
      *out_error = std::string("fork failed: ") + std::strerror(errno);
      close_if_open(out_pipe[0]);
      close_if_open(out_pipe[1]);
      close_if_open(in_pipe[0]);
      close_if_open(in_pipe[1]);
      return false;
    }

    if (_pid == 0) {
      dup2(in_pipe[0], STDIN_FILENO);
      dup2(out_pipe[1], STDOUT_FILENO);
      dup2(out_pipe[1], STDERR_FILENO);

      close_if_open(in_pipe[0]);
      close_if_open(in_pipe[1]);
      close_if_open(out_pipe[0]);
      close_if_open(out_pipe[1]);

      exec_child_command(command, workdir);
    }

    _stdin_fd = in_pipe[1];
    _stdout_fd = out_pipe[0];
    close_if_open(in_pipe[0]);
    close_if_open(out_pipe[1]);
    set_nonblock(_stdout_fd);

    _running = true;
    return true;
  }

  void stop() override {
    if (!_running || _pid <= 0) {
      _running = false;
      close_fds();
      return;
    }

    (void)kill(_pid, SIGTERM);

    int status = 0;
    for (int i = 0; i < 20; ++i) {
      const pid_t result = waitpid(_pid, &status, WNOHANG);
      if (result == _pid) {
        update_exit_code(status);
        _running = false;
        close_fds();
        return;
      }
      if (result < 0 && errno == ECHILD) {
        _exit_code = 0;
        _running = false;
        close_fds();
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    (void)kill(_pid, SIGKILL);
    bool reaped = false;
    for (int i = 0; i < 20; ++i) {
      const pid_t result = waitpid(_pid, &status, WNOHANG);
      if (result == _pid) {
        update_exit_code(status);
        reaped = true;
        break;
      }
      if (result < 0 && errno == ECHILD) {
        _exit_code = 0;
        reaped = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (!reaped) {
      _exit_code = 1;
    }
    _running = false;
    close_fds();
  }

  bool is_running() override {
    if (!_running) {
      return false;
    }

    int status = 0;
    const pid_t result = waitpid(_pid, &status, WNOHANG);
    if (result == 0) {
      return true;
    }

    if (result == _pid) {
      update_exit_code(status);
    } else if (result < 0 && errno == ECHILD) {
      _exit_code = 0;
    }

    _running = false;
    close_fds();
    return false;
  }

  int exit_code() const override {
    return _exit_code;
  }

  int process_id() const override {
    return static_cast<int>(_pid);
  }

  bool read_available(std::string* out_chunk) override {
    if (_stdout_fd < 0) {
      return false;
    }

    char buffer[4096];
    const ssize_t read_bytes = read(_stdout_fd, buffer, sizeof(buffer));

    if (read_bytes > 0) {
      out_chunk->assign(buffer, static_cast<std::size_t>(read_bytes));
      return true;
    }

    if (read_bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return false;
    }

    return false;
  }

  bool write_input(const std::string& data) override {
    if (_stdin_fd < 0) {
      return false;
    }

    const ssize_t written = write(_stdin_fd, data.c_str(), data.size());
    return written == static_cast<ssize_t>(data.size());
  }

  bool attach_until_detach(std::string* out_error) override {
    if (!_tty) {
      *out_error = "Process was not started with tty=true.";
      return false;
    }

    if (!_running || _pty_master < 0) {
      *out_error = "TTY process is not running.";
      return false;
    }

    termios old_termios{};
    if (tcgetattr(STDIN_FILENO, &old_termios) != 0) {
      *out_error = std::string("tcgetattr failed: ") + std::strerror(errno);
      return false;
    }

    termios raw = old_termios;
    cfmakeraw(&raw);

    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
      *out_error = std::string("tcsetattr failed: ") + std::strerror(errno);
      return false;
    }

    const int stdin_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    const int pty_flags = fcntl(_pty_master, F_GETFL, 0);
    if (stdin_flags >= 0) {
      fcntl(STDIN_FILENO, F_SETFL, stdin_flags | O_NONBLOCK);
    }
    if (pty_flags >= 0) {
      fcntl(_pty_master, F_SETFL, pty_flags | O_NONBLOCK);
    }

    const char* banner = "\r\n[attached: Ctrl-] to detach]\r\n";
    write(STDOUT_FILENO, banner, std::strlen(banner));

    bool detached = false;
    bool process_exited = false;

    while (!detached) {
      if (!is_running()) {
        process_exited = true;
        break;
      }

      fd_set reads;
      FD_ZERO(&reads);
      FD_SET(STDIN_FILENO, &reads);
      FD_SET(_pty_master, &reads);
      const int max_fd = std::max(STDIN_FILENO, _pty_master);

      timeval timeout{};
      timeout.tv_sec = 0;
      timeout.tv_usec = 150000;

      const int ready = select(max_fd + 1, &reads, nullptr, nullptr, &timeout);
      if (ready < 0) {
        if (errno == EINTR) {
          continue;
        }
        *out_error = std::string("select failed: ") + std::strerror(errno);
        break;
      }

      if (FD_ISSET(_pty_master, &reads)) {
        char out_buffer[4096];
        const ssize_t bytes = read(_pty_master, out_buffer, sizeof(out_buffer));
        if (bytes > 0) {
          (void)write(STDOUT_FILENO, out_buffer, static_cast<std::size_t>(bytes));
        }
      }

      if (FD_ISSET(STDIN_FILENO, &reads)) {
        char in_buffer[512];
        const ssize_t bytes = read(STDIN_FILENO, in_buffer, sizeof(in_buffer));
        for (ssize_t i = 0; i < bytes; ++i) {
          const unsigned char ch = static_cast<unsigned char>(in_buffer[i]);
          if (ch == 0x1d) {
            detached = true;
            break;
          }
          (void)write(_pty_master, &in_buffer[i], 1);
        }
      }
    }

    if (stdin_flags >= 0) {
      fcntl(STDIN_FILENO, F_SETFL, stdin_flags);
    }
    if (pty_flags >= 0 && _pty_master >= 0) {
      fcntl(_pty_master, F_SETFL, pty_flags);
    }
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &old_termios);

    if (process_exited) {
      *out_error = "Process exited while attached.";
      return false;
    }

    const char* footer = "\r\n[detached]\r\n";
    write(STDOUT_FILENO, footer, std::strlen(footer));
    return true;
  }

 private:
  static void close_if_open(int fd) {
    if (fd >= 0) {
      close(fd);
    }
  }

  static void set_nonblock(int fd) {
    if (fd < 0) {
      return;
    }
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
      fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
  }

  void close_fds() {
    if (_stdin_fd >= 0 && _stdin_fd == _stdout_fd) {
      close_if_open(_stdin_fd);
      _stdin_fd = -1;
      _stdout_fd = -1;
    } else {
      close_if_open(_stdin_fd);
      close_if_open(_stdout_fd);
      _stdin_fd = -1;
      _stdout_fd = -1;
    }

    _pty_master = -1;
    _pid = -1;
  }

  void update_exit_code(int status) {
    if (WIFEXITED(status)) {
      _exit_code = WEXITSTATUS(status);
      return;
    }

    if (WIFSIGNALED(status)) {
      _exit_code = 128 + WTERMSIG(status);
      return;
    }

    _exit_code = 1;
  }

  pid_t _pid = -1;
  int _stdin_fd = -1;
  int _stdout_fd = -1;
  int _pty_master = -1;
  int _exit_code = 0;
  bool _running = false;
  bool _tty = false;
};

}  // namespace

std::unique_ptr<PlatformProcess> create_platform_process() {
  return std::make_unique<PosixProcess>();
}

#endif
