#pragma once
/*
  Header-only C++20 process metrics by PID (default: current process).
  Platforms: Windows, Linux, macOS.

  CPU percentages are computed from deltas, therefore they are meaningful only
  after at least two calls to sample().
*/

#include <cstdint>
#include <chrono>
#include <string>
#include <fstream>
#include <sstream>
#include <optional>
#include <thread>
#include <mutex>

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <tlhelp32.h>
  #include <psapi.h>
  #pragma comment(lib, "psapi.lib")
#elif defined(__linux__)
  #include <unistd.h>
#elif defined(__APPLE__)
  #include <unistd.h>
  #include <libproc.h>
  #include <sys/proc_info.h>
  #include <mach/mach.h>
  #include <mach/mach_time.h>
  #include <mach/host_info.h>
  #include <mach/mach_host.h>
#else
  #error "Unsupported platform"
#endif

namespace mads {

/**
 * @brief Read-only process resource metrics for a given PID.
 *
 * Provides the following metrics for a target process:
 * - number of threads
 * - resident memory (RSS / working set)
 * - CPU load (relative to total machine capacity and per-core scale)
 *
 * @note For macOS, querying other PIDs may require elevated privileges
 *       (task_for_pid entitlement/root); otherwise metrics may be unavailable.
 */
class process_metrics {
public:
  /** @brief PID type used by the platform. */
  using pid_type =
#if defined(_WIN32)
    DWORD;
#else
    pid_t;
#endif

  /** @brief Steady clock type used internally. */
  using clock = std::chrono::steady_clock;

  /**
   * @brief Construct metrics for a target process.
   *
   * @param pid Optional PID; if not provided, the current process PID is used.
   */
  explicit process_metrics(std::optional<pid_type> pid = std::nullopt)
  : pid_(pid.value_or(current_pid())),
    ncpu_(std::max(1u, std::thread::hardware_concurrency()))
  {}

  /**
   * @brief Get the PID associated with this instance.
   * @return Target PID.
   */
  pid_type pid() const noexcept { return pid_; }

  /**
   * @brief Take a new CPU usage sample for the target PID.
   *
   * Reads process CPU time and system CPU time, and updates internal deltas.
   *
   * @note CPU percentages returned by cpu_percent_*() are meaningful only after
   *       at least two calls to sample().
   */
  void sample() {
    std::scoped_lock lk(m_);
    auto proc_ns = process_cpu_time_ns_(pid_);
    auto sys_ns  = system_cpu_time_ns_();

    if (proc_ns && sys_ns) {
      if (last_) {
        auto dproc = (*proc_ns >= last_->proc_ns) ? (*proc_ns - last_->proc_ns) : 0ULL;
        auto dsys  = (*sys_ns  >= last_->sys_ns)  ? (*sys_ns  - last_->sys_ns)  : 0ULL;

        cpu_total_capacity_ =
          (dsys > 0)
            ? (static_cast<double>(dproc) * 100.0) / static_cast<double>(dsys)
            : 0.0;
      }
      last_ = sample_state{ *proc_ns, *sys_ns };
    } else {
      cpu_total_capacity_ = 0.0;
      last_.reset();
    }
  }

  /**
   * @brief CPU usage as percentage of total machine capacity.
   *
   * 0..100, where 100 means the process is using ~all CPU capacity across all
   * logical CPUs (i.e., saturating the machine).
   *
   * @return CPU percentage in range [0, 100] (best-effort).
   * @note Meaningful only after at least two calls to sample().
   */
  double cpu_percent_total_capacity() const {
    std::scoped_lock lk(m_);
    return cpu_total_capacity_ < 0.0 ? 0.0 : cpu_total_capacity_;
  }

  /**
   * @brief CPU usage scaled per logical core.
   *
   * 0..(100 * cpu_count), where 100 corresponds to one fully saturated logical core.
   *
   * @return CPU percentage in range [0, 100*cpu_count()] (best-effort).
   * @note Meaningful only after at least two calls to sample().
   */
  double cpu_percent_per_core() const {
    std::scoped_lock lk(m_);
    return (cpu_total_capacity_ < 0.0 ? 0.0 : cpu_total_capacity_)
           * static_cast<double>(ncpu_);
  }

  /**
   * @brief Get instantaneous number of threads in the target process.
   * @return Thread count; 0 if unavailable (e.g., insufficient permissions).
   */
  std::uint32_t thread_count() const {
#if defined(_WIN32)
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    THREADENTRY32 te{};
    te.dwSize = sizeof(te);

    std::uint32_t count = 0;
    if (Thread32First(snap, &te)) {
      do {
        if (te.th32OwnerProcessID == pid_) ++count;
      } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return count;
#elif defined(__linux__)
    std::ifstream f("/proc/" + std::to_string(pid_) + "/status");
    if (!f) return 0;
    std::string line;
    while (std::getline(f, line)) {
      if (line.rfind("Threads:", 0) == 0) {
        std::istringstream iss(line.substr(8));
        std::uint32_t n = 0;
        iss >> n;
        return n;
      }
    }
    return 0;
#elif defined(__APPLE__)
    proc_taskinfo info{};
    if (read_task_info_(pid_, &info)) {
      return static_cast<std::uint32_t>(info.pti_threadnum);
    }

    mach_port_t task = task_for_pid_port_(pid_);
    if (task == MACH_PORT_NULL) return 0;

    thread_act_array_t thread_list = nullptr;
    mach_msg_type_number_t thread_count = 0;
    if (task_threads(task, &thread_list, &thread_count) != KERN_SUCCESS) {
      return 0;
    }

    for (mach_msg_type_number_t i = 0; i < thread_count; ++i) {
      mach_port_deallocate(mach_task_self(), thread_list[i]);
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(thread_list),
                  static_cast<vm_size_t>(thread_count * sizeof(thread_act_t)));

    return static_cast<std::uint32_t>(thread_count);
#endif
  }

  /**
   * @brief Get resident memory usage (RSS / working set) in bytes.
   *
   * - Windows: Working Set Size.
   * - Linux: RSS derived from /proc/<pid>/statm.
   * - macOS: resident_size from Mach task info.
   *
   * @return Resident bytes; 0 if unavailable.
   */
  std::uint64_t ram_bytes() const {
#if defined(_WIN32)
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                           FALSE, pid_);
    if (!h) return 0;

    PROCESS_MEMORY_COUNTERS_EX pmc{};
    std::uint64_t ws = 0;
    if (GetProcessMemoryInfo(h,
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
      ws = static_cast<std::uint64_t>(pmc.WorkingSetSize);
    }
    CloseHandle(h);
    return ws;
#elif defined(__linux__)
    std::ifstream f("/proc/" + std::to_string(pid_) + "/statm");
    if (!f) return 0;

    std::uint64_t size=0, resident=0;
    f >> size >> resident;
    long page = ::sysconf(_SC_PAGESIZE);
    if (page <= 0) return 0;

    return resident * static_cast<std::uint64_t>(page);
#elif defined(__APPLE__)
    proc_taskinfo proc_info{};
    if (read_task_info_(pid_, &proc_info)) {
      return static_cast<std::uint64_t>(proc_info.pti_resident_size);
    }

    mach_port_t task = task_for_pid_port_(pid_);
    if (task == MACH_PORT_NULL) return 0;

    task_basic_info_data_t mach_info{};
    mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
    if (task_info(task, TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&mach_info), &count) != KERN_SUCCESS)
      return 0;

    return static_cast<std::uint64_t>(mach_info.resident_size);
#endif
  }

  /**
   * @brief Get number of logical CPUs detected on the system.
   * @return Logical CPU count (>= 1).
   */
  std::uint32_t cpu_count() const { return ncpu_; }

private:
  struct sample_state {
    std::uint64_t proc_ns;
    std::uint64_t sys_ns;
  };

  /**
   * @brief Get PID of the current process.
   * @return Current process PID.
   */
  static pid_type current_pid() {
#if defined(_WIN32)
    return GetCurrentProcessId();
#else
    return ::getpid();
#endif
  }

#if defined(__APPLE__)
  static std::uint64_t mach_absolute_to_ns_(std::uint64_t ticks) {
    mach_timebase_info_data_t timebase{};
    if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.denom == 0) {
      return ticks;
    }
    const long double ns = static_cast<long double>(ticks) *
                           static_cast<long double>(timebase.numer) /
                           static_cast<long double>(timebase.denom);
    return static_cast<std::uint64_t>(ns);
  }

  static bool read_task_info_(pid_type pid, proc_taskinfo* out_info) {
    if (out_info == nullptr) {
      return false;
    }

    proc_taskinfo info{};
    const int bytes = proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &info, PROC_PIDTASKINFO_SIZE);
    if (bytes != PROC_PIDTASKINFO_SIZE) {
      return false;
    }

    *out_info = info;
    return true;
  }

  /**
   * @brief Obtain a Mach task port for a given PID.
   *
   * @param pid Target PID.
   * @return Mach task port, or MACH_PORT_NULL if unavailable.
   * @note Accessing other PIDs typically requires privileges.
   */
  static mach_port_t task_for_pid_port_(pid_type pid) {
    if (pid == current_pid())
      return mach_task_self();

    mach_port_t task = MACH_PORT_NULL;
    if (task_for_pid(mach_task_self(), pid, &task) != KERN_SUCCESS)
      return MACH_PORT_NULL;
    return task;
  }
#endif

  /**
   * @brief Read total process CPU time (user + kernel) in nanoseconds.
   *
   * @param pid Target PID.
   * @return Process CPU time in nanoseconds, or std::nullopt if unavailable.
   */
  static std::optional<std::uint64_t> process_cpu_time_ns_(pid_type pid) {
#if defined(_WIN32)
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return std::nullopt;

    FILETIME ft_create{}, ft_exit{}, ft_kernel{}, ft_user{};
    if (!GetProcessTimes(h, &ft_create, &ft_exit, &ft_kernel, &ft_user)) {
      CloseHandle(h);
      return std::nullopt;
    }

    auto to100 = [](const FILETIME& ft) -> std::uint64_t {
      ULARGE_INTEGER u{};
      u.LowPart = ft.dwLowDateTime;
      u.HighPart = ft.dwHighDateTime;
      return static_cast<std::uint64_t>(u.QuadPart);
    };

    std::uint64_t total_ns = (to100(ft_kernel) + to100(ft_user)) * 100ULL; /* 100ns -> ns */
    CloseHandle(h);
    return total_ns;

#elif defined(__linux__)
    std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
    if (!f) return std::nullopt;

    int dummy = 0;
    std::string comm;
    char state = 0;

    f >> dummy >> comm >> state;
    if (!f) return std::nullopt;

    /* Skip fields 4..13 (10 fields), then read utime (14) and stime (15). */
    for (int i = 0; i < 10; ++i) { std::string tmp; f >> tmp; if (!f) return std::nullopt; }

    std::uint64_t ut = 0, st = 0;
    f >> ut >> st;
    if (!f) return std::nullopt;

    long hz = ::sysconf(_SC_CLK_TCK);
    if (hz <= 0) return std::nullopt;

    long double sec = static_cast<long double>(ut + st) / static_cast<long double>(hz);
    return static_cast<std::uint64_t>(sec * 1.0e9L);

#elif defined(__APPLE__)
    proc_taskinfo proc_info{};
    if (read_task_info_(pid, &proc_info)) {
      const std::uint64_t total = static_cast<std::uint64_t>(proc_info.pti_total_user + proc_info.pti_total_system);
      return mach_absolute_to_ns_(total);
    }

    mach_port_t task = task_for_pid_port_(pid);
    if (task == MACH_PORT_NULL) return std::nullopt;

    task_thread_times_info_data_t mach_info{};
    mach_msg_type_number_t count = TASK_THREAD_TIMES_INFO_COUNT;
    if (task_info(task, TASK_THREAD_TIMES_INFO,
                  reinterpret_cast<task_info_t>(&mach_info), &count) != KERN_SUCCESS)
      return std::nullopt;

    auto tv2ns = [](time_value_t tv) -> std::uint64_t {
      return static_cast<std::uint64_t>(tv.seconds) * 1000000000ULL +
             static_cast<std::uint64_t>(tv.microseconds) * 1000ULL;
    };

    return tv2ns(mach_info.user_time) + tv2ns(mach_info.system_time);
#endif
  }

  /**
   * @brief Read total accumulated system CPU time across all logical CPUs in nanoseconds.
   *
   * This is used as the denominator for computing "% of total machine capacity".
   *
   * @return System CPU time in nanoseconds, or std::nullopt if unavailable.
   */
  static std::optional<std::uint64_t> system_cpu_time_ns_() {
#if defined(_WIN32)
    FILETIME ft_idle{}, ft_kernel{}, ft_user{};
    if (!GetSystemTimes(&ft_idle, &ft_kernel, &ft_user))
      return std::nullopt;

    auto to100 = [](const FILETIME& ft) -> std::uint64_t {
      ULARGE_INTEGER u{};
      u.LowPart = ft.dwLowDateTime;
      u.HighPart = ft.dwHighDateTime;
      return static_cast<std::uint64_t>(u.QuadPart);
    };

    return (to100(ft_kernel) + to100(ft_user)) * 100ULL; /* 100ns -> ns */

#elif defined(__linux__)
    std::ifstream f("/proc/stat");
    if (!f) return std::nullopt;

    std::string cpu;
    f >> cpu;
    if (!f || cpu != "cpu") return std::nullopt;

    std::uint64_t u=0,n=0,s=0,i=0,iw=0,irq=0,si=0,st=0;
    f >> u >> n >> s >> i >> iw >> irq >> si >> st;
    if (!f) return std::nullopt;

    long hz = ::sysconf(_SC_CLK_TCK);
    if (hz <= 0) return std::nullopt;

    long double sec = static_cast<long double>(u + n + s + i + iw + irq + si + st)
                    / static_cast<long double>(hz);
    return static_cast<std::uint64_t>(sec * 1.0e9L);

#elif defined(__APPLE__)
    host_cpu_load_info_data_t info{};
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics64(mach_host_self(), HOST_CPU_LOAD_INFO,
                          reinterpret_cast<host_info64_t>(&info), &count) != KERN_SUCCESS)
      return std::nullopt;

    std::uint64_t ticks =
      static_cast<std::uint64_t>(info.cpu_ticks[CPU_STATE_USER]) +
      static_cast<std::uint64_t>(info.cpu_ticks[CPU_STATE_SYSTEM]) +
      static_cast<std::uint64_t>(info.cpu_ticks[CPU_STATE_IDLE]) +
      static_cast<std::uint64_t>(info.cpu_ticks[CPU_STATE_NICE]);

    long hz = ::sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100; /* fallback */

    long double sec = static_cast<long double>(ticks) / static_cast<long double>(hz);
    return static_cast<std::uint64_t>(sec * 1.0e9L);
#endif
  }

private:
  pid_type pid_;
  std::uint32_t ncpu_;
  mutable std::mutex m_;
  std::optional<sample_state> last_;
  double cpu_total_capacity_ = 0.0;
};

} // namespace mads
