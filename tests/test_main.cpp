// Custom entry point, replacing Catch2::Catch2WithMain for every test
// executable. On Windows, end the process via TerminateProcess() right after
// the test session finishes, instead of returning normally. zmqpp's
// internal, process-lifetime static (zmqpp::actor::actor_pipe_ctx_) is not
// safely destructible there once CurveAuth has been used during the
// process's life, and aborts during exit-time teardown well after Catch2 has
// already reported results. A normal return (or exit()/_Exit()/quick_exit())
// all still funnel through ExitProcess(), which sends DLL_PROCESS_DETACH to
// every loaded DLL and runs MadsCore.dll's own static destructors --
// including the problematic one. TerminateProcess() is the only way to end
// the process without that DLL-unload notification. This works around an
// upstream zmqpp/libzmq Windows limitation, not a MADS bug. POSIX keeps a
// normal return so coverage instrumentation (gcov's atexit-registered flush)
// still runs.
#include <catch2/catch_session.hpp>
#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char *argv[]) {
  int result = Catch::Session().run(argc, argv);
#ifdef _WIN32
  TerminateProcess(GetCurrentProcess(), static_cast<UINT>(result));
#endif
  return result;
}
