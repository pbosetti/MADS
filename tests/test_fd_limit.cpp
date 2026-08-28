// Pins Mads::detail's descriptor-limit logic (src/detail/fd_limit.hpp), which
// backs the `[broker] max_open_files` setting.
//
// plan_fd_limit() is pure -- it takes the observed limits as data and makes no
// syscalls -- so every branch is exercised here on every platform, including
// the ones the build is not currently running on (a Windows "not applicable"
// plan is asserted from Linux, and vice versa).
//
// Port range for this file: 44400-44449 (none of these cases bind a socket,
// but the range is reserved for consistency with the rest of the suite).
#include <catch2/catch_test_macros.hpp>

#include <cerrno>

#include "detail/fd_limit.hpp"

using namespace Mads::detail;

namespace {

FdLimits limits(uint64_t soft, uint64_t hard, bool supported = true) {
  FdLimits l;
  l.supported = supported;
  l.soft = soft;
  l.hard = hard;
  return l;
}

bool contains(const std::string &haystack, const std::string &needle) {
  return haystack.find(needle) != std::string::npos;
}

} // namespace

/* ---- capacity arithmetic ------------------------------------------------ */

TEST_CASE("agent_capacity: two descriptors per agent above the broker's own",
          "[fd_limit]") {
  // The wall this whole feature exists for: a stock 1024 soft limit.
  REQUIRE(agent_capacity(1024) == (1024 - FD_BROKER_OVERHEAD) / 2);
  REQUIRE(agent_capacity(1024) == 496);
  REQUIRE(agent_capacity(65536) == (65536 - FD_BROKER_OVERHEAD) / 2);
}

TEST_CASE("agent_capacity: a limit below the broker's own footprint is zero",
          "[fd_limit]") {
  REQUIRE(agent_capacity(FD_BROKER_OVERHEAD) == 0);
  REQUIRE(agent_capacity(0) == 0);
  REQUIRE(agent_capacity(8) == 0);
}

/* ---- platforms without a descriptor limit ------------------------------- */

TEST_CASE("plan_fd_limit: unsupported platform says nothing when unconfigured",
          "[fd_limit]") {
  const auto plan = plan_fd_limit(std::nullopt, limits(0, 0, false));
  REQUIRE(plan.action == FdLimitPlan::Action::NotSupported);
  REQUIRE_FALSE(plan.warn);
  REQUIRE(plan.message.empty());
}

TEST_CASE("plan_fd_limit: unsupported platform warns that a request had no "
          "effect",
          "[fd_limit]") {
  const auto plan = plan_fd_limit(65536, limits(0, 0, false));
  REQUIRE(plan.action == FdLimitPlan::Action::NotSupported);
  REQUIRE(plan.warn);
  REQUIRE(contains(plan.message, "not applicable"));
}

/* ---- unconfigured: report, and nag only when raising would help ---------- */

TEST_CASE("plan_fd_limit: unconfigured reports the limit without touching it",
          "[fd_limit]") {
  const auto plan = plan_fd_limit(std::nullopt, limits(65536, 1048576));
  REQUIRE(plan.action == FdLimitPlan::Action::Unset);
  REQUIRE(plan.target == 65536);
  REQUIRE_FALSE(plan.warn);
  REQUIRE(contains(plan.message, "65536 soft"));
}

TEST_CASE("plan_fd_limit: unconfigured warns at the stock 1024 soft limit",
          "[fd_limit]") {
  const auto plan = plan_fd_limit(std::nullopt, limits(1024, 1048576));
  REQUIRE(plan.action == FdLimitPlan::Action::Unset);
  REQUIRE(plan.warn);
  REQUIRE(contains(plan.message, "max_open_files"));
  // The capacity estimate is the whole point of the line: it is what turns
  // "1024" into "this is why the 461st agent was refused".
  REQUIRE(contains(plan.message, "496 agents"));
}

TEST_CASE("plan_fd_limit: no nagging when the hard limit leaves nothing to gain",
          "[fd_limit]") {
  const auto plan = plan_fd_limit(std::nullopt, limits(1024, 1024));
  REQUIRE(plan.action == FdLimitPlan::Action::Unset);
  REQUIRE_FALSE(plan.warn);
}

/* ---- bad values are ignored, never fatal -------------------------------- */

TEST_CASE("plan_fd_limit: a negative request is ignored with a warning",
          "[fd_limit]") {
  const auto plan = plan_fd_limit(-1, limits(1024, 1048576));
  REQUIRE(plan.action == FdLimitPlan::Action::Invalid);
  REQUIRE(plan.warn);
  REQUIRE(plan.target == 1024); // untouched
  REQUIRE(contains(plan.message, "Invalid"));
}

/* ---- zero means "as high as this process may go" ------------------------ */

TEST_CASE("plan_fd_limit: zero targets the hard limit", "[fd_limit]") {
  const auto plan = plan_fd_limit(0, limits(1024, 1048576));
  REQUIRE(plan.action == FdLimitPlan::Action::Raise);
  REQUIRE(plan.target == 1048576);
}

TEST_CASE("plan_fd_limit: zero is a no-op when soft already equals hard",
          "[fd_limit]") {
  const auto plan = plan_fd_limit(0, limits(4096, 4096));
  REQUIRE(plan.action == FdLimitPlan::Action::AlreadyEnough);
  REQUIRE(plan.target == 4096);
}

/* ---- explicit targets --------------------------------------------------- */

TEST_CASE("plan_fd_limit: a request below the current soft limit is a no-op",
          "[fd_limit]") {
  const auto plan = plan_fd_limit(512, limits(4096, 1048576));
  REQUIRE(plan.action == FdLimitPlan::Action::AlreadyEnough);
  REQUIRE(plan.target == 4096); // never lowers an existing limit
}

TEST_CASE("plan_fd_limit: a request equal to the soft limit is a no-op",
          "[fd_limit]") {
  const auto plan = plan_fd_limit(4096, limits(4096, 1048576));
  REQUIRE(plan.action == FdLimitPlan::Action::AlreadyEnough);
  REQUIRE(plan.target == 4096);
}

TEST_CASE("plan_fd_limit: a reachable request raises the soft limit",
          "[fd_limit]") {
  const auto plan = plan_fd_limit(65536, limits(1024, 1048576));
  REQUIRE(plan.action == FdLimitPlan::Action::Raise);
  REQUIRE(plan.target == 65536);
  REQUIRE_FALSE(plan.warn);
  REQUIRE(contains(plan.message, "1024 -> 65536"));
}

TEST_CASE("plan_fd_limit: a request beyond the hard limit is clamped, not "
          "refused",
          "[fd_limit]") {
  const auto plan = plan_fd_limit(2000000, limits(1024, 1048576));
  REQUIRE(plan.action == FdLimitPlan::Action::Clamped);
  REQUIRE(plan.target == 1048576);
  REQUIRE(plan.warn);
  // The operator needs to know the remedy is outside the settings file.
  REQUIRE(contains(plan.message, "LimitNOFILE"));
}

TEST_CASE("plan_fd_limit: a clamp that lands on the current limit becomes a "
          "no-op but still warns",
          "[fd_limit]") {
  const auto plan = plan_fd_limit(65536, limits(1024, 1024));
  REQUIRE(plan.action == FdLimitPlan::Action::AlreadyEnough);
  REQUIRE(plan.target == 1024);
  REQUIRE(plan.warn); // the request could not be honoured; say so
}

/* ---- rendering ---------------------------------------------------------- */

TEST_CASE("describe_fd_limits: soft, hard and the implied agent count",
          "[fd_limit]") {
  const auto text = describe_fd_limits(limits(1024, 1048576));
  REQUIRE(contains(text, "1024 soft"));
  REQUIRE(contains(text, "1048576 hard"));
  REQUIRE(contains(text, "496 agents"));
}

/* ---- exhaustion errnos -------------------------------------------------- */

TEST_CASE("is_fd_exhaustion: recognises a full descriptor table", "[fd_limit]") {
  REQUIRE(is_fd_exhaustion(EMFILE));
#ifndef _WIN32
  REQUIRE(is_fd_exhaustion(ENFILE));
#endif
  REQUIRE_FALSE(is_fd_exhaustion(ECONNREFUSED));
  REQUIRE_FALSE(is_fd_exhaustion(0));
}

/* ---- the live query ----------------------------------------------------- */

TEST_CASE("query_fd_limits: reports a sane, self-consistent limit",
          "[fd_limit]") {
  const auto live = query_fd_limits();
#ifdef _WIN32
  REQUIRE_FALSE(live.supported);
#else
  REQUIRE(live.supported);
  REQUIRE(live.soft > 0);
  // query_fd_limits() clamps hard to the kernel's per-process ceiling and then
  // to at least soft, so this must hold however exotic the host's rlimits are.
  REQUIRE(live.hard >= live.soft);
  REQUIRE(live.hard <= FD_ABSOLUTE_CEILING);
#endif
}
