// Custom entry point, replacing Catch2::Catch2WithMain for every test
// executable.
//
// This used to end the process with TerminateProcess() on Windows, because
// zmqpp's internal, process-lifetime static (zmqpp::actor::actor_pipe_ctx_)
// was not safely destructible there once CurveAuth had been used, and aborted
// during exit-time teardown well after Catch2 had already reported results.
// The cppzmq migration removed zmqpp, and Mads::ZapAuth owns and joins its
// handler thread deterministically, so there is no exit-time state left to
// trip over and every platform can return normally again. That also restores
// gcov's atexit-registered flush on Windows.
#include <catch2/catch_session.hpp>

int main(int argc, char *argv[]) {
  return Catch::Session().run(argc, argv);
}
