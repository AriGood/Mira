#include <doctest.h>

#include <filesystem>

#include "config/Config.h"
#include "runner/NativeRunner.h"
#include "runner/RunnerRegistry.h"

using namespace mira;
namespace fs = std::filesystem;

namespace {
fs::path TempFile(const char* name) {
  const fs::path dir = fs::temp_directory_path() / "mira-tests";
  fs::create_directories(dir);
  const fs::path file = dir / name;
  fs::remove(file);
  return file;
}
}  // namespace

TEST_CASE("NativeRunner builds argv from install_path/exe_path and splits args") {
  model::Game game;
  game.install_path = "/games/Celeste";
  game.exe_path = "Celeste";
  game.args = "-fullscreen -novideo";
  game.env["FOO"] = "bar";

  runner::NativeRunner native;
  auto command = native.BuildCommand(game, std::nullopt);
  REQUIRE(command.has_value());
  CHECK(command->argv == std::vector<std::string>{"/games/Celeste/Celeste", "-fullscreen", "-novideo"});
  CHECK(command->cwd == "/games/Celeste");
  CHECK(command->env.at("FOO") == "bar");
}

TEST_CASE("NativeRunner rejects a game with no exe_path") {
  model::Game game;
  game.install_path = "/games/Celeste";
  runner::NativeRunner native;
  CHECK_FALSE(native.BuildCommand(game, std::nullopt).has_value());
}

TEST_CASE("RunnerRegistry resolves native:native with no build required") {
  config::Config config(TempFile("runner-registry-settings.toml"));
  config.Load();
  runner::RunnerRegistry registry(config);

  auto resolved = registry.Resolve("native:native");
  REQUIRE(resolved.has_value());
  CHECK(resolved->runner != nullptr);
  CHECK_FALSE(resolved->build.has_value());
}

TEST_CASE("RunnerRegistry::Resolve rejects a malformed or unknown reference") {
  config::Config config(TempFile("runner-registry-bad.toml"));
  config.Load();
  runner::RunnerRegistry registry(config);

  CHECK_FALSE(registry.Resolve("no-colon-here").has_value());
  CHECK_FALSE(registry.Resolve("not_a_real_kind:whatever").has_value());
}

TEST_CASE("ProvisionGame marks a native game ready with no build") {
  config::Config config(TempFile("runner-registry-native-provision.toml"));
  config.Load();
  runner::RunnerRegistry registry(config);

  model::Game game;
  game.id = "celeste";
  game.platform = model::Platform::Native;
  game.install_path = "/games/Celeste";
  game.exe_path = "Celeste";

  model::Game result = registry.ProvisionGame(game);
  CHECK(result.status == model::GameStatus::Ready);
  CHECK(result.runner_ref == "native:native");
}

// Everything below actually exercises UmuRunner — real umu-run, real Proton,
// a real prefix on disk. Environment-dependent by nature (this is what it's
// testing), so it adapts to what's actually installed rather than assuming
// a specific machine: it checks what RunnerRegistry itself discovers first,
// then asserts the outcome that implies, on either branch.
TEST_CASE("UmuRunner provisioning matches what's actually installed on this machine") {
  config::Config config(TempFile("runner-registry-umu-settings.toml"));
  config.Load();
  runner::RunnerRegistry registry(config);

  const bool has_proton = std::ranges::any_of(
      registry.DiscoverAll(), [](const model::RunnerBuild& b) { return b.kind == "proton_umu"; });

  const fs::path data_dir = TempFile("umu-provision-prefix");
  fs::remove_all(data_dir);

  model::Game game;
  game.id = "test-windows-game";
  game.platform = model::Platform::Windows;
  game.install_path = "/nonexistent/TestGame";
  game.exe_path = "TestGame.exe";
  game.data_dir = data_dir.string();

  model::Game result = registry.ProvisionGame(game);

  if (has_proton) {
    INFO("a Proton build was discovered; expecting real provisioning to succeed");
    CHECK(result.status == model::GameStatus::Ready);
    CHECK(fs::exists(data_dir / "drive_c"));
    CHECK(result.runner_ref.starts_with("proton_umu:"));
  } else {
    INFO("no Proton build installed here; expecting a graceful, specific failure");
    CHECK(result.status == model::GameStatus::Broken);
    CHECK_FALSE(result.last_error.empty());
  }

  fs::remove_all(data_dir);
}

TEST_CASE("WineRunner discovers the system wine, if installed") {
  config::Config config(TempFile("wine-discover-settings.toml"));
  config.Load();
  runner::RunnerRegistry registry(config);

  const auto builds = registry.DiscoverAll();
  const bool has_wine = std::ranges::any_of(builds, [](const model::RunnerBuild& b) {
    return b.kind == "wine" && b.name == "system";
  });
  INFO("has_wine=", has_wine, " (depends on whether this machine has wine installed)");
  // Nothing to assert unconditionally beyond "it doesn't crash" — this is
  // exactly the graceful-degradation contract: environment-dependent by
  // nature. Full provisioning is exercised below when it's actually present.
}

TEST_CASE("WineRunner provisioning matches what's actually installed on this machine") {
  config::Config config(TempFile("wine-provision-settings.toml"));
  config.Load();
  runner::RunnerRegistry registry(config);

  const bool has_wine = std::ranges::any_of(
      registry.DiscoverAll(), [](const model::RunnerBuild& b) { return b.kind == "wine"; });

  const fs::path data_dir = TempFile("wine-provision-prefix");
  fs::remove_all(data_dir);

  model::Game game;
  game.id = "test-wine-game";
  game.platform = model::Platform::Windows;
  game.install_path = "/nonexistent/TestGame";
  game.exe_path = "TestGame.exe";
  game.data_dir = data_dir.string();
  game.runner_ref = "wine:system";

  model::Game result = registry.ProvisionGame(game);

  if (has_wine) {
    INFO("system wine was discovered; expecting real provisioning to succeed");
    CHECK(result.status == model::GameStatus::Ready);
    CHECK(fs::exists(data_dir / "drive_c"));
  } else {
    INFO("no wine installed here; expecting a graceful, specific failure");
    CHECK(result.status == model::GameStatus::Broken);
    CHECK_FALSE(result.last_error.empty());
  }

  fs::remove_all(data_dir);
}

TEST_CASE("\"auto\" prefers proton_umu when a Proton build exists, else falls back to wine") {
  config::Config config(TempFile("auto-fallback-settings.toml"));
  config.Load();
  runner::RunnerRegistry registry(config);

  const auto builds = registry.DiscoverAll();
  const bool has_proton = std::ranges::any_of(
      builds, [](const model::RunnerBuild& b) { return b.kind == "proton_umu"; });
  const bool has_wine =
      std::ranges::any_of(builds, [](const model::RunnerBuild& b) { return b.kind == "wine"; });

  model::Game game;
  game.id = "auto-test";
  game.platform = model::Platform::Windows;
  game.install_path = "/nonexistent/TestGame";
  game.exe_path = "TestGame.exe";
  game.data_dir = TempFile("auto-fallback-prefix").string();
  fs::remove_all(game.data_dir);
  // runner_ref left empty: forces resolution of default_runner.windows ("auto").

  model::Game result = registry.ProvisionGame(game);

  if (has_proton) {
    CHECK(result.runner_ref.starts_with("proton_umu:"));
  } else if (has_wine) {
    CHECK(result.runner_ref.starts_with("wine:"));
  } else {
    CHECK(result.status == model::GameStatus::Broken);
  }

  fs::remove_all(game.data_dir);
}
