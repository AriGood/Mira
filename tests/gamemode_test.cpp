#include <doctest.h>

#include "runner/GameMode.h"

using namespace mira;

// Environment-dependent by nature (this machine may or may not have
// gamemoded installed) — same posture as the Proton/Wine discovery tests in
// runner_test.cpp. The contract under test is just "never crashes, never
// blocks, always answers something", which matters more here than most:
// this is what backs GET /v1/gamemode/status, which the frontend polls to
// warn about a misconfiguration.
TEST_CASE("gamemode::IsInstalled and IsDaemonRunning never throw or hang") {
  const bool installed = gamemode::IsInstalled();
  const bool running = gamemode::IsDaemonRunning();
  // A daemon can't be running without being installed.
  if (running) CHECK(installed);
}

TEST_CASE("gamemode::RegisterGame/UnregisterGame on an implausible pid never crash") {
  // Exercises the real gdbus shell-out path; a missing daemon (the common
  // case on a dev/CI machine) must be a silent no-op, not a failure.
  gamemode::RegisterGame(999999);
  gamemode::UnregisterGame(999999);
}
