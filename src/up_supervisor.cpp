#include "up_supervisor.hpp"

#include "broker_probe.hpp"

#include <reproc/error.h>
#include <reproc/reproc.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <regex>
#include <thread>

namespace Mads {

namespace {

constexpr std::chrono::milliseconds kBackoffInitial{100};
constexpr std::chrono::milliseconds kBackoffCap{30000};
// If a relaunch=true process ran at least this long before dying, its
// backoff resets to kBackoffInitial instead of continuing to double -- an
// occasional crash shouldn't permanently push a normally-stable process
// towards the 30s cap.
constexpr std::chrono::milliseconds kStableThreshold{10000};
constexpr std::chrono::milliseconds kPollInterval{100};

std::mutex &output_mutex() {
  static std::mutex m;
  return m;
}

// Minimal whitespace/quote-aware tokenizer for --no-shell. Handles single-
// and double-quoted segments and backslash escapes; not a full shell
// grammar (no globbing, no variable expansion -- by design, that's exactly
// what --no-shell opts out of).
std::vector<std::string> tokenize_command(const std::string &command) {
  std::vector<std::string> tokens;
  std::string current;
  bool in_token = false;
  char quote = '\0';

  for (size_t i = 0; i < command.size(); ++i) {
    const char c = command[i];
    if (quote != '\0') {
      if (c == quote) {
        quote = '\0';
      } else if (c == '\\' && quote == '"' && i + 1 < command.size()) {
        current.push_back(command[++i]);
      } else {
        current.push_back(c);
      }
      in_token = true;
      continue;
    }
    if (c == '\'' || c == '"') {
      quote = c;
      in_token = true;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(c))) {
      if (in_token) {
        tokens.push_back(current);
        current.clear();
        in_token = false;
      }
      continue;
    }
    if (c == '\\' && i + 1 < command.size()) {
      current.push_back(command[++i]);
      in_token = true;
      continue;
    }
    current.push_back(c);
    in_token = true;
  }
  if (in_token) {
    tokens.push_back(current);
  }
  return tokens;
}

// Candidate argvs to try, in order, for shelling out to `command`. Mirrors
// mads_director v2.4.2's exec_child_command() (see director_config.cpp's
// top-of-file comment): try $SHELL first, fall back to /bin/sh -lc, then
// finally plain /bin/sh -c. Windows has no fallback chain (Director always uses
// `cmd.exe /S /C "<command>"`) and does not go through argv at all -- see
// spawn_shell_verbatim().
//
// Caveat: reproc's argv[]-only API can't express Director's C-level
// distinction between "path used to exec" and "argv[0] shown to the child"
// (execl() there passes the resolved shell path for lookup but just its
// basename as argv[0]). Here argv[0] does double duty, so the child sees
// its full resolved shell path as $0 instead of a bare name. This is
// cosmetic only -- it doesn't change how "-lc"/"-c" are interpreted.
#ifndef _WIN32
std::vector<std::vector<std::string>> shell_candidates(const std::string &command) {
  std::vector<std::vector<std::string>> candidates;
  const char *env_shell = std::getenv("SHELL");
  if (env_shell != nullptr && env_shell[0] != '\0') {
    candidates.push_back({env_shell, "-lc", command});
  }
  candidates.push_back({"/bin/sh", "-lc", command});
  candidates.push_back({"/bin/sh", "-c", command});
  return candidates;
}
#endif

#ifndef _WIN32
// pipe() + FD_CLOEXEC, matching reproc's own posix/pipe.c fallback for
// platforms without pipe2(); *read_fd is the read end, *write_fd the write
// end. reproc itself uses fd value 0 as its own "not set" sentinel
// (reproc_destroy()/fd_close() no-op on 0), which is safe here too: this
// process already has fds 0/1/2 open, so a freshly created pipe end is never
// actually 0.
bool make_cloexec_pipe(int *read_fd, int *write_fd) {
  int fds[2] = {-1, -1};
  if (::pipe(fds) != 0) {
    return false;
  }
  ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  *read_fd = fds[0];
  *write_fd = fds[1];
  return true;
}

void close_if_valid(int fd) {
  if (fd >= 0) {
    ::close(fd);
  }
}

// Spawns argv (already resolved to an executable/PATH-searched name, per
// execvp()) with fresh stdin/stdout/stderr pipes, populating `out_process`'s
// public fields exactly as reproc_start() would (see reproc/reproc.h --
// `id`/`in`/`out`/`err`/`running` are documented struct members, not
// opaque), so the rest of this file can keep using reproc_read()/
// reproc_wait()/reproc_destroy()/reproc_exit_status() on it unchanged.
//
// The one thing this duplicates reproc's own posix/process.c for: the child
// calls setpgid(0, 0) *before* exec, making it its own process group leader
// so killpg() during teardown reaches its descendants too. reproc_start()
// itself cannot be made to do this from the caller's side -- it internally
// blocks on an exec-status pipe until the child has *already* called exec()
// (the classic synchronous-exec-error trick), so a setpgid() call placed
// after reproc_start() returns is always too late (confirmed empirically:
// it reliably lost the race on macOS during development of this file). The
// parent also calls setpgid() on the child pid, redundantly but harmlessly:
// racing the child's own call is safe by construction (POSIX's "child
// already exec'd" EACCES restriction only applies to a *parent* changing a
// *child's* group after that child has exec'd; a process changing its own
// group, as the child does here, is always legal).
REPROC_ERROR spawn_with_own_process_group(reproc_t *out_process,
                                          const char *const *argv,
                                          const char *working_directory) {
  int child_stdin = -1, parent_stdin = -1;
  int parent_stdout = -1, child_stdout = -1;
  int parent_stderr = -1, child_stderr = -1;
  int error_read = -1, error_write = -1;

  if (!make_cloexec_pipe(&child_stdin, &parent_stdin) ||
      !make_cloexec_pipe(&parent_stdout, &child_stdout) ||
      !make_cloexec_pipe(&parent_stderr, &child_stderr) ||
      !make_cloexec_pipe(&error_read, &error_write)) {
    close_if_valid(child_stdin);
    close_if_valid(parent_stdin);
    close_if_valid(parent_stdout);
    close_if_valid(child_stdout);
    close_if_valid(parent_stderr);
    close_if_valid(child_stderr);
    close_if_valid(error_read);
    close_if_valid(error_write);
    return REPROC_ERROR_SYSTEM;
  }

  const pid_t child_pid = ::fork();

  if (child_pid == 0) {
    // Child. First action: become our own process group leader.
    ::setpgid(0, 0);

    if (working_directory != nullptr && working_directory[0] != '\0' &&
        ::chdir(working_directory) == -1) {
      int e = errno;
      (void)!write(error_write, &e, sizeof(e));
      _exit(e);
    }
    if (::dup2(child_stdin, STDIN_FILENO) == -1 ||
        ::dup2(child_stdout, STDOUT_FILENO) == -1 ||
        ::dup2(child_stderr, STDERR_FILENO) == -1) {
      int e = errno;
      (void)!write(error_write, &e, sizeof(e));
      _exit(e);
    }
    const int max_fd = static_cast<int>(::sysconf(_SC_OPEN_MAX));
    for (int fd = 3; fd < max_fd; ++fd) {
      if (fd == error_write) {
        continue; // FD_CLOEXEC closes it for us once exec succeeds
      }
      ::close(fd);
    }
    ::execvp(argv[0], const_cast<char *const *>(argv));
    int e = errno;
    (void)!write(error_write, &e, sizeof(e));
    _exit(e);
  }

  if (child_pid < 0) {
    close_if_valid(child_stdin);
    close_if_valid(parent_stdin);
    close_if_valid(parent_stdout);
    close_if_valid(child_stdout);
    close_if_valid(parent_stderr);
    close_if_valid(child_stderr);
    close_if_valid(error_read);
    close_if_valid(error_write);
    return REPROC_ERROR_SYSTEM;
  }

  // See the function comment: redundant with the child's own call, but
  // together they make this race-free regardless of scheduling order.
  ::setpgid(child_pid, child_pid);

  close_if_valid(child_stdin);
  close_if_valid(child_stdout);
  close_if_valid(child_stderr);
  close_if_valid(error_write);

  int child_error = 0;
  ssize_t n = 0;
  do {
    n = ::read(error_read, &child_error, sizeof(child_error));
  } while (n < 0 && errno == EINTR);
  close_if_valid(error_read);

  if (n == static_cast<ssize_t>(sizeof(child_error))) {
    // The child reported a system error before (or instead of) exec()ing.
    close_if_valid(parent_stdin);
    close_if_valid(parent_stdout);
    close_if_valid(parent_stderr);
    int status = 0;
    ::waitpid(child_pid, &status, 0);
    errno = child_error;
    return REPROC_ERROR_SYSTEM;
  }

  out_process->running = true;
  out_process->exit_status = 0;
  out_process->id = child_pid;
  out_process->in = parent_stdin;
  out_process->out = parent_stdout;
  out_process->err = parent_stderr;
  return REPROC_SUCCESS;
}
#endif // !_WIN32

} // namespace

struct UpManagedProcess {
  ProcessConfig config;
  reproc_t proc{};
  bool running = false;
  int times_started = 0;
  int restart_count = 0;
  bool pending_restart = false;
  std::chrono::milliseconds next_backoff{kBackoffInitial};
  std::chrono::steady_clock::time_point started_at{};
  std::chrono::steady_clock::time_point restart_at{};
  int last_exit_code = 0;
  std::atomic<bool> ready_signal{false};
  std::thread stdout_reader;
  std::thread stderr_reader;
#ifdef _WIN32
  // `void *` = `HANDLE`. The job object holding this child and every process it
  // goes on to spawn; see make_job_for()/kill_job().
  void *job = nullptr;
#endif
};

namespace {

#ifdef _WIN32
// Windows equivalent of the setpgid()/killpg() pairing the POSIX path above
// uses. It is needed because nothing else kills a process *tree* here:
// reproc_kill() calls TerminateProcess() on the direct child only, and the
// direct child is almost always cmd.exe (see shell_candidates()), which --
// unlike `sh -c`, which execs and so becomes the workload -- stays resident as
// a parent. Killing it therefore orphans the actual workload instead of
// stopping it. That also stalls teardown: descendants inherit the child's
// stdout/stderr write handles, so reader_thread_main() never sees EOF and the
// joins below block until the orphan happens to exit on its own.
//
// A job object fixes both: every process the child spawns after being assigned
// joins the job automatically, and TerminateJobObject() takes down the whole
// tree in one call.
void *make_job_for(void *process_handle) {
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job == nullptr) {
    return nullptr;
  }
  // If this supervisor dies without unwinding, dropping the last handle to the
  // job takes the tree with it rather than leaking orphans.
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                          sizeof(limits));
  // Assignment can only happen once CreateProcess has returned, so a child that
  // spawns a grandchild within that window escapes the job. reproc offers no
  // CREATE_SUSPENDED hook to close it; in practice a shell needs milliseconds
  // to start up and parse before it spawns anything.
  if (!AssignProcessToJobObject(job, process_handle)) {
    CloseHandle(job);
    return nullptr;
  }
  return job;
}

// Kills whatever is still in the job and releases it. Idempotent, and safe once
// the managed process itself has exited: a job outlives its members, so this is
// also how orphans it left behind get reaped.
void kill_job(void *&job) {
  if (job == nullptr) {
    return;
  }
  TerminateJobObject(static_cast<HANDLE>(job), 1);
  CloseHandle(static_cast<HANDLE>(job));
  job = nullptr;
}

// UTF-8 to UTF-16 for CreateProcessW. std::nullopt means the input was not
// valid UTF-8.
std::optional<std::wstring> widen(const std::string &utf8) {
  if (utf8.empty()) {
    return std::wstring();
  }
  const int length = static_cast<int>(utf8.size());
  const int needed =
      MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), length, nullptr, 0);
  if (needed <= 0) {
    return std::nullopt;
  }
  std::wstring wide(static_cast<size_t>(needed), L'\0');
  if (MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), length, wide.data(),
                          needed) != needed) {
    return std::nullopt;
  }
  return wide;
}

// One pipe, with the end the parent keeps marked non-inheritable so it cannot
// leak into this or any other child. Mirrors reproc's windows/pipe.c.
bool make_pipe(HANDLE *read, bool inherit_read, HANDLE *write,
               bool inherit_write) {
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  if (!CreatePipe(read, write, &attributes, 0)) {
    return false;
  }
  if (!inherit_read && !SetHandleInformation(*read, HANDLE_FLAG_INHERIT, 0)) {
    return false;
  }
  if (!inherit_write && !SetHandleInformation(*write, HANDLE_FLAG_INHERIT, 0)) {
    return false;
  }
  return true;
}

// Spawns `cmd.exe /S /C "<command>"` with `command` inserted *verbatim*, and
// fills in `out_process` so reproc_read()/reproc_wait()/reproc_terminate()/
// reproc_kill()/reproc_exit_status()/reproc_destroy() all keep working on it --
// the same arrangement, and for the same kind of reason, as
// spawn_with_own_process_group() below does on POSIX.
//
// reproc_start() cannot be used for a shelled-out command here. It joins argv
// into a command line using the CRT/CommandLineToArgvW escaping rules, which
// rewrite every `"` inside an argument as `\"` (reproc's windows/process.c,
// argument_escape()). cmd.exe does not implement those rules: under /S it strips
// one leading and one trailing quote and takes the rest literally, so `\"`
// arrives as two characters. A command as ordinary as
// `echo hi >> "C:\dir\f.txt"` therefore reached cmd.exe as
// `echo hi >> \"C:\dir\f.txt\"` and died with "The filename, directory name, or
// volume label syntax is incorrect" -- every director.toml command containing a
// double quote was broken on Windows. Pre-escaping cannot work around it either,
// because argument_escape() only ever *adds* characters: no input makes it emit
// a bare `"`. The command line has to be built here instead.
REPROC_ERROR spawn_shell_verbatim(reproc_t *out_process,
                                  const std::string &command,
                                  const char *workdir) {
  *out_process = reproc_t{};

  // Same split reproc_start() uses: the child takes the read end of stdin and
  // the write ends of stdout/stderr, the parent keeps the opposite ends.
  HANDLE child_in = nullptr, child_out = nullptr, child_err = nullptr;
  HANDLE parent_in = nullptr, parent_out = nullptr, parent_err = nullptr;

  const auto close_all = [&] {
    for (HANDLE *handle : {&child_in, &child_out, &child_err, &parent_in,
                           &parent_out, &parent_err}) {
      if (*handle != nullptr) {
        CloseHandle(*handle);
        *handle = nullptr;
      }
    }
  };

  if (!make_pipe(&child_in, true, &parent_in, false) ||
      !make_pipe(&parent_out, false, &child_out, true) ||
      !make_pipe(&parent_err, false, &child_err, true)) {
    close_all();
    return REPROC_ERROR_SYSTEM;
  }

  auto command_line = widen("cmd.exe /S /C \"" + command + "\"");
  std::optional<std::wstring> workdir_wide;
  if (workdir != nullptr) {
    workdir_wide = widen(workdir);
  }
  if (!command_line.has_value() ||
      (workdir != nullptr && !workdir_wide.has_value())) {
    close_all();
    return REPROC_ERROR_SYSTEM;
  }

  // An explicit inherit list, so the child receives these three handles and
  // nothing else that happens to be inheritable at this moment.
  SIZE_T attribute_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
  std::vector<unsigned char> attribute_buffer(attribute_size);
  auto *attributes =
      reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_buffer.data());
  if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size)) {
    close_all();
    return REPROC_ERROR_SYSTEM;
  }

  HANDLE inherited[3] = {child_in, child_out, child_err};
  bool ok = UpdateProcThreadAttribute(attributes, 0,
                                      PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                      inherited, sizeof(inherited), nullptr,
                                      nullptr) != FALSE;

  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = child_in;
  startup.StartupInfo.hStdOutput = child_out;
  startup.StartupInfo.hStdError = child_err;
  startup.lpAttributeList = attributes;

  PROCESS_INFORMATION info{};
  if (ok) {
    // CREATE_NEW_PROCESS_GROUP for the reason reproc sets it too: it is what
    // reproc_terminate()'s CTRL_BREAK_EVENT is delivered to.
    ok = CreateProcessW(nullptr, command_line->data(), nullptr, nullptr, TRUE,
                        CREATE_NEW_PROCESS_GROUP | EXTENDED_STARTUPINFO_PRESENT,
                        nullptr,
                        workdir_wide.has_value() ? workdir_wide->c_str()
                                                 : nullptr,
                        &startup.StartupInfo, &info) != FALSE;
  }
  DeleteProcThreadAttributeList(attributes);

  // The child has its own copies now (or there is no child); either way the
  // parent is done with these, and stdout/stderr must not stay open here or the
  // reader threads would never see EOF.
  for (HANDLE *handle : {&child_in, &child_out, &child_err}) {
    CloseHandle(*handle);
    *handle = nullptr;
  }

  if (!ok) {
    close_all();
    return REPROC_ERROR_SYSTEM;
  }

  CloseHandle(info.hThread);
  out_process->running = true;
  out_process->id = info.dwProcessId;
  out_process->handle = info.hProcess;
  out_process->in = parent_in;
  out_process->out = parent_out;
  out_process->err = parent_err;
  return REPROC_SUCCESS;
}
#endif

// Reads one stream (stdout or stderr) of a managed process until it closes,
// line-buffering into multiplexed "[name] line" output and (for a
// `ready = "log:<regex>"` process) flipping ready_signal on a match. Runs on
// its own thread because reproc_read() blocks until data arrives or the
// stream closes (see reproc's posix/pipe.c: a plain blocking read()); the
// main supervisor loop in UpSupervisor::run() polls every managed process
// non-blockingly (reproc_wait(&proc, 0)) and would stall on whichever
// process happened to be quietest if it read stdout/stderr directly instead
// of delegating that to these per-process, per-stream threads.
void reader_thread_main(UpManagedProcess *mp, REPROC_STREAM stream,
                        bool quiet) {
  std::optional<std::regex> log_regex;
  const bool is_log_probe =
      mp->config.ready.has_value() && mp->config.ready->kind == ReadyKind::Log;
  if (is_log_probe) {
    try {
      log_regex = std::regex(mp->config.ready->log_pattern);
    } catch (const std::regex_error &) {
      // Already validated at config-parse time; unreachable in practice.
    }
  }

  const auto emit_line = [&](const std::string &line) {
    if (!quiet) {
      std::scoped_lock lock(output_mutex());
      std::ostream &os = (stream == REPROC_STREAM_ERR) ? std::cerr : std::cout;
      os << "[" << mp->config.name << "] " << line << "\n";
    }
    if (is_log_probe && log_regex.has_value() && !mp->ready_signal.load()) {
      if (std::regex_search(line, *log_regex)) {
        mp->ready_signal.store(true);
      }
    }
  };

  std::string buffer;
  uint8_t chunk[4096];
  while (true) {
    unsigned int n = 0;
    const REPROC_ERROR err =
        reproc_read(&mp->proc, stream, chunk, sizeof(chunk), &n);
    if (err != REPROC_SUCCESS || n == 0) {
      break;
    }
    buffer.append(reinterpret_cast<char *>(chunk), n);
    size_t pos;
    while ((pos = buffer.find('\n')) != std::string::npos) {
      std::string line = buffer.substr(0, pos);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      buffer.erase(0, pos + 1);
      emit_line(line);
    }
  }
  if (!buffer.empty()) {
    emit_line(buffer);
  }
}

// A `ready` probe that times out says only "nothing answered", which for a
// broker probe has one cause that no amount of staring at the plan reveals:
// the encryption mode did not match. A CURVE-secured broker drops a plain
// peer during the ZMTP handshake, and a plain broker drops an encrypted one,
// so either way a healthy broker looks exactly like an absent one. Name it
// here, in the same words `mads doctor` uses for the same failure.
std::string ready_failure_hint(const ReadySpec &spec, bool crypto) {
  if (spec.kind != ReadyKind::Broker) {
    return "";
  }
  return crypto
             ? " (probed " + spec.broker_uri +
                   " with CURVE; check --keys_dir/--key_client/--key_broker, "
                   "or drop --crypto if the broker runs unencrypted)"
             : " (probed " + spec.broker_uri +
                   " in the clear; pass --crypto if the broker runs with "
                   "CURVE encryption)";
}

} // namespace

UpSupervisor::UpSupervisor(std::vector<ProcessConfig> processes,
                          UpOptions options)
    : _processes(std::move(processes)), _options(std::move(options)) {
  _managed.reserve(_processes.size());
  for (const auto &p : _processes) {
    auto mp = std::make_unique<UpManagedProcess>();
    mp->config = p;
    _managed.push_back(std::move(mp));
  }
}

UpSupervisor::~UpSupervisor() {
  // Safety net if run() threw or was never fully drained: never leak a
  // running child.
  teardown();
}

void UpSupervisor::request_stop() { _stop_requested.store(true); }

bool UpSupervisor::stop_requested() const { return _stop_requested.load(); }

UpManagedProcess *UpSupervisor::find(const std::string &name) {
  for (auto &mp : _managed) {
    if (mp->config.name == name) {
      return mp.get();
    }
  }
  return nullptr;
}

int UpSupervisor::start_count(const std::string &name) const {
  for (const auto &mp : _managed) {
    if (mp->config.name == name) {
      return mp->times_started;
    }
  }
  return 0;
}

bool UpSupervisor::any_running() const {
  for (const auto &mp : _managed) {
    if (mp->running) {
      return true;
    }
  }
  return false;
}

bool UpSupervisor::start_one(UpManagedProcess &mp) {
  mp.proc = reproc_t{};

  std::vector<std::vector<std::string>> candidates;
  if (_options.no_shell) {
    auto tokens = tokenize_command(mp.config.command);
    if (tokens.empty()) {
      std::cerr << "[" << mp.config.name
               << "] --no-shell: command has no tokens: '" << mp.config.command
               << "'\n";
      return false;
    }
    candidates.push_back(std::move(tokens));
  }
#ifndef _WIN32
  else {
    candidates = shell_candidates(mp.config.command);
  }
#endif

  REPROC_ERROR err = REPROC_ERROR_SYSTEM;

#ifdef _WIN32
  if (!_options.no_shell) {
    // Leaves `candidates` empty, so the argv loop below is a no-op: shelling
    // out on Windows needs a verbatim command line, which reproc's argv API
    // cannot express. --no-shell still goes through the loop, where reproc's
    // CRT-style escaping is the correct convention for the program being run.
    err = spawn_shell_verbatim(&mp.proc, mp.config.command,
                               mp.config.workdir.empty()
                                   ? nullptr
                                   : mp.config.workdir.c_str());
  }
#endif

  for (const auto &argv_strings : candidates) {
    std::vector<const char *> argv;
    argv.reserve(argv_strings.size() + 1);
    for (const auto &s : argv_strings) {
      argv.push_back(s.c_str());
    }
    argv.push_back(nullptr);
    const char *workdir =
        mp.config.workdir.empty() ? nullptr : mp.config.workdir.c_str();
#ifdef _WIN32
    // reproc already puts the child in a new process group here
    // (CREATE_NEW_PROCESS_GROUP, see reproc's windows/process.c), which is what
    // reproc_terminate()'s CTRL_BREAK_EVENT needs. It is not enough to *kill* a
    // tree, though -- see make_job_for(), applied right after the spawn below.
    err = reproc_start(&mp.proc, argv.data(), nullptr, workdir);
#else
    // Not reproc_start(): see spawn_with_own_process_group()'s comment for
    // why a POSIX process group can't be set up by calling setpgid() after
    // reproc_start() returns.
    err = spawn_with_own_process_group(&mp.proc, argv.data(), workdir);
#endif
    if (err == REPROC_SUCCESS) {
      break;
    }
  }

  if (err != REPROC_SUCCESS) {
    std::cerr << "[" << mp.config.name << "] failed to start: "
             << reproc_strerror(err) << " (system error "
             << reproc_system_error() << ")\n";
    return false;
  }

#ifdef _WIN32
  // As early as possible after the spawn: anything the child starts from here
  // on is in the job and dies with it.
  mp.job = make_job_for(mp.proc.handle);
#endif

  mp.running = true;
  mp.ready_signal.store(false);
  mp.started_at = std::chrono::steady_clock::now();
  ++mp.times_started;

  mp.stdout_reader = std::thread(reader_thread_main, &mp, REPROC_STREAM_OUT,
                                 _options.quiet);
  mp.stderr_reader = std::thread(reader_thread_main, &mp, REPROC_STREAM_ERR,
                                 _options.quiet);

  {
    std::scoped_lock lock(output_mutex());
    std::cout << "[" << mp.config.name << "] started: " << mp.config.command
             << "\n";
  }
  return true;
}

bool UpSupervisor::wait_ready(UpManagedProcess &mp,
                              std::chrono::milliseconds timeout) {
  const ReadySpec &spec = *mp.config.ready;
  switch (spec.kind) {
  case ReadyKind::Delay:
    std::this_thread::sleep_for(spec.delay);
    return true;
  case ReadyKind::Broker:
    return probe_broker(spec.broker_uri, timeout, _options.curve);
  case ReadyKind::Port:
    return probe_tcp_port("localhost", spec.port, timeout);
  case ReadyKind::Log: {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!mp.ready_signal.load()) {
      if (_stop_requested.load() || !mp.running) {
        return false;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
  }
  }
  return true;
}

void UpSupervisor::stop_one(UpManagedProcess &mp,
                            std::chrono::milliseconds grace) {
  if (!mp.running) {
    return;
  }

  {
    std::scoped_lock lock(output_mutex());
    std::cout << "[" << mp.config.name << "] stopping (SIGTERM)\n";
  }

#ifdef _WIN32
  reproc_terminate(&mp.proc); // sends CTRL_BREAK_EVENT to the whole group
#else
  if (::killpg(mp.proc.id, SIGTERM) != 0) {
    ::kill(mp.proc.id, SIGTERM);
  }
#endif

  REPROC_ERROR err =
      reproc_wait(&mp.proc, static_cast<unsigned int>(grace.count()));
  if (err != REPROC_SUCCESS) {
    std::scoped_lock lock(output_mutex());
    std::cout << "[" << mp.config.name
             << "] still running after grace period, sending SIGKILL\n";
#ifdef _WIN32
    kill_job(mp.job);      // the whole tree, not just the direct child
    reproc_kill(&mp.proc); // fallback if the job could not be created
#else
    if (::killpg(mp.proc.id, SIGKILL) != 0) {
      ::kill(mp.proc.id, SIGKILL);
    }
#endif
    reproc_wait(&mp.proc, REPROC_INFINITE);
  }

  mp.last_exit_code = static_cast<int>(reproc_exit_status(&mp.proc));
  mp.running = false;

#ifdef _WIN32
  // Also for a child that stopped gracefully: it may still have left
  // descendants holding the stdout/stderr write handles, which would keep the
  // joins below from ever returning.
  kill_job(mp.job);
#endif

  // Join the readers *before* reproc_destroy() closes the pipe fds they may
  // still be blocked reading from (the process has already been reaped at
  // this point, so both reads should already be unblocking on EOF, but there
  // is no synchronization forcing that ordering -- join first to make it
  // one regardless).
  if (mp.stdout_reader.joinable()) {
    mp.stdout_reader.join();
  }
  if (mp.stderr_reader.joinable()) {
    mp.stderr_reader.join();
  }
  reproc_destroy(&mp.proc);
}

void UpSupervisor::teardown() {
  for (auto it = _managed.rbegin(); it != _managed.rend(); ++it) {
    UpManagedProcess &mp = **it;
    mp.pending_restart = false;
    if (mp.running) {
      stop_one(mp, _options.grace);
    }
  }
}

RunResult UpSupervisor::run() {
  RunResult result;

  if (_options.until_exit.has_value() && find(*_options.until_exit) == nullptr) {
    result.outcome = RunOutcome::StartFailed;
    result.exit_code = 1;
    result.message = "--until-exit target '" + *_options.until_exit +
                     "' is not a process in this director.toml";
    return result;
  }

  const std::optional<std::chrono::steady_clock::time_point> overall_deadline =
      _options.timeout ? std::optional<std::chrono::steady_clock::time_point>(
                            std::chrono::steady_clock::now() + *_options.timeout)
                       : std::nullopt;

  bool startup_ok = true;

  // --- start phase: dependency order, gated on each `ready` probe ----------
  for (auto &mp_ptr : _managed) {
    if (_stop_requested.load()) {
      break;
    }
    UpManagedProcess &mp = *mp_ptr;
    if (!mp.config.enabled) {
      std::scoped_lock lock(output_mutex());
      std::cout << "[" << mp.config.name << "] disabled, skipping auto-start\n";
      continue;
    }
    if (!start_one(mp)) {
      result.outcome = RunOutcome::StartFailed;
      result.exit_code = 1;
      result.message = "failed to start process '" + mp.config.name + "'";
      startup_ok = false;
      break;
    }
    if (mp.config.ready.has_value() &&
        !wait_ready(mp, _options.ready_timeout)) {
      result.outcome = RunOutcome::ReadyTimeout;
      result.exit_code = 1;
      result.message =
          "process '" + mp.config.name + "' did not become ready in time" +
          ready_failure_hint(*mp.config.ready, _options.curve.has_value());
      startup_ok = false;
      break;
    }
    if (overall_deadline.has_value() &&
        std::chrono::steady_clock::now() >= *overall_deadline) {
      result.outcome = RunOutcome::Timeout;
      result.exit_code = 1;
      result.message = "--timeout elapsed during startup";
      startup_ok = false;
      break;
    }
  }

  // --- monitor phase ---------------------------------------------------
  if (startup_ok && !_stop_requested.load()) {
    while (!_stop_requested.load()) {
      for (auto &mp_ptr : _managed) {
        UpManagedProcess &mp = *mp_ptr;

        if (mp.running) {
          const REPROC_ERROR err = reproc_wait(&mp.proc, 0);
          if (err == REPROC_ERROR_WAIT_TIMEOUT) {
            continue; // still running
          }

          mp.last_exit_code = static_cast<int>(reproc_exit_status(&mp.proc));
          mp.running = false;
#ifdef _WIN32
          // The managed process is done, so anything it left behind is an
          // orphan this supervisor owns -- and one still holding the pipe write
          // handles would block the joins below indefinitely.
          kill_job(mp.job);
#endif
          if (mp.stdout_reader.joinable()) {
            mp.stdout_reader.join();
          }
          if (mp.stderr_reader.joinable()) {
            mp.stderr_reader.join();
          }
          reproc_destroy(&mp.proc);

          {
            std::scoped_lock lock(output_mutex());
            std::cout << "[" << mp.config.name << "] exited with code "
                     << mp.last_exit_code << "\n";
          }

          if (_options.until_exit.has_value() &&
              *_options.until_exit == mp.config.name) {
            result.outcome = RunOutcome::Ok;
            result.exit_code = mp.last_exit_code;
            result.message =
                "--until-exit target '" + mp.config.name + "' exited";
            _stop_requested.store(true);
            break;
          }

          if (mp.config.relaunch) {
            const bool budget_left =
                _options.max_restarts < 0 || mp.restart_count < _options.max_restarts;
            if (budget_left) {
              const auto ran_for = std::chrono::steady_clock::now() - mp.started_at;
              if (ran_for >= kStableThreshold) {
                mp.next_backoff = kBackoffInitial;
              }
              mp.pending_restart = true;
              mp.restart_at = std::chrono::steady_clock::now() + mp.next_backoff;
              ++mp.restart_count;
              mp.next_backoff = std::min(mp.next_backoff * 2, kBackoffCap);
              continue;
            }
            std::scoped_lock lock(output_mutex());
            std::cerr << "[" << mp.config.name
                     << "] exceeded --max-restarts, giving up\n";
            continue;
          }

          if (mp.last_exit_code != 0) {
            result.outcome = RunOutcome::ProcessFailed;
            result.exit_code = mp.last_exit_code;
            result.message = "process '" + mp.config.name +
                             "' exited with code " +
                             std::to_string(mp.last_exit_code);
            _stop_requested.store(true);
            break;
          }
          // Clean one-shot exit, not the --until-exit target: leave it
          // stopped and keep supervising everything else.
        } else if (mp.pending_restart &&
                  std::chrono::steady_clock::now() >= mp.restart_at) {
          mp.pending_restart = false;
          {
            std::scoped_lock lock(output_mutex());
            std::cout << "[" << mp.config.name << "] relaunching (attempt "
                     << mp.restart_count << ")\n";
          }
          if (start_one(mp) && mp.config.ready.has_value()) {
            wait_ready(mp, _options.ready_timeout); // best-effort on relaunch
          }
        }
      }

      if (_stop_requested.load()) {
        break;
      }
      if (overall_deadline.has_value() &&
          std::chrono::steady_clock::now() >= *overall_deadline) {
        result.outcome = RunOutcome::Timeout;
        result.exit_code = 1;
        result.message = "--timeout elapsed";
        _stop_requested.store(true);
        break;
      }
      std::this_thread::sleep_for(kPollInterval);
    }
  }

  teardown();
  return result;
}

} // namespace Mads
