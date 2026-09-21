#include <doctest.h>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>

#include <json.hpp>

#include "api/EventBus.h"
#include "config/Config.h"
#include "desktop/DesktopEntryScanner.h"
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

void WriteFile(const fs::path& path, std::string_view contents) {
  fs::create_directories(path.parent_path());
  std::ofstream(path) << contents;
}

// The scanner also walks $XDG_DATA_HOME/applications, $XDG_DATA_DIRS, and
// the two well-known Flatpak export dirs unconditionally -- on a real
// desktop machine those pick up real installed apps, which would make a
// test's expected candidate count depend on whatever happens to be
// installed. Point both env vars at empty, otherwise-unused directories so
// only this fixture's extra_dirs entry is ever actually scanned.
struct Fixture {
  fs::path apps_dir;
  fs::path state_dir;
  fs::path empty_data_home;
  config::Config config;
  store::GameStore games;
  api::EventBus events;

  explicit Fixture(const char* name)
      : apps_dir(TempDir((std::string(name) + "-apps").c_str())),
        state_dir(TempDir((std::string(name) + "-state").c_str())),
        empty_data_home(TempDir((std::string(name) + "-empty-data-home").c_str())),
        config(state_dir / "settings.toml"),
        games(state_dir / "games.toml") {
    setenv("XDG_DATA_HOME", empty_data_home.string().c_str(), 1);
    setenv("XDG_DATA_DIRS", empty_data_home.string().c_str(), 1);
    config.Load();
    REQUIRE(config.Set("desktop_import.extra_dirs", nlohmann::json::array({apps_dir.string()})).has_value());
    games.Load();
  }
};

}  // namespace

TEST_CASE("DesktopEntryScanner: ListCandidates finds a Flatpak entry and a plain native entry, "
          "skips Mira's own, Steam's, NoDisplay, and non-Application entries") {
  Fixture fx("desktop-scan-candidates");

  WriteFile(fx.apps_dir / "com.example.App.desktop",
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=Example App\n"
            "Icon=com.example.App\n"
            "Exec=flatpak run --branch=stable --command=example com.example.App @@u %u @@\n"
            "X-Flatpak=com.example.App\n");

  WriteFile(fx.apps_dir / "native-game.desktop",
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=Native Game\n"
            "Exec=\"/opt/nativegame/game\" --fullscreen\n");

  WriteFile(fx.apps_dir / "mira-celeste.desktop",
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=Celeste\n"
            "Exec=mira launch celeste\n"
            "X-Mira-Game-Id=celeste\n");

  WriteFile(fx.apps_dir / "steam-game.desktop",
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=Some Steam Game\n"
            "Exec=steam steam://rungameid/12345\n");

  WriteFile(fx.apps_dir / "hidden.desktop",
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=Hidden Thing\n"
            "Exec=/usr/bin/hiddenthing\n"
            "NoDisplay=true\n");

  WriteFile(fx.apps_dir / "not-an-app.desktop",
            "[Desktop Entry]\n"
            "Type=Link\n"
            "Name=A Link\n"
            "URL=https://example.com\n");

  desktop::DesktopEntryScanner scanner(fx.config, fx.games, fx.events);
  auto candidates = scanner.ListCandidates();
  REQUIRE(candidates.has_value());

  // Not an exact-size assertion: /var/lib/flatpak/exports is scanned
  // unconditionally (by design, for production reliability), so a real
  // machine's own installed Flatpak apps may legitimately add candidates
  // beyond this fixture's own two.
  bool found_flatpak = false, found_native = false;
  for (const auto& c : *candidates) {
    CHECK(c.id != "celeste");            // Mira's own entry, never a candidate
    CHECK(c.name != "Some Steam Game");  // Steam-style Exec=, covered by SteamScanner
    CHECK(c.name != "Hidden Thing");     // NoDisplay=true
    CHECK(c.name != "A Link");           // Type=Link, not Application
    if (c.id == "com.example.App") {
      found_flatpak = true;
      CHECK(c.name == "Example App");
      CHECK(c.icon == "com.example.App");
    }
    if (c.id == "native-game") {
      found_native = true;
      CHECK(c.name == "Native Game");
    }
  }
  CHECK(found_flatpak);
  CHECK(found_native);
}

TEST_CASE("DesktopEntryScanner: Import adds a Flatpak-style entry with the run-<app-id> convention") {
  Fixture fx("desktop-scan-import-flatpak");

  WriteFile(fx.apps_dir / "com.example.App.desktop",
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=Example App\n"
            "Exec=flatpak run com.example.App\n"
            "X-Flatpak=com.example.App\n");

  desktop::DesktopEntryScanner scanner(fx.config, fx.games, fx.events);
  auto summary = scanner.Import({"com.example.App"});
  REQUIRE(summary.has_value());
  CHECK(summary->added == 1);
  REQUIRE(summary->added_games.size() == 1);

  const model::Game& game = summary->added_games[0];
  CHECK(game.name == "Example App");
  CHECK(game.exe_path == "flatpak");
  CHECK(game.args == "run com.example.App");
  CHECK(game.platform == model::Platform::Native);
  CHECK(game.install_path.ends_with(".var/app/com.example.App"));
  CHECK(game.status == model::GameStatus::Ready);
}

TEST_CASE("DesktopEntryScanner: Import adds a plain native entry, and re-importing updates rather than duplicates") {
  Fixture fx("desktop-scan-import-native");
  fs::create_directories(fx.apps_dir.parent_path() / "opt" / "nativegame");

  const fs::path exe_dir = TempDir("desktop-scan-import-native-exe");
  WriteFile(fx.apps_dir / "native-game.desktop", std::format("[Desktop Entry]\n"
                                                              "Type=Application\n"
                                                              "Name=Native Game\n"
                                                              "Exec=\"{}/game\" --fullscreen\n",
                                                              exe_dir.string()));

  desktop::DesktopEntryScanner scanner(fx.config, fx.games, fx.events);
  auto first = scanner.Import({"native-game"});
  REQUIRE(first.has_value());
  CHECK(first->added == 1);
  REQUIRE(first->added_games.size() == 1);
  CHECK(first->added_games[0].exe_path == "game");
  CHECK(first->added_games[0].args == "--fullscreen");
  CHECK(first->added_games[0].install_path == exe_dir.string());

  auto second = scanner.Import({"native-game"});
  REQUIRE(second.has_value());
  CHECK(second->added == 0);
  CHECK(second->updated == 1);
  CHECK(fx.games.All().size() == 1);
}
