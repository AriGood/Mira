#include <doctest.h>

#include <algorithm>
#include <filesystem>

#include "config/Config.h"
#include "runner/Exec.h"
#include "runner/FlatpakRunner.h"

using namespace mira;

TEST_CASE("FlatpakRunner::BuildCommand: no args means no trailing --") {
  model::Game game;
  game.id = "flatpak-org.example.App";
  game.runner_ref = "flatpak:org.example.App";

  runner::FlatpakRunner flatpak;
  auto command = flatpak.BuildCommand(game, std::nullopt);
  REQUIRE(command.has_value());
  CHECK(command->argv == std::vector<std::string>{"flatpak", "run", "org.example.App"});
}

TEST_CASE("FlatpakRunner::BuildCommand translates game.env into --env= flags, and args after --") {
  model::Game game;
  game.id = "flatpak-org.example.App";
  game.runner_ref = "flatpak:org.example.App";
  game.args = "--fullscreen --no-splash";
  game.env["FOO"] = "bar";

  runner::FlatpakRunner flatpak;
  auto command = flatpak.BuildCommand(game, std::nullopt);
  REQUIRE(command.has_value());
  CHECK(command->argv == std::vector<std::string>{"flatpak", "run", "--env=FOO=bar", "org.example.App", "--",
                                                   "--fullscreen", "--no-splash"});
  // Not merged into command.env -- see FlatpakRunner.cpp's comment: the
  // sandbox only ever sees the --env= flags above, not a MergedEnv overlay.
  CHECK(command->env.empty());
}

TEST_CASE("FlatpakRunner::BuildCommand rejects a game with no flatpak: runner_ref") {
  model::Game game;
  game.id = "flatpak-org.example.App";
  runner::FlatpakRunner flatpak;
  CHECK_FALSE(flatpak.BuildCommand(game, std::nullopt).has_value());
}

TEST_CASE("ListInstalledFlatpakApps degrades to an empty list, never an error, when flatpak isn't on PATH") {
  // This machine may or may not actually have flatpak installed — either is
  // fine, the contract under test is just "never an error either way".
  const auto apps = runner::ListInstalledFlatpakApps();
  CHECK(apps.has_value());
}

TEST_CASE("FlatpakRunner::Discover is always empty -- installed apps aren't runner builds") {
  // Confirmed against this machine's real installed apps (several) as well
  // as a config with none discoverable -- either way this must never list
  // them, or every installed Flatpak app pollutes a runner-build picker
  // meant for Proton/Wine versions. See FlatpakRunner.h's UsesBuilds().
  config::Config config(std::filesystem::temp_directory_path() / "mira-tests" / "flatpak-discover-settings.toml");
  config.Load();
  runner::FlatpakRunner flatpak;
  CHECK(flatpak.Discover(config).empty());
}
