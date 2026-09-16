#include <doctest.h>

#include <filesystem>
#include <fstream>

#include "config/Config.h"
#include "library/AutoSetup.h"
#include "library/Detector.h"
#include "library/Scanner.h"
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

library::DetectorSettings DefaultSettings() {
  library::DetectorSettings settings;
  settings.deny_name_patterns = {"unins*", "vcredist*", "*crashreport*"};
  settings.ignore_globs = {".*", "*/Redist*"};
  return settings;
}

}  // namespace

TEST_CASE("Detector picks the exe matching the folder name over an installer") {
  const fs::path dir = TempDir("celeste-detect");
  Touch(dir / "Celeste.exe");
  Touch(dir / "unins000.exe");

  const library::Detector detector(DefaultSettings());
  auto result = detector.Detect(dir);

  REQUIRE(result.candidates.size() == 2);
  CHECK(result.candidates[0].rel_path == "Celeste.exe");
  CHECK(result.candidates[0].chosen);
  CHECK(result.confidence > 0.5);
}

TEST_CASE("Detector prefers the shallower, name-matching exe in a nested folder") {
  const fs::path dir = TempDir("hollow-knight-detect");
  Touch(dir / "Content" / "unrelated_tool.exe");
  Touch(dir / "hollow_knight.exe");

  const library::Detector detector(DefaultSettings());
  auto result = detector.Detect(dir);

  REQUIRE_FALSE(result.candidates.empty());
  CHECK(result.candidates[0].rel_path == "hollow_knight.exe");
}

TEST_CASE("Detector finds a native ELF binary via its execute bit, ignoring redist noise") {
  const fs::path dir = TempDir("celeste-native-detect");
  Touch(dir / "Celeste", /*executable=*/true);
  Touch(dir / "Redist" / "somelib.so");  // matches the */Redist* ignore glob

  const library::Detector detector(DefaultSettings());
  auto result = detector.Detect(dir);

  REQUIRE(result.candidates.size() == 1);
  CHECK(result.candidates[0].rel_path == "Celeste");
  CHECK(result.candidates[0].kind == model::Platform::Native);
}

TEST_CASE("Detector returns no candidates and low confidence for an empty folder") {
  const fs::path dir = TempDir("empty-detect");
  const library::Detector detector(DefaultSettings());
  auto result = detector.Detect(dir);
  CHECK(result.candidates.empty());
  CHECK(result.confidence == doctest::Approx(0.0));
}

TEST_CASE("AutoSetup stores a native game as ready and a windows game as setting_up") {
  const fs::path dir = TempDir("autosetup-config");
  config::Config config(dir / "settings.toml");
  config.Load();
  store::GameStore games(dir / "games.toml");
  games.Load();
  api::EventBus events;
  library::AutoSetup auto_setup(config, games, events);

  library::Detector::Result native_result;
  native_result.candidates.push_back({"Celeste", model::Platform::Native, 4.5, true});
  native_result.confidence = 0.9;
  model::Game native_game = auto_setup.CreateGame("/games/Celeste", native_result);
  CHECK(native_game.status == model::GameStatus::Ready);
  CHECK(native_game.id == "celeste");

  library::Detector::Result windows_result;
  windows_result.candidates.push_back(
      {"hollow_knight.exe", model::Platform::Windows, 4.5, true});
  windows_result.confidence = 0.8;
  model::Game windows_game =
      auto_setup.CreateGame("/games/Hollow Knight", windows_result);
  CHECK(windows_game.status == model::GameStatus::SettingUp);
  CHECK_FALSE(windows_game.data_dir.empty());

  auto stream = events.Since(0);
  REQUIRE(stream.size() == 2);
  CHECK(stream[0].type == "game.added");
  CHECK(stream[0].payload.value("open_config", false) == true);
}

TEST_CASE("Scanner adds new games, skips known ones, and marks missing folders") {
  const fs::path lib = TempDir("scan-library");
  fs::create_directories(lib / "Celeste");
  Touch(lib / "Celeste" / "Celeste.exe");

  const fs::path state = TempDir("scan-state");
  config::Config config(state / "settings.toml");
  config.Load();
  REQUIRE(config.Set("library_roots", nlohmann::json::array({lib.string()})).has_value());
  REQUIRE(config.Set("prefix_root", (lib / "prefix").string()).has_value());

  store::GameStore games(state / "games.toml");
  games.Load();
  api::EventBus events;
  library::Scanner scanner(config, games, events);

  // A prefix directory sitting inside the library root must never be
  // rediscovered as a game — this is the regression the design specifically
  // guards against (see docs/architecture.md).
  fs::create_directories(lib / "prefix" / "celeste" / "drive_c");
  Touch(lib / "prefix" / "celeste" / "system.reg");

  library::ScanSummary first = scanner.ScanAll();
  CHECK(first.added == 1);
  CHECK(games.All().size() == 1);
  CHECK(games.Find("celeste").has_value());

  library::ScanSummary second = scanner.ScanAll();
  CHECK(second.added == 0);  // already known; must not be re-detected
  CHECK(games.All().size() == 1);

  fs::remove_all(lib / "Celeste");
  library::ScanSummary third = scanner.ScanAll();
  CHECK(third.missing == 1);
  CHECK(games.Find("celeste")->status == model::GameStatus::Missing);

  fs::create_directories(lib / "Celeste");
  Touch(lib / "Celeste" / "Celeste.exe");
  library::ScanSummary fourth = scanner.ScanAll();
  CHECK(fourth.restored == 1);
  CHECK(games.Find("celeste")->status == model::GameStatus::Ready);
}
