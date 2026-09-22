#include <doctest.h>

#include <filesystem>
#include <fstream>

#include "config/Config.h"
#include "humble/Humble.h"

using namespace mira;
namespace fs = std::filesystem;

namespace {

fs::path TempDir(const char* name) {
  const fs::path dir = fs::temp_directory_path() / "mira-tests" / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

// A stand-in for the real `humble-cli`, pointed at by humble.humble_cli_bin.
// humble-cli has no --json output (confirmed live against a real release,
// v0.23.2) -- only a Go text/tabwriter-style table, space-padded columns --
// so `list_output` here reproduces that shape exactly, header row and all,
// rather than something more convenient to parse.
void WriteFakeHumbleCli(const fs::path& path, bool authenticated, const std::string& list_output) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path);
  out << "#!/bin/sh\n"
      << "case \"$1\" in\n"
      << "  auth) exit 0 ;;\n"
      << "  list)\n";
  if (authenticated) {
    out << "    printf '%b' \"" << list_output << "\"\n";
  } else {
    out << "    echo 'Error: config file not found. Use '\"'\"'humble-cli auth <SESSION-KEY>'\"'\"' to set it' >&2\n"
        << "    exit 1\n";
  }
  out << "    ;;\n"
      << "  --version) printf 'humble-cli version 0.23.2\\n' ;;\n"
      << "esac\n"
      << "exit 0\n";
  out.close();
  fs::permissions(path, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec);
}

// A realistic tabwriter render: columns padded to the widest entry, at
// least two spaces between columns.
constexpr const char* kTable =
    "KEY                       NAME                            CLAIMED\\n"
    "abc123def456              Indie Bundle 42                 yes\\n"
    "xyz789                    RPG Maker Bundle                no\\n";

struct Fixture {
  fs::path dir;
  config::Config config;

  explicit Fixture(const char* name) : dir(TempDir(name)), config(dir / "settings.toml") { config.Load(); }

  void UseFakeHumbleCli(bool authenticated, const std::string& list_output = kTable) {
    const fs::path bin = dir / "humble-cli";
    WriteFakeHumbleCli(bin, authenticated, list_output);
    REQUIRE(config.Set("humble.humble_cli_bin", bin.string()));
  }
};

}  // namespace

TEST_CASE("DetectHumbleCli honours the humble.humble_cli_bin override") {
  Fixture fixture("humble-detect");
  fixture.UseFakeHumbleCli(true);

  const humble::HumbleStatus status = humble::DetectHumbleCli(fixture.config);
  CHECK(status.installed);
  CHECK(status.source == "override");
}

TEST_CASE("Status reports unauthenticated when humble-cli has no session key yet") {
  Fixture fixture("humble-status-unauth");
  fixture.UseFakeHumbleCli(false);

  const humble::HumbleAuthStatus status = humble::Status(fixture.config);
  REQUIRE(status.humble_cli.installed);
  CHECK_FALSE(status.authenticated);
}

TEST_CASE("Status reports authenticated once a session key works") {
  Fixture fixture("humble-status-auth");
  fixture.UseFakeHumbleCli(true);

  const humble::HumbleAuthStatus status = humble::Status(fixture.config);
  REQUIRE(status.humble_cli.installed);
  CHECK(status.authenticated);
}

TEST_CASE("ListBundles parses a tabwriter-style table, skipping the header") {
  Fixture fixture("humble-list");
  fixture.UseFakeHumbleCli(true);

  const Result<std::vector<humble::BundleSummary>> bundles = humble::ListBundles(fixture.config);
  REQUIRE(bundles);
  REQUIRE(bundles->size() == 2);

  CHECK((*bundles)[0].key == "abc123def456");
  CHECK((*bundles)[0].name == "Indie Bundle 42");
  CHECK((*bundles)[0].claimed);

  CHECK((*bundles)[1].key == "xyz789");
  CHECK((*bundles)[1].name == "RPG Maker Bundle");
  CHECK_FALSE((*bundles)[1].claimed);
}

TEST_CASE("ListBundles copes with a name containing internal single spaces") {
  // The real risk with a "2+ spaces = new column" split: a name column
  // itself never has 2 consecutive spaces in practice (title text), but a
  // single space inside one (very common) must stay part of that column,
  // not get treated as a delimiter.
  Fixture fixture("humble-list-spaces");
  fixture.UseFakeHumbleCli(true,
                          "KEY            NAME                       CLAIMED\\n"
                          "k1             A Rather Long Bundle Name  yes\\n");

  const Result<std::vector<humble::BundleSummary>> bundles = humble::ListBundles(fixture.config);
  REQUIRE(bundles);
  REQUIRE(bundles->size() == 1);
  CHECK((*bundles)[0].name == "A Rather Long Bundle Name");
}
