#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "config/Config.h"
#include "model/Types.h"
#include "runner/Exec.h"
#include "runner/RunnerRegistry.h"
#include "runner/Winetricks.h"

using namespace mira;
namespace fs = std::filesystem;

namespace {
fs::path TempFile(const char* name) { return fs::temp_directory_path() / "mira-tests" / name; }
}  // namespace

TEST_CASE("RunTricksVerb rejects a native runner outright") {
  config::Config config(TempFile("tricks-native-settings.toml"));
  config.Load();
  const runner::RunnerRegistry registry(config);

  model::Game game;
  game.id = "native-game";
  game.data_dir = TempFile("tricks-native-prefix").string();
  fs::create_directories(fs::path(game.data_dir) / "drive_c");
  game.runner_ref = "native:native";

  const auto ran = runner::RunTricksVerb(registry, game, "corefonts");
  REQUIRE_FALSE(ran.has_value());
  CHECK(ran.error().code == "not_wine_based");

  fs::remove_all(game.data_dir);
}

TEST_CASE("RunTricksVerb rejects a game with no data_dir") {
  config::Config config(TempFile("tricks-nodatadir-settings.toml"));
  config.Load();
  const runner::RunnerRegistry registry(config);

  model::Game game;
  game.id = "no-prefix-game";
  game.runner_ref = "wine:system";

  const auto ran = runner::RunTricksVerb(registry, game, "corefonts");
  REQUIRE_FALSE(ran.has_value());
  CHECK(ran.error().code == "no_data_dir");
}

TEST_CASE("RunTricksVerb rejects an unprovisioned prefix") {
  config::Config config(TempFile("tricks-unprovisioned-settings.toml"));
  config.Load();
  const runner::RunnerRegistry registry(config);

  const fs::path data_dir = TempFile("tricks-unprovisioned-prefix");
  fs::remove_all(data_dir);
  fs::create_directories(data_dir);  // exists, but no drive_c -- never provisioned

  model::Game game;
  game.id = "unprovisioned-game";
  game.data_dir = data_dir.string();
  game.runner_ref = "wine:system";

  const auto ran = runner::RunTricksVerb(registry, game, "corefonts");
  REQUIRE_FALSE(ran.has_value());
  CHECK(ran.error().code == "not_provisioned");

  fs::remove_all(data_dir);
}
