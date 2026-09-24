#include <doctest.h>

#include <filesystem>

#include "config/Config.h"
#include "library/GamesFolder.h"

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

TEST_CASE("SetGamesFolder moves every games-folder setting under the new folder") {
  const fs::path dir = TempDir("games-folder");
  config::Config config(dir / "settings.toml");
  config.Load();
  REQUIRE(config.Set("library_roots", nlohmann::json::array({"~/Games", "/mnt/extra"})));

  const fs::path folder = dir / "My Games";
  REQUIRE(library::SetGamesFolder(config, folder.string() + "/"));

  CHECK(fs::is_directory(folder));
  CHECK(config.Get("library_roots") == nlohmann::json::array({folder.string(), "/mnt/extra"}));
  CHECK(config.Get("prefix_root") == (folder / "prefixes").string());
  CHECK(config.Get("gog.install_root") == (folder / "GOG").string());
  CHECK(config.Get("itch.install_root") == (folder / "itch").string());
  CHECK(config.Get("humble.download_root") == (folder / "Humble Bundle").string());
}

TEST_CASE("SetGamesFolder refuses a relative path and changes nothing") {
  const fs::path dir = TempDir("games-folder-relative");
  config::Config config(dir / "settings.toml");
  config.Load();
  const nlohmann::json before = config.Get("library_roots");

  const Result<void> result = library::SetGamesFolder(config, "Games");
  REQUIRE_FALSE(result);
  CHECK(result.error().code == "invalid_path");
  CHECK(config.Get("library_roots") == before);
}
