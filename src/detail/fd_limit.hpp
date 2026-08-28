/*
Internal helper: reads, plans and applies the process-wide open-file-descriptor
limit (POSIX RLIMIT_NOFILE), backing the `[broker] max_open_files` setting.
Not part of the installed SDK (src/detail/ is excluded from the LIB_HEADERS
install glob in CMakeLists.txt).

WHY THIS EXISTS. The broker holds two permanently open descriptors per
connected agent -- the agent's PUB connects to the XSUB frontend and its SUB to
the XPUB backend -- plus a third, transient one while the agent fetches its
settings over the REQ/ROUTER settings socket. With Linux's usual soft
RLIMIT_NOFILE of 1024 and the broker's own ~30 descriptors of overhead, that
walls a fleet in at roughly 490 agents, and libzmq reports the wall almost
invisibly: tcp_listener_t::accept() lists EMFILE/ENFILE among its non-fatal
errnos, so a refused agent produces only a ZMQ_EVENT_ACCEPT_FAILED that nobody
was listening for.

The decision logic (plan_fd_limit) is deliberately pure -- it takes the
observed limits as data and makes no syscalls -- so every branch, including the
platforms the build is not currently running on, is unit-testable from
tests/test_fd_limit.cpp. Mirrors the "pure evaluator" split that
src/doctor_checks.hpp documents.
*/
#pragma once

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>

#ifndef _WIN32
#include <sys/resource.h>
#include <sys/time.h>
#endif

#ifdef __APPLE__
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

namespace Mads::detail {

/// Descriptors a single connected agent costs the broker for as long as it
/// stays connected: one for its publisher, one for its subscriber. (The
/// settings request needs a third, but only while it is in flight.)
inline constexpr uint64_t FD_PER_AGENT = 2;

/// The broker's own descriptor footprint, independent of fleet size: three
/// listening sockets, ~10 libzmq socket mailboxes, the context reaper's and
/// each I/O thread's poller, stdio, and the settings-file watch.
///
/// Platform-dependent, because the mailboxes dominate it and their cost is
/// not the same everywhere: libzmq's signaler uses eventfd() where it exists
/// (Linux), costing one descriptor per mailbox, and falls back to a
/// socketpair -- two descriptors -- everywhere else. Measured at idle: ~34 on
/// Linux, 49 on macOS. Rounded up in both cases, so the capacity estimate
/// errs toward understating how many agents will fit rather than overstating.
#if defined(__linux__)
inline constexpr uint64_t FD_BROKER_OVERHEAD = 40;
#else
inline constexpr uint64_t FD_BROKER_OVERHEAD = 64;
#endif

/// Soft limit at or below which raising is worth suggesting -- the default on
/// essentially every Linux distribution, and the value systemd hands a unit
/// that does not set LimitNOFILE=.
inline constexpr uint64_t FD_LOW_WATERMARK = 1024;

/// Last-resort ceiling when the OS reports an unbounded hard limit and no
/// kernel cap could be read. Matches Linux's own default fs.nr_open.
inline constexpr uint64_t FD_ABSOLUTE_CEILING = 1048576;

/// How many agents a given soft limit leaves room for.
inline uint64_t agent_capacity(uint64_t soft) {
  if (soft <= FD_BROKER_OVERHEAD)
    return 0;
  return (soft - FD_BROKER_OVERHEAD) / FD_PER_AGENT;
}

/// The descriptor limits in force for this process. `supported` is false where
/// the concept does not apply (Windows), in which case soft/hard are 0 and
/// nothing should be reported as a limit.
struct FdLimits {
  bool supported = false;
  uint64_t soft = 0;
  /// The ceiling this process may raise `soft` to without privileges. Already
  /// clamped to the kernel's own per-process cap (fs.nr_open on Linux,
  /// kern.maxfilesperproc on macOS), so a plan built from it never proposes a
  /// target that setrlimit() would reject.
  uint64_t hard = 0;
};

namespace fd_limit_impl {

/// Reads a single unsigned integer out of a /proc or /sys file.
inline std::optional<uint64_t> read_uint_file(const char *path) {
  std::ifstream in(path);
  if (!in)
    return std::nullopt;
  uint64_t value = 0;
  if (!(in >> value))
    return std::nullopt;
  return value;
}

/// The kernel's own hard ceiling on a per-process descriptor table, which is
/// NOT the same thing as RLIMIT_NOFILE's rlim_max: on macOS rlim_max is
/// routinely RLIM_INFINITY while setrlimit() still fails with EINVAL above
/// kern.maxfilesperproc, and on Linux it fails with EPERM above fs.nr_open.
/// Clamping here is what keeps plan_fd_limit() from proposing the impossible.
inline std::optional<uint64_t> kernel_fd_ceiling() {
#if defined(__APPLE__)
  int value = 0;
  size_t size = sizeof(value);
  if (sysctlbyname("kern.maxfilesperproc", &value, &size, nullptr, 0) == 0 &&
      value > 0) {
    return static_cast<uint64_t>(value);
  }
  return std::nullopt;
#elif defined(__linux__)
  return read_uint_file("/proc/sys/fs/nr_open");
#else
  return std::nullopt;
#endif
}

} // namespace fd_limit_impl

/// Reads the limits currently in force, with `hard` clamped as described above.
inline FdLimits query_fd_limits() {
  FdLimits limits;
#ifdef _WIN32
  // Windows has no RLIMIT_NOFILE. libzmq uses SOCKET handles, which are not C
  // runtime descriptors and are bounded by the process handle quota rather
  // than a settable per-process table; _setmaxstdio() governs only stdio
  // FILE* streams and would do nothing for sockets. Reporting this as
  // unsupported is honest; silently pretending to apply a limit would not be.
  limits.supported = false;
#else
  rlimit rl{};
  if (getrlimit(RLIMIT_NOFILE, &rl) != 0)
    return limits;
  limits.supported = true;
  limits.soft = static_cast<uint64_t>(rl.rlim_cur);
  limits.hard = rl.rlim_max == RLIM_INFINITY
                    ? FD_ABSOLUTE_CEILING
                    : static_cast<uint64_t>(rl.rlim_max);
  if (const auto ceiling = fd_limit_impl::kernel_fd_ceiling())
    limits.hard = std::min(limits.hard, *ceiling);
  // A hard limit below the soft one is nonsensical, but clamping keeps every
  // downstream comparison well-behaved rather than underflowing.
  limits.hard = std::max(limits.hard, limits.soft);
#endif
  return limits;
}

/// What should be done about the limit, and what to tell the operator. Pure:
/// built from `requested` plus the observed limits, with no syscalls.
struct FdLimitPlan {
  enum class Action {
    NotSupported, ///< no per-process descriptor limit on this platform
    Unset,        ///< nothing configured; report the limit and leave it alone
    Invalid,      ///< configured value makes no sense; ignored, limit untouched
    Unchanged,    ///< the soft limit is already exactly what was asked for
    Raise,        ///< raise the soft limit to `target`
    Lower         ///< lower the soft limit to `target`
  };

  /// Set when the request was above the hard limit and `target` had to be
  /// capped to it. Orthogonal to `action`, which still describes what happens
  /// to the soft limit: a clamped request can still raise, lower or change
  /// nothing at all.
  bool clamped = false;

  Action action = Action::Unset;
  /// The soft limit that should end up in force.
  uint64_t target = 0;
  /// One line, ready to print, explaining the limit and what was done to it.
  std::string message;
  /// True when `message` reports something the operator should act on.
  bool warn = false;
};

/// Renders e.g. "1024 soft / 1048576 hard, about 492 agents".
inline std::string describe_fd_limits(const FdLimits &limits) {
  return std::to_string(limits.soft) + " soft / " + std::to_string(limits.hard) +
         " hard, about " + std::to_string(agent_capacity(limits.soft)) +
         " agents";
}

/// The hint appended to every message that reports a limit worth raising.
inline std::string fd_limit_hint() {
  return "set `max_open_files` in the [broker] section of the settings file "
         "(or LimitNOFILE= in the systemd unit) to raise it";
}

/// Decides what to do. `requested` is the resolved `max_open_files` value:
/// nullopt leaves the limit untouched, 0 means "as high as this process is
/// allowed to go", and a positive value is a specific soft limit.
inline FdLimitPlan plan_fd_limit(std::optional<int64_t> requested,
                                 const FdLimits &limits) {
  FdLimitPlan plan;

  if (!limits.supported) {
    plan.action = FdLimitPlan::Action::NotSupported;
    // Only worth saying anything when the operator actually asked for a limit
    // and is entitled to know it did not take effect.
    if (requested.has_value()) {
      plan.message = "max_open_files is not applicable on this platform: it "
                     "has no per-process descriptor limit for sockets";
      plan.warn = true;
    }
    return plan;
  }

  plan.target = limits.soft;

  if (!requested.has_value()) {
    plan.action = FdLimitPlan::Action::Unset;
    plan.message = "File descriptors: " + describe_fd_limits(limits);
    // Nagging is only useful where raising would actually buy something.
    if (limits.soft <= FD_LOW_WATERMARK && limits.hard > limits.soft) {
      plan.warn = true;
      plan.message += " -- " + fd_limit_hint();
    }
    return plan;
  }

  if (*requested < 0) {
    // Same posture as [broker] io_threads: a bad value in the settings file
    // must not stop the broker from starting.
    plan.action = FdLimitPlan::Action::Invalid;
    plan.warn = true;
    plan.message = "Invalid [broker] max_open_files = " +
                   std::to_string(*requested) + ", ignoring it. File "
                   "descriptors: " + describe_fd_limits(limits);
    return plan;
  }

  // 0 means "give me everything this process is permitted to have".
  uint64_t wanted = *requested == 0 ? limits.hard
                                    : static_cast<uint64_t>(*requested);

  // Above the hard limit the request is capped rather than refused: only
  // LimitNOFILE= in the unit, or a privileged `ulimit -Hn`, can lift that.
  plan.clamped = wanted > limits.hard;
  if (plan.clamped)
    wanted = limits.hard;

  plan.target = wanted;

  if (wanted == limits.soft) {
    plan.action = FdLimitPlan::Action::Unchanged;
    plan.message = "File descriptors: " + describe_fd_limits(limits) +
                   " (max_open_files already in force)";
  } else if (wanted > limits.soft) {
    plan.action = FdLimitPlan::Action::Raise;
    plan.message = "File descriptors: raising soft limit " +
                   std::to_string(limits.soft) + " -> " +
                   std::to_string(wanted) + " (hard " +
                   std::to_string(limits.hard) + "), about " +
                   std::to_string(agent_capacity(wanted)) + " agents";
  } else {
    // Lowering is deliberate and allowed: the soft limit moves freely below
    // the hard one, needing no privileges. It is how a broker is capped on a
    // shared box, and -- the reason it must not be silently ignored -- the
    // only way to exercise the descriptor-exhaustion path without root.
    //
    // Always reported as a warning: it is by far the minority case, and an
    // accidental one (128 typed for 1280) is otherwise discovered only when
    // the fleet stops growing.
    plan.action = FdLimitPlan::Action::Lower;
    plan.warn = true;
    plan.message = "File descriptors: lowering soft limit " +
                   std::to_string(limits.soft) + " -> " +
                   std::to_string(wanted) + " (hard " +
                   std::to_string(limits.hard) + "), room for about " +
                   std::to_string(agent_capacity(wanted)) + " agents";
    // Below its own footprint the broker cannot finish starting: it runs out
    // while creating its own sockets, long before an agent ever connects.
    // Saying so here is much cheaper than letting it die mid-startup.
    if (wanted <= FD_BROKER_OVERHEAD) {
      plan.message += " -- WARNING: the broker needs about " +
                      std::to_string(FD_BROKER_OVERHEAD) +
                      " descriptors for itself and will most likely fail to"
                      " start with this few";
    }
  }

  if (plan.clamped) {
    plan.warn = true;
    plan.message += ". max_open_files = " + std::to_string(*requested) +
                    " exceeds this process's hard limit of " +
                    std::to_string(limits.hard) +
                    "; raising that needs LimitNOFILE= in the systemd unit or"
                    " a privileged `ulimit -Hn`";
  }
  return plan;
}

/// The result of acting on a plan.
struct FdLimitOutcome {
  FdLimits limits;      ///< the limits in force afterwards
  bool applied = false; ///< a setrlimit() call was made and succeeded
  std::string error;    ///< why it failed, when it did
};

/// Carries out `plan`. Only ever moves the soft limit: rlim_max is left
/// untouched, since raising it needs CAP_SYS_RESOURCE and would simply fail
/// for an unprivileged broker, while moving the soft limit anywhere at or
/// below the hard one never needs privileges at all -- in either direction.
///
/// Lowering does not close descriptors that are already open; it only makes
/// further allocations fail. Since the broker applies this before binding
/// anything, that distinction does not arise in practice.
inline FdLimitOutcome apply_fd_limit(const FdLimitPlan &plan) {
  FdLimitOutcome outcome;
  outcome.limits = query_fd_limits();

  const bool wants_change = plan.action == FdLimitPlan::Action::Raise ||
                            plan.action == FdLimitPlan::Action::Lower;
  if (!wants_change || !outcome.limits.supported)
    return outcome;

#ifndef _WIN32
  rlimit rl{};
  if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
    outcome.error = std::strerror(errno);
    return outcome;
  }
  rl.rlim_cur = static_cast<rlim_t>(plan.target);
  if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
    outcome.error = std::strerror(errno);
    return outcome;
  }
  outcome.applied = true;
  outcome.limits = query_fd_limits();
#endif
  return outcome;
}

/// True when `err` is the errno of a process (or system) descriptor table that
/// has filled up -- the condition every explanatory message here exists for.
inline bool is_fd_exhaustion(int err) {
#ifdef _WIN32
  // WSAEMFILE is what Winsock reports; EMFILE covers the CRT paths.
  return err == EMFILE || err == 10024 /* WSAEMFILE */;
#else
  return err == EMFILE || err == ENFILE;
#endif
}

} // namespace Mads::detail
