#include <doctest.h>

#include <filesystem>
#include <fstream>

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

TEST_CASE("Fetch on a non-Steam game with no SteamGridDB key writes a minimal cache file, no artwork") {
  const fs::path dir = TempDir("metadata-nonsteam");
  config::Config config(dir / "settings.toml");
  config.Load();

  model::Game game;
  game.id = "some-game";
  game.name = "Some Game";
  // runner_ref left empty -> not Steam-owned.

  const Result<void> fetched = metadata::Fetch(config, game);
  REQUIRE(fetched.has_value());

  const fs::path metadata_file = metadata::MetadataFile(config, game.id);
  REQUIRE(fs::exists(metadata_file));

  const nlohmann::json info = nlohmann::json::parse(std::ifstream(metadata_file));
  CHECK(info["source"] == "steamgriddb");
  CHECK_FALSE(info.contains("artwork"));  // no key configured, nothing to fetch
  CHECK_FALSE(info.contains("steam"));
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

  CHECK(fs::exists(metadata::MetadataFile(config, game.id)));
}
