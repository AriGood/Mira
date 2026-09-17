#include <doctest.h>

#include <filesystem>

#include "config/Config.h"
#include "desktop/DesktopEntries.h"

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

TEST_CASE("DesktopEntries never writes an entry for a Steam-sourced game -- Steam already has one") {
  const fs::path state = TempDir("desktop-entries-steam-state");
  const fs::path applications = TempDir("desktop-entries-steam-apps");

  config::Config config(state / "settings.toml");
  config.Load();
  REQUIRE(config.Set("desktop_entries.enabled", true).has_value());
  // Point it at a scratch dir rather than the real
  // ~/.local/share/applications.
  REQUIRE(config.Set("desktop_entries.directory", applications.string()).has_value());

  model::Game steam_game;
  steam_game.id = "some-steam-game";
  steam_game.name = "Some Steam Game";
  steam_game.status = model::GameStatus::Ready;
  steam_game.runner_ref = "steam:504230";
  // Deliberately no exe_path -- a Steam game in "steam" launch mode never
  // has one, and used to still get an entry regardless (see IsLaunchable).

  model::Game native_game;
  native_game.id = "native-game";
  native_game.name = "Native Game";
  native_game.status = model::GameStatus::Ready;
  native_game.exe_path = "game";

  desktop::DesktopEntries entries(config);
  REQUIRE(entries.Sync({steam_game, native_game}).has_value());

  CHECK_FALSE(fs::exists(applications / "mira-some-steam-game.desktop"));
  CHECK(fs::exists(applications / "mira-native-game.desktop"));
}
