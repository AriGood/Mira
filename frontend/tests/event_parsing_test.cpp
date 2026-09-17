#include <doctest.h>

#include "client/MiradClient.h"

using namespace mira_gui;

// The SSE payload parsers. These decide what a live event does to the
// library on screen, and they run on data the frontend never requested — so
// a malformed or unexpected payload has to be a no-op, never a crash and
// never a half-applied update.

TEST_CASE("ParseGameSummary accepts the full record game.added carries") {
  GameSummary game;
  REQUIRE(MiradClient::ParseGameSummary(
      R"({"id": "blue-prince", "name": "Blue Prince", "status": "ready",
          "platform": "windows", "confidence": 1.0, "play_seconds": 0})",
      &game));
  CHECK(game.id == "blue-prince");
  CHECK(game.name == "Blue Prince");
  CHECK(game.status == "ready");
}

TEST_CASE("ParseGameSummary rejects anything that isn't a JSON object") {
  GameSummary game;
  CHECK_FALSE(MiradClient::ParseGameSummary("", &game));
  CHECK_FALSE(MiradClient::ParseGameSummary("not json", &game));
  CHECK_FALSE(MiradClient::ParseGameSummary("[]", &game));
  CHECK_FALSE(MiradClient::ParseGameSummary("null", &game));
  CHECK_FALSE(MiradClient::ParseGameSummary("42", &game));
}

TEST_CASE("ParseGameState reads the launch/exit signal") {
  GameStateEvent state;
  REQUIRE(MiradClient::ParseGameState(R"({"id": "x", "state": "running", "pid": 1234})", &state));
  CHECK(state.id == "x");
  CHECK(state.state == "running");

  REQUIRE(MiradClient::ParseGameState(
      R"({"id": "x", "state": "crashed", "exit_code": 139, "played_seconds": 11})", &state));
  CHECK(state.state == "crashed");
}

TEST_CASE("ParseGameState requires an id") {
  // Without one there is no row to act on, and acting on the wrong row is
  // worse than ignoring the event.
  GameStateEvent state;
  CHECK_FALSE(MiradClient::ParseGameState(R"({"state": "running"})", &state));
  CHECK_FALSE(MiradClient::ParseGameState("not json", &state));
}

TEST_CASE("ParseRemovedId returns an empty id rather than throwing") {
  CHECK(MiradClient::ParseRemovedId(R"({"id": "wandering-sword"})") == "wandering-sword");
  CHECK(MiradClient::ParseRemovedId(R"({})").empty());
  CHECK(MiradClient::ParseRemovedId("[]").empty());
  CHECK(MiradClient::ParseRemovedId("").empty());
}

TEST_CASE("ParseRunnerDownload ignores unrelated event types") {
  // HandleEvent is called for every event on the stream, not just runner
  // ones, so the type check is what keeps a game.updated from being read as
  // a download.
  RunnerDownloadEvent event;
  CHECK_FALSE(MiradClient::ParseRunnerDownload("game.added", R"({"id": "x"})", &event));
  CHECK_FALSE(MiradClient::ParseRunnerDownload("runners.updated", R"({})", &event));
  CHECK(MiradClient::ParseRunnerDownload("runners.download.started", R"({})", &event));
}
