#include <doctest.h>

#include <filesystem>

#include "config/Config.h"
#include "library/PrefixNaming.h"

using namespace mira;
namespace fs = std::filesystem;

namespace {

fs::path TempDir(const char* name) {
  const fs::path dir = fs::temp_directory_path() / "mira-tests" / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

fs::path TempConfigFile(const char* name) {
  const fs::path dir = fs::temp_directory_path() / "mira-tests";
  fs::create_directories(dir);
  const fs::path file = dir / name;
  fs::remove(file);
  return file;
}

}  // namespace

TEST_CASE("PrefixDir names by slugified game name by default") {
  config::Config config(TempConfigFile("prefix-naming-default.toml"));
  config.Load();
  REQUIRE(config.Set("prefix_root", TempDir("prefix-naming-root").string()).has_value());

  model::Game game;
  game.id = "gog-1207660413";
  game.name = "Celeste";
  CHECK(library::PrefixDir(config, game).filename() == "celeste");
}

TEST_CASE("PrefixDir uses the raw id when prefix_naming is \"id\"") {
  config::Config config(TempConfigFile("prefix-naming-id.toml"));
  config.Load();
  const fs::path root = TempDir("prefix-naming-id-root");
  REQUIRE(config.Set("prefix_root", root.string()).has_value());
  REQUIRE(config.Set("prefix_naming", "id").has_value());

  model::Game game;
  game.id = "gog-1207660413";
  game.name = "Celeste";
  CHECK(library::PrefixDir(config, game) == root / "gog-1207660413");
}

TEST_CASE("PrefixDir appends a numeric suffix on a real directory collision") {
  config::Config config(TempConfigFile("prefix-naming-collision.toml"));
  config.Load();
  const fs::path root = TempDir("prefix-naming-collision-root");
  REQUIRE(config.Set("prefix_root", root.string()).has_value());
  fs::create_directories(root / "celeste");

  model::Game game;
  game.id = "gog-1207660413";
  game.name = "Celeste";
  CHECK(library::PrefixDir(config, game).filename() == "celeste-2");
}

TEST_CASE("PrefixDir falls back to strings::Slugify's own \"game\" default for a name with no letters or digits") {
  config::Config config(TempConfigFile("prefix-naming-empty.toml"));
  config.Load();
  const fs::path root = TempDir("prefix-naming-empty-root");
  REQUIRE(config.Set("prefix_root", root.string()).has_value());

  model::Game game;
  game.id = "itch-42";
  game.name = "!!!";
  CHECK(library::PrefixDir(config, game) == root / "game");
}
