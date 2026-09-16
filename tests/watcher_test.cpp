#include <doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "config/Config.h"
#include "library/Watcher.h"
#include "store/GameStore.h"

using namespace mira;
namespace fs = std::filesystem;

namespace {

fs::path TempDir(const char* name) {
  const fs::path dir = fs::temp_directory_path() / "mira-tests" / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

void Touch(const fs::path& path, bool executable = false) {
  fs::create_directories(path.parent_path());
  std::ofstream(path).close();
  if (executable) fs::permissions(path, fs::perms::owner_exec, fs::perm_options::add);
}

// Waits for a "game.added" event, polling the bus rather than sleeping a
// fixed amount — debounce plus the watcher's poll tick make the exact timing
// unpredictable, so this waits for the actual signal instead.
bool WaitForGameAdded(api::EventBus& events, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    for (const model::Event& event : events.Since(0)) {
      if (event.type == "game.added") return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

}  // namespace

TEST_CASE("Watcher picks up a new game folder in each of two watched roots") {
  const fs::path games_root = TempDir("watch-games");
  const fs::path apps_root = TempDir("watch-apps");
  const fs::path state = TempDir("watch-state");

  config::Config config(state / "settings.toml");
  config.Load();
  REQUIRE(config.Set("library_roots", nlohmann::json::array({games_root.string(), apps_root.string()}))
              .has_value());
  REQUIRE(config.Set("prefix_root", (games_root / "prefix").string()).has_value());
  REQUIRE(config.Set("scan.debounce_ms", 100).has_value());  // fast, this is a test

  store::GameStore games(state / "games.toml");
  games.Load();
  api::EventBus events;
  library::Watcher watcher(config, games, events);

  std::thread watcher_thread([&] { watcher.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));  // let the watches register

  fs::create_directories(games_root / "Celeste");
  Touch(games_root / "Celeste" / "Celeste.exe");
  fs::create_directories(apps_root / "Hollow Knight");
  Touch(apps_root / "Hollow Knight" / "hollow_knight", /*executable=*/true);

  CHECK(WaitForGameAdded(events, std::chrono::seconds(5)));
  // Both settle around the same debounce window; give the second a moment
  // too rather than assuming the first WaitForGameAdded covered both.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  watcher.Stop();
  watcher_thread.join();

  auto celeste = games.Find("celeste");
  REQUIRE(celeste.has_value());
  // Celeste.exe is a Windows executable, so it waits at setting_up for the
  // (not yet built) runner layer to provision it — only native games reach
  // ready with no further work needed.
  CHECK(celeste->status == model::GameStatus::SettingUp);
  CHECK(celeste->platform == model::Platform::Windows);
  CHECK(celeste->install_path == (games_root / "Celeste").string());

  auto hollow_knight = games.Find("hollow-knight");
  REQUIRE(hollow_knight.has_value());
  CHECK(hollow_knight->platform == model::Platform::Native);
  CHECK(hollow_knight->install_path == (apps_root / "Hollow Knight").string());
}

TEST_CASE("Watcher never rediscovers its own prefix directory as a game") {
  const fs::path root = TempDir("watch-prefix-regression");
  const fs::path state = TempDir("watch-prefix-state");

  config::Config config(state / "settings.toml");
  config.Load();
  REQUIRE(config.Set("library_roots", nlohmann::json::array({root.string()})).has_value());
  REQUIRE(config.Set("prefix_root", (root / "prefix").string()).has_value());
  REQUIRE(config.Set("scan.debounce_ms", 100).has_value());

  store::GameStore games(state / "games.toml");
  games.Load();
  api::EventBus events;
  library::Watcher watcher(config, games, events);

  std::thread watcher_thread([&] { watcher.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  fs::create_directories(root / "prefix" / "celeste" / "drive_c");
  Touch(root / "prefix" / "celeste" / "system.reg");
  std::this_thread::sleep_for(std::chrono::milliseconds(800));  // past debounce, nothing should fire

  watcher.Stop();
  watcher_thread.join();

  CHECK(games.All().empty());
  CHECK(events.Since(0).empty());
}
