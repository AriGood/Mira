#include <doctest.h>

#include <filesystem>
#include <fstream>

#include "launchers/Launchers.h"

using namespace mira;
namespace fs = std::filesystem;

TEST_CASE("launchers: Ubisoft installs are read from system.reg") {
  const fs::path prefix = fs::temp_directory_path() / "mira-tests" / "launcher-reg";
  fs::remove_all(prefix);
  fs::create_directories(prefix / "drive_c");
  std::ofstream(prefix / "system.reg")
      << "WINE REGISTRY Version 2\n\n"
         "[Software\\\\Wow6432Node\\\\Ubisoft\\\\Launcher\\\\Installs\\\\5595] 1700000000\n"
         "\"InstallDir\"=\"C:/Program Files (x86)/Ubisoft/Ubisoft Game Launcher/games/Trackmania/\"\n\n"
         "[Software\\\\Wow6432Node\\\\Ubisoft\\\\Launcher\\\\Installs\\\\5595\\\\Nested] 1700000000\n"
         "\"InstallDir\"=\"C:\\\\nope\"\n";

  const auto installs =
      launchers::ReadRegSubkeys(prefix / "system.reg", "Software\\Wow6432Node\\Ubisoft\\Launcher\\Installs");
  REQUIRE(installs.size() == 1);
  const std::string dir = installs.at("5595").at("InstallDir");
  CHECK(launchers::HostPath(prefix, dir) ==
        prefix / "drive_c/Program Files (x86)/Ubisoft/Ubisoft Game Launcher/games/Trackmania");
}

TEST_CASE("launchers: games are matched by their folder as Wine shows it") {
  model::Game game;
  game.data_dir = "/games/prefixes/battle-net";
  game.install_path = "/games/prefixes/battle-net/drive_c/Program Files (x86)/Hearthstone";
  CHECK(launchers::WindowsDir(game) == "c:/program files (x86)/hearthstone");
  game.install_path = "/mnt/Games/Trackmania/";
  CHECK(launchers::WindowsDir(game) == "z:/mnt/games/trackmania");
}
