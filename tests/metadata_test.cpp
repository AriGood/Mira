#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <json.hpp>

#include "api/EventBus.h"
#include "config/Config.h"
#include "metadata/FetchQueue.h"
#include "metadata/MetadataFetcher.h"
#include "model/Types.h"

using namespace mira;
namespace fs = std::filesystem;

namespace {
fs::path TempDir(const char* name) {
  const fs::path dir = fs::temp_directory_path() / "mira-tests" / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}
}  // namespace

// Everything here deliberately exercises only the non-Steam path with no
// steamgriddb.api_key set: that's the one branch that's fully offline (see
// MetadataFetcher.cpp's FetchNonSteam), so these stay hermetic without
// mocking curl. The Steam/ProtonDB/SteamGridDB paths themselves were
// verified live against real APIs during development, not here.

TEST_CASE("Fetch on a non-Steam game with no SteamGridDB key fails, and caches nothing") {
  // SteamGridDB is the only free cover source for a non-Steam game, so with
  // no key there is nothing this could have tried. Reporting success left a
  // cache file and a game.metadata_ready event behind, which reads as
  // "looked and found nothing" — and the next attempt then looked answered.
  const fs::path dir = TempDir("metadata-nonsteam");
  config::Config config(dir / "settings.toml");
  config.Load();

  model::Game game;
  game.id = "some-game";
  game.name = "Some Game";
  // runner_ref left empty -> not Steam-owned.

  const Result<void> fetched = metadata::Fetch(config, game);
  REQUIRE_FALSE(fetched.has_value());
  CHECK(fetched.error().code == "no_steamgriddb_key");
  // The message names the setting, because it is the whole remedy.
  CHECK(fetched.error().message.find("steamgriddb.api_key") != std::string::npos);

  CHECK_FALSE(fs::exists(metadata::MetadataFile(config, game.id)));
  CHECK_FALSE(fs::exists(metadata::ArtworkDir(config, game.id)));
}

TEST_CASE("Metadata/artwork cache paths sit next to settings.toml, not a global XDG lookup") {
  const fs::path dir = TempDir("metadata-paths");
  config::Config config(dir / "settings.toml");
  config.Load();

  CHECK(metadata::MetadataFile(config, "foo") == dir / "metadata" / "foo.json");
  CHECK(metadata::ArtworkDir(config, "foo") == dir / "artwork" / "foo");
}

TEST_CASE("FetchQueue::Enqueue is a no-op when metadata.enabled is false") {
  const fs::path dir = TempDir("metadata-disabled");
  config::Config config(dir / "settings.toml");
  config.Load();
  REQUIRE(config.Set("metadata.enabled", false).has_value());

  api::EventBus events;
  model::Game game;
  game.id = "disabled-game";
  game.name = "Disabled Game";

  {
    metadata::FetchQueue queue;
    queue.Enqueue(config, events, game);
  }  // destructor blocks until any spawned fetch finishes -- none should exist

  CHECK_FALSE(fs::exists(metadata::MetadataFile(config, game.id)));
}

TEST_CASE("FetchQueue::Enqueue force=true bypasses metadata.enabled") {
  const fs::path dir = TempDir("metadata-forced");
  config::Config config(dir / "settings.toml");
  config.Load();
  REQUIRE(config.Set("metadata.enabled", false).has_value());

  api::EventBus events;
  model::Game game;
  game.id = "forced-game";
  game.name = "Forced Game";

  {
    metadata::FetchQueue queue;
    queue.Enqueue(config, events, game, /*force=*/true);
  }  // destructor blocks until the forced fetch completes

  // The fetch ran, which with no key means it ran and failed — the event is
  // the observable, since a failed fetch deliberately writes no cache file.
  const std::vector<model::Event> published = events.Since(0);
  REQUIRE(published.size() == 1);
  CHECK(published[0].type == "game.metadata_failed");
  CHECK(published[0].payload.value("id", std::string()) == "forced-game");
  CHECK(published[0].payload.value("error", std::string()).find("steamgriddb.api_key") !=
        std::string::npos);
}

TEST_CASE("A failed fetch publishes game.metadata_failed and no game.metadata_ready") {
  // What a client keys off: metadata_ready has to mean there is something to
  // show, or a frontend refreshing its artwork on that event refreshes into
  // the same placeholder it already had.
  const fs::path dir = TempDir("metadata-failed-event");
  config::Config config(dir / "settings.toml");
  config.Load();

  api::EventBus events;
  model::Game game;
  game.id = "keyless";
  game.name = "Keyless";

  {
    metadata::FetchQueue queue;
    queue.Enqueue(config, events, game);
  }

  for (const model::Event& event : events.Since(0)) {
    CHECK(event.type != "game.metadata_ready");
  }
}
