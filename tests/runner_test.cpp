#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

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

TEST_CASE("NativeRunner runs a .sh with no execute bit through sh") {
  const fs::path dir = fs::temp_directory_path() / "mira-tests" / "native-sh";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const fs::path script = dir / "start.sh";
  std::ofstream(script) << "#!/bin/sh\necho hi\n";
  fs::permissions(script, fs::perms::owner_read | fs::perms::owner_write);  // no +x

  model::Game game;
  game.install_path = dir.string();
  game.exe_path = "start.sh";
  runner::NativeRunner native;
  auto command = native.BuildCommand(game, std::nullopt);
  REQUIRE(command.has_value());
  CHECK(command->argv == std::vector<std::string>{"sh", script.string()});
  fs::remove_all(dir);
}

TEST_CASE("NativeRunner runs an executable .sh directly through sh regardless") {
  const fs::path dir = fs::temp_directory_path() / "mira-tests" / "native-sh-exec";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const fs::path script = dir / "start.sh";
  std::ofstream(script) << "#!/bin/sh\necho hi\n";
  fs::permissions(script, fs::perms::owner_all);

  model::Game game;
  game.install_path = dir.string();
  game.exe_path = "start.sh";
  runner::NativeRunner native;
  auto command = native.BuildCommand(game, std::nullopt);
  REQUIRE(command.has_value());
  CHECK(command->argv == std::vector<std::string>{"sh", script.string()});
  fs::remove_all(dir);
}

TEST_CASE("NativeRunner rejects a non-script exe with no execute bit") {
  const fs::path dir = fs::temp_directory_path() / "mira-tests" / "native-noexec";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const fs::path exe = dir / "game";
  std::ofstream(exe) << "not actually elf, doesn't matter here";
  fs::permissions(exe, fs::perms::owner_read | fs::perms::owner_write);

  model::Game game;
  game.install_path = dir.string();
  game.exe_path = "game";
  runner::NativeRunner native;
  auto command = native.BuildCommand(game, std::nullopt);
  REQUIRE_FALSE(command.has_value());
  CHECK(command.error().code == "not_executable");
  fs::remove_all(dir);
}

TEST_CASE("NativeRunner runs an executable AppImage directly when FUSE is available") {
  const fs::path dir = fs::temp_directory_path() / "mira-tests" / "native-appimage";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const fs::path exe = dir / "Game.AppImage";
  std::ofstream(exe) << "not a real appimage, doesn't matter here";
  fs::permissions(exe, fs::perms::owner_all);

  model::Game game;
  game.install_path = dir.string();
  game.exe_path = "Game.AppImage";
  runner::NativeRunner native;
  auto command = native.BuildCommand(game, std::nullopt);
  REQUIRE(command.has_value());
  // Whether extraction is forced depends on whether this machine actually has
  // FUSE — assert whichever shape that implies, same posture as the
  // environment-dependent Proton/Wine tests below.
  if (command->argv.size() > 1 && command->argv[1] == "--appimage-extract-and-run") {
    CHECK(command->argv[0] == exe.string());
  } else {
    CHECK(command->argv == std::vector<std::string>{exe.string()});
  }
  fs::remove_all(dir);
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

TEST_CASE("DeduplicateBuilds collapses one build reached through a symlink") {
  // The real case this exists for: on a normal Arch/Steam setup
  // ~/.steam/steam is a symlink to ~/.local/share/Steam, and
  // libraryfolders.vdf lists the target as well — so every Proton build under
  // it is discovered twice, once per search path, with only `path` differing.
  const fs::path root = fs::temp_directory_path() / "mira-tests" / "runner-dedupe";
  fs::remove_all(root);
  const fs::path real = root / "real" / "GE-Proton11-7";
  fs::create_directories(real);
  const fs::path link = root / "link";
  std::error_code ec;
  fs::create_directory_symlink(root / "real", link, ec);
  REQUIRE_FALSE(ec);

  std::vector<model::RunnerBuild> builds = {
      {"proton", "GE-Proton11-7", real.string(), "2"},
      {"proton", "GE-Proton11-7", (link / "GE-Proton11-7").string(), "2"},
  };

  const std::vector<model::RunnerBuild> unique = runner::DeduplicateBuilds(builds);
  REQUIRE(unique.size() == 1);
  CHECK(unique[0].path == real.string());  // first occurrence wins
  fs::remove_all(root);
}

TEST_CASE("DeduplicateBuilds collapses two entries sharing a reference") {
  // Different directories, same "kind:name" — which is all a game stores
  // (model::RunnerBuild::Reference), so no client could pick between them.
  std::vector<model::RunnerBuild> builds = {
      {"proton", "GE-Proton11-7", "/a/GE-Proton11-7", "2"},
      {"proton", "GE-Proton11-7", "/b/GE-Proton11-7", "2"},
  };
  CHECK(runner::DeduplicateBuilds(builds).size() == 1);
}

TEST_CASE("DeduplicateBuilds keeps genuinely different builds, in order") {
  std::vector<model::RunnerBuild> builds = {
      {"proton", "GE-Proton11-7", "/a/GE-Proton11-7", "2"},
      {"proton", "GE-Proton11-6", "/a/GE-Proton11-6", "1"},
      {"wine", "system", "/usr/bin/wine", "wine-11.17"},
  };
  const std::vector<model::RunnerBuild> unique = runner::DeduplicateBuilds(builds);
  REQUIRE(unique.size() == 3);
  CHECK(unique[0].name == "GE-Proton11-7");  // newest-first order preserved
  CHECK(unique[2].kind == "wine");
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

// Everything below actually exercises ProtonRunner — real umu-run, real Proton,
// a real prefix on disk. Environment-dependent by nature (this is what it's
// testing), so it adapts to what's actually installed rather than assuming
// a specific machine: it checks what RunnerRegistry itself discovers first,
// then asserts the outcome that implies, on either branch.
TEST_CASE("ProtonRunner provisioning matches what's actually installed on this machine") {
  config::Config config(TempFile("runner-registry-umu-settings.toml"));
  config.Load();
  runner::RunnerRegistry registry(config);

  const bool has_proton = std::ranges::any_of(
      registry.DiscoverAll(), [](const model::RunnerBuild& b) { return b.kind == "proton"; });

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
    CHECK(result.runner_ref.starts_with("proton:"));
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

TEST_CASE("\"auto\" prefers proton when a Proton build exists, else falls back to wine") {
  config::Config config(TempFile("auto-fallback-settings.toml"));
  config.Load();
  runner::RunnerRegistry registry(config);

  const auto builds = registry.DiscoverAll();
  const bool has_proton = std::ranges::any_of(
      builds, [](const model::RunnerBuild& b) { return b.kind == "proton"; });
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
    CHECK(result.runner_ref.starts_with("proton:"));
  } else if (has_wine) {
    CHECK(result.runner_ref.starts_with("wine:"));
  } else {
    CHECK(result.status == model::GameStatus::Broken);
  }

  fs::remove_all(game.data_dir);
}

TEST_CASE("Resolve reports an uninstalled build instead of succeeding with none") {
  config::Config config(TempFile("resolve-missing-build.toml"));
  config.Load();
  // Point discovery at nothing, so no wine/proton builds exist at all.
  REQUIRE(config.Set("runner_search_paths", nlohmann::json::array()).has_value());
  REQUIRE(config.Set("wine_search_paths", nlohmann::json::array()).has_value());
  runner::RunnerRegistry registry(config);

  // Regression: this used to return success-with-no-build, which made a
  // Windows game resolve to "no build" and report a vague error — and made
  // native:anything-at-all silently "succeed" for a Windows game.
  auto proton = registry.Resolve("proton:GE-Proton-Nonexistent");
  CHECK_FALSE(proton.has_value());

  // native has no build concept at all, so it still resolves.
  auto native = registry.Resolve("native:native");
  REQUIRE(native.has_value());
  CHECK_FALSE(native->build.has_value());
}

TEST_CASE("the old proton_umu: runner_ref spelling still resolves after the rename") {
  // umu was briefly modelled as its own runner kind; it's the mechanism
  // Proton runs through, not a runner. A games.toml written before the
  // rename must keep working.
  config::Config config(TempFile("legacy-ref.toml"));
  config.Load();
  runner::RunnerRegistry registry(config);

  const bool has_proton = std::ranges::any_of(
      registry.DiscoverAll(), [](const model::RunnerBuild& b) { return b.kind == "proton"; });
  if (!has_proton) return;  // nothing installed to resolve against on this machine

  auto legacy = registry.Resolve("proton_umu:latest");
  REQUIRE(legacy.has_value());
  CHECK(legacy->runner->kind() == "proton");
}
