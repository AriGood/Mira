#include <doctest.h>

#include <fstream>
#include <filesystem>

#include "store/GameStore.h"

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

model::Game MakeGame(const std::string& id, const std::string& name) {
  model::Game game;
  game.id = id;
  game.name = name;
  game.install_path = "/games/" + id;
  game.status = model::GameStatus::Ready;
  return game;
}
}  // namespace

TEST_CASE("GameStore starts empty when no file exists") {
  store::GameStore store(TempFile("games-empty.toml"));
  store.Load();
  CHECK(store.All().empty());
}

TEST_CASE("NextId disambiguates collisions so ids stay readable") {
  store::GameStore store(TempFile("games-ids.toml"));
  store.Load();
  CHECK(store.NextId("Celeste") == "celeste");
  REQUIRE(store.Upsert(MakeGame("celeste", "Celeste")).has_value());
  CHECK(store.NextId("Celeste") == "celeste-2");
}

TEST_CASE("games round-trip through games.toml, including nested fields") {
  const fs::path file = TempFile("games-roundtrip.toml");
  store::GameStore store(file);
  store.Load();

  model::Game game = MakeGame("celeste", "Celeste");
  game.candidates.push_back({"Celeste.exe", model::Platform::Windows, 4.5, true});
  game.env["FOO"] = "bar";
  game.overrides["scan.max_depth"] = 8;
  REQUIRE(store.Upsert(game).has_value());

  store::GameStore reloaded(file);
  reloaded.Load();
  auto found = reloaded.Find("celeste");
  REQUIRE(found.has_value());
  CHECK(found->name == "Celeste");
  CHECK(found->candidates.size() == 1);
  CHECK(found->candidates[0].chosen);
  CHECK(found->env.at("FOO") == "bar");
  CHECK(found->overrides.value("scan.max_depth", 0) == 8);
}

TEST_CASE("Update mutates under lock and reports the saved result") {
  store::GameStore store(TempFile("games-update.toml"));
  store.Load();
  REQUIRE(store.Upsert(MakeGame("celeste", "Celeste")).has_value());

  auto result = store.Update("celeste", [](model::Game& game) {
    game.reviewed = true;
    game.play_seconds += 60;
  });
  REQUIRE(result.has_value());
  CHECK(result->reviewed);
  CHECK(result->play_seconds == 60);

  auto missing = store.Update("no-such-id", [](model::Game&) {});
  CHECK_FALSE(missing.has_value());
}

TEST_CASE("Remove deletes a game and reports an error for an unknown id") {
  store::GameStore store(TempFile("games-remove.toml"));
  store.Load();
  REQUIRE(store.Upsert(MakeGame("celeste", "Celeste")).has_value());
  CHECK(store.Remove("celeste").has_value());
  CHECK_FALSE(store.Find("celeste").has_value());
  CHECK_FALSE(store.Remove("celeste").has_value());
}

TEST_CASE("a corrupt games.toml is quarantined and the library starts empty") {
  const fs::path file = TempFile("games-corrupt.toml");
  {
    std::ofstream out(file);
    out << "not [ valid toml at all";
  }
  store::GameStore store(file);
  store.Load();  // must not throw
  CHECK(store.All().empty());
  CHECK(fs::exists(file.string() + ".bad"));
  fs::remove(file.string() + ".bad");
}
