#include <doctest.h>

#include <filesystem>
#include <fstream>

#include "library/ArchiveExtractor.h"

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

TEST_CASE("LooksLikeArchive recognizes every supported extension, nothing else") {
  CHECK(library::LooksLikeArchive("Game.zip"));
  CHECK(library::LooksLikeArchive("Game.tar.gz"));
  CHECK(library::LooksLikeArchive("Game.tar.xz"));
  CHECK(library::LooksLikeArchive("Game.tar.bz2"));
  CHECK(library::LooksLikeArchive("Game.tgz"));
  CHECK(library::LooksLikeArchive("Game.tar"));
  CHECK(library::LooksLikeArchive("Game.rar"));
  CHECK(library::LooksLikeArchive("Game.7z"));
  CHECK(library::LooksLikeArchive("GAME.ZIP"));  // case-insensitive

  CHECK_FALSE(library::LooksLikeArchive("Game.exe"));
  CHECK_FALSE(library::LooksLikeArchive("Game"));
}

TEST_CASE("StemWithoutArchiveExtension strips exactly the matched suffix, not a guessed one") {
  CHECK(library::StemWithoutArchiveExtension("Game.zip") == "Game");
  CHECK(library::StemWithoutArchiveExtension("Game.tar.gz") == "Game");
  CHECK(library::StemWithoutArchiveExtension("Game.tar") == "Game");
  // A dot inside the real name must survive: naive repeated stem() calls
  // would wrongly eat "Edition" here too.
  CHECK(library::StemWithoutArchiveExtension("My.Game.Deluxe.Edition.zip") == "My.Game.Deluxe.Edition");
  CHECK(library::StemWithoutArchiveExtension("My.Game.tar.gz") == "My.Game");
}

TEST_CASE("ExtractAndRemove extracts a real tar.gz and deletes the archive on success") {
  const fs::path dir = TempDir("archive-extractor-tar");
  const fs::path src = dir / "src";
  fs::create_directories(src);
  std::ofstream(src / "file.txt") << "hello";

  const fs::path archive = dir / "Game.tar.gz";
  REQUIRE(std::system(("tar -C " + src.string() + " -czf " + archive.string() + " file.txt").c_str()) == 0);

  const fs::path dest = dir / "Game";
  auto result = library::ExtractAndRemove(archive, dest);
  REQUIRE(result.has_value());
  CHECK(fs::exists(dest / "file.txt"));
  CHECK_FALSE(fs::exists(archive));  // the archive itself is gone
}

TEST_CASE("ExtractAndRemove leaves a non-archive file untouched") {
  const fs::path dir = TempDir("archive-extractor-not-archive");
  const fs::path file = dir / "readme.txt";
  std::ofstream(file) << "not an archive";

  auto result = library::ExtractAndRemove(file, dir / "out");
  CHECK_FALSE(result.has_value());
  CHECK(fs::exists(file));  // never deleted on failure
}
