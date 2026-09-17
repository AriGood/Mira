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

  // Both native: a .exe here would trigger a real, multi-second umu-run
  // provisioning attempt as a side effect (see tests/runner_test.cpp for
  // that, deliberately isolated) — this test's job is proving two
  // independently-watched roots both get picked up, not provisioning.
  fs::create_directories(games_root / "Celeste");
  Touch(games_root / "Celeste" / "Celeste", /*executable=*/true);
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
  CHECK(celeste->status == model::GameStatus::Ready);
  CHECK(celeste->platform == model::Platform::Native);
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

TEST_CASE("Watcher auto-extracts a dropped archive and picks up the resulting folder as a game") {
  const fs::path root = TempDir("watch-archive-root");
  const fs::path state = TempDir("watch-archive-state");

  config::Config config(state / "settings.toml");
  config.Load();
  REQUIRE(config.Set("library_roots", nlohmann::json::array({root.string()})).has_value());
  REQUIRE(config.Set("prefix_root", (root / "prefix").string()).has_value());
  REQUIRE(config.Set("scan.debounce_ms", 100).has_value());
  REQUIRE(config.Set("scan.auto_extract_archives", true).has_value());

  store::GameStore games(state / "games.toml");
  games.Load();
  api::EventBus events;
  library::Watcher watcher(config, games, events);

  std::thread watcher_thread([&] { watcher.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Build a real tar.gz containing a native game folder's shape, and drop
  // it directly into the watched root, exactly like a user extracting a
  // download by hand — except here Mira does it.
  const fs::path staging = TempDir("watch-archive-staging");
  fs::create_directories(staging / "Celeste");
  Touch(staging / "Celeste" / "Celeste", /*executable=*/true);
  const fs::path archive = root / "Celeste.tar.gz";
  REQUIRE(std::system(("tar -C " + staging.string() + " -czf " + archive.string() + " Celeste").c_str()) == 0);

  CHECK(WaitForGameAdded(events, std::chrono::seconds(5)));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  watcher.Stop();
  watcher_thread.join();

  CHECK_FALSE(fs::exists(archive));  // the archive itself is gone
  CHECK(fs::exists(root / "Celeste" / "Celeste"));

  auto celeste = games.Find("celeste");
  REQUIRE(celeste.has_value());
  CHECK(celeste->platform == model::Platform::Native);
  CHECK(celeste->install_path == (root / "Celeste").string());
}

TEST_CASE("Watcher leaves a dropped archive alone when auto_extract_archives is off") {
  const fs::path root = TempDir("watch-archive-off-root");
  const fs::path state = TempDir("watch-archive-off-state");

  config::Config config(state / "settings.toml");
  config.Load();
  REQUIRE(config.Set("library_roots", nlohmann::json::array({root.string()})).has_value());
  REQUIRE(config.Set("prefix_root", (root / "prefix").string()).has_value());
  REQUIRE(config.Set("scan.debounce_ms", 100).has_value());
  // scan.auto_extract_archives left at its default: false.

  store::GameStore games(state / "games.toml");
  games.Load();
  api::EventBus events;
  library::Watcher watcher(config, games, events);

  std::thread watcher_thread([&] { watcher.Run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  const fs::path staging = TempDir("watch-archive-off-staging");
  fs::create_directories(staging / "Celeste");
  Touch(staging / "Celeste" / "Celeste", /*executable=*/true);
  const fs::path archive = root / "Celeste.tar.gz";
  REQUIRE(std::system(("tar -C " + staging.string() + " -czf " + archive.string() + " Celeste").c_str()) == 0);

  std::this_thread::sleep_for(std::chrono::milliseconds(800));  // past debounce, nothing should fire

  watcher.Stop();
  watcher_thread.join();

  CHECK(fs::exists(archive));  // untouched
  CHECK(games.All().empty());
}
