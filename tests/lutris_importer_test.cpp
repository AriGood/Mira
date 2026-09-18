#include <doctest.h>

#include <filesystem>
#include <fstream>

#include "config/Config.h"
#include "lutris/LutrisImporter.h"
#include "runner/Exec.h"
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

// Every LutrisRow with runner != 'wine' is inserted too, to exercise the
// skip path, matching a real pga.db (Steam-runner rows sit alongside
// wine-runner ones there).
struct FixtureRow {
  std::string name;
  std::string slug;
  std::string runner;
  std::string configpath;
};

Result<void> BuildFixtureDb(const fs::path& db_path, const std::vector<FixtureRow>& rows) {
  std::string sql = "CREATE TABLE games (id INTEGER PRIMARY KEY, name TEXT, slug TEXT, runner TEXT, "
                    "configpath TEXT);";
  for (const FixtureRow& row : rows) {
    sql += std::format("INSERT INTO games (name, slug, runner, configpath) VALUES ('{}', '{}', '{}', '{}');",
                       row.name, row.slug, row.runner, row.configpath);
  }
  const auto sqlite3 = runner::FindOnPath("sqlite3");
  if (!sqlite3) return Err("sqlite3_missing", "sqlite3 not found");
  Command command;
  command.argv = {*sqlite3, db_path.string(), sql};
  const Result<runner::ExecResult> result = runner::RunAndWait(command);
  if (!result || result->exit_code != 0) {
    return Err("fixture_db_failed", !result ? result.error().message : result->output);
  }
  return {};
}

struct Fixture {
  fs::path lutris_dir;
  fs::path state_dir;
  config::Config config;
  store::GameStore games;
  api::EventBus events;

  explicit Fixture(const char* name)
      : lutris_dir(TempDir((std::string(name) + "-lutris").c_str())),
        state_dir(TempDir((std::string(name) + "-state").c_str())),
        config(state_dir / "settings.toml"),
        games(state_dir / "games.toml") {
    config.Load();
    REQUIRE(config.Set("lutris.data_dir", lutris_dir.string()).has_value());
    games.Load();
    fs::create_directories(lutris_dir / "games");
  }
};

}  // namespace

TEST_CASE("LutrisImporter derives install_path from the exe's own directory, data_dir verbatim from prefix") {
  if (!runner::FindOnPath("sqlite3")) return;  // soft dependency, same posture as winetricks

  Fixture fx("lutris-combined");
  const fs::path game_dir = fx.lutris_dir.parent_path() / "batman";
  fs::create_directories(game_dir / "Binaries");

  REQUIRE(BuildFixtureDb(fx.lutris_dir / "pga.db",
                        {{"Batman: Arkham Asylum", "batman-arkham-asylum", "wine", "batman-123"}})
             .has_value());
  WriteFile(fx.lutris_dir / "games" / "batman-123.yml", std::format(R"(game:
  exe: {}/Binaries/BmLauncher.exe
  prefix: {}
)",
                                                                    game_dir.string(), game_dir.string()));

  lutris::LutrisImporter importer(fx.config, fx.games, fx.events);
  const auto summary = importer.Import();
  REQUIRE(summary.has_value());
  CHECK(summary->added == 1);
  CHECK(summary->skipped == 0);

  const auto game = fx.games.Find("batman-arkham-asylum");
  REQUIRE(game.has_value());
  CHECK(game->install_path == (game_dir / "Binaries").string());
  CHECK(game->data_dir == game_dir.string());
  CHECK(game->exe_path == "BmLauncher.exe");
  CHECK(game->status == model::GameStatus::Ready);
}

TEST_CASE("LutrisImporter keeps a relative exe relative to prefix, per the yaml itself") {
  if (!runner::FindOnPath("sqlite3")) return;

  Fixture fx("lutris-relative-exe");
  const fs::path prefix_dir = fx.lutris_dir.parent_path() / "epic-games-store";
  fs::create_directories(prefix_dir / "drive_c");

  REQUIRE(BuildFixtureDb(fx.lutris_dir / "pga.db", {{"Epic Games Store", "epic-games-store", "wine", "egs-1"}})
             .has_value());
  WriteFile(fx.lutris_dir / "games" / "egs-1.yml", std::format(R"(game:
  exe: drive_c/EpicGamesLauncher.exe
  prefix: {}
)",
                                                               prefix_dir.string()));

  lutris::LutrisImporter importer(fx.config, fx.games, fx.events);
  const auto summary = importer.Import();
  REQUIRE(summary.has_value());
  CHECK(summary->added == 1);

  const auto game = fx.games.Find("epic-games-store");
  REQUIRE(game.has_value());
  CHECK(game->install_path == (prefix_dir / "drive_c").string());
  CHECK(game->data_dir == prefix_dir.string());
  CHECK(game->exe_path == "EpicGamesLauncher.exe");
}

TEST_CASE("LutrisImporter skips a row with no prefix recorded in its yaml") {
  if (!runner::FindOnPath("sqlite3")) return;

  Fixture fx("lutris-no-prefix");
  REQUIRE(BuildFixtureDb(fx.lutris_dir / "pga.db", {{"Celeste", "celeste", "wine", "celeste-456"}}).has_value());
  WriteFile(fx.lutris_dir / "games" / "celeste-456.yml", "game:\n  exe: /home/exo/Games/Celeste/Celeste.exe\n");

  lutris::LutrisImporter importer(fx.config, fx.games, fx.events);
  const auto summary = importer.Import();
  REQUIRE(summary.has_value());
  CHECK(summary->added == 0);
  CHECK(summary->skipped == 1);
}

TEST_CASE("LutrisImporter skips non-wine runners and updates known games in place") {
  if (!runner::FindOnPath("sqlite3")) return;

  Fixture fx("lutris-skip-and-update");
  const fs::path game_dir = fx.lutris_dir.parent_path() / "Celeste";
  fs::create_directories(game_dir);

  REQUIRE(BuildFixtureDb(fx.lutris_dir / "pga.db",
                        {{"Celeste", "celeste", "wine", "celeste-1"},
                         {"Half-Life", "half-life", "steam", "half-life-1"}})
             .has_value());
  WriteFile(fx.lutris_dir / "games" / "celeste-1.yml", std::format(R"(game:
  exe: {}/Celeste.exe
  prefix: {}
)",
                                                                   game_dir.string(), game_dir.string()));
  // The steam-runner row has no yaml fixture at all — ReadGameConfig fails
  // to open it, which is exactly what "not a wine game we handle" looks
  // like in a real pga.db too (Steam rows aren't given a Lutris yaml).

  lutris::LutrisImporter importer(fx.config, fx.games, fx.events);
  const auto first = importer.Import();
  REQUIRE(first.has_value());
  CHECK(first->added == 1);
  CHECK(first->skipped == 1);

  const auto second = importer.Import();
  REQUIRE(second.has_value());
  CHECK(second->added == 0);
  CHECK(second->updated == 1);
  CHECK(fx.games.All().size() == 1);
}
