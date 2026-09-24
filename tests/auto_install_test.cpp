#include <doctest.h>

#include <filesystem>
#include <fstream>

#include "library/AutoInstall.h"

using namespace mira;
namespace fs = std::filesystem;

namespace {

fs::path TempFile(const char* name) {
  const fs::path dir = fs::temp_directory_path() / "mira-tests" / "auto-install";
  fs::create_directories(dir);
  const fs::path file = dir / name;
  fs::remove(file);
  return file;
}

void Write(const fs::path& path, std::string_view head, std::string_view tail, size_t padding) {
  std::ofstream out(path, std::ios::binary);
  out << head;
  out << std::string(padding, '\0');
  out << tail;
}

}  // namespace

TEST_CASE("DetectInstallerFormat recognizes Inno Setup near the head") {
  const fs::path file = TempFile("inno-head.exe");
  Write(file, "junk...Inno Setup junk", "", 0);
  CHECK(library::DetectInstallerFormat(file) == library::InstallerFormat::kInnoSetup);
}

TEST_CASE("DetectInstallerFormat recognizes NSIS's marker near the tail of a large file") {
  const fs::path file = TempFile("nsis-tail.exe");
  // Bigger than the 2MB sniff window on each end, marker only in the tail --
  // regression coverage for "don't just read the head of a monolithic
  // installer".
  Write(file, std::string(64, 'a'), "...Nullsoft...", 3 * 1024 * 1024);
  CHECK(library::DetectInstallerFormat(file) == library::InstallerFormat::kNsis);
}

TEST_CASE("DetectInstallerFormat reports unknown for a non-installer exe") {
  const fs::path file = TempFile("plain.exe");
  Write(file, "just a normal game binary, nothing special here", "", 0);
  CHECK(library::DetectInstallerFormat(file) == library::InstallerFormat::kUnknown);
}

TEST_CASE("DetectInstallerFormat reports unknown for a non-.exe file regardless of content") {
  const fs::path file = TempFile("setup.msi");
  Write(file, "Inno Setup", "", 0);
  CHECK(library::DetectInstallerFormat(file) == library::InstallerFormat::kUnknown);
}
