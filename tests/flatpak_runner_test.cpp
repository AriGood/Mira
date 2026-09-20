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

  model::RunnerBuild build;
  build.kind = "flatpak";
  build.name = "org.example.App";

  runner::FlatpakRunner flatpak;
  auto command = flatpak.BuildCommand(game, build);
  REQUIRE(command.has_value());
  CHECK(command->argv == std::vector<std::string>{"flatpak", "run", "org.example.App"});
}

TEST_CASE("FlatpakRunner::BuildCommand translates game.env into --env= flags, and args after --") {
  model::Game game;
  game.id = "flatpak-org.example.App";
  game.args = "--fullscreen --no-splash";
  game.env["FOO"] = "bar";

  model::RunnerBuild build;
  build.kind = "flatpak";
  build.name = "org.example.App";

  runner::FlatpakRunner flatpak;
  auto command = flatpak.BuildCommand(game, build);
  REQUIRE(command.has_value());
  CHECK(command->argv == std::vector<std::string>{"flatpak", "run", "--env=FOO=bar", "org.example.App", "--",
                                                   "--fullscreen", "--no-splash"});
  // Not merged into command.env -- see FlatpakRunner.cpp's comment: the
  // sandbox only ever sees the --env= flags above, not a MergedEnv overlay.
  CHECK(command->env.empty());
}

TEST_CASE("FlatpakRunner::BuildCommand rejects an unresolved build") {
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

TEST_CASE("FlatpakRunner::Discover reflects what's actually installed on this machine") {
  if (!runner::FindOnPath("flatpak")) return;  // soft dependency, same posture as sqlite3/wine elsewhere

  config::Config config(std::filesystem::temp_directory_path() / "mira-tests" / "flatpak-discover-settings.toml");
  config.Load();
  runner::FlatpakRunner flatpak;
  const auto builds = flatpak.Discover(config);
  // Nothing to assert unconditionally beyond "it doesn't crash and every
  // build looks like a real one" -- environment-dependent by nature, same
  // posture as the Proton/Wine discovery tests in runner_test.cpp.
  for (const auto& build : builds) {
    CHECK(build.kind == "flatpak");
    CHECK_FALSE(build.name.empty());
  }
}
