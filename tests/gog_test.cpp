#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "api/EventBus.h"
#include "config/Config.h"
#include "core/Paths.h"
#include "gog/Gog.h"
#include "gog/GogImporter.h"
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

// A stand-in for the real `gogdl`, pointed at by gog.gogdl_bin --
// mirrors epic_test.cpp's WriteFakeLegendary (real executable, real
// fork/exec/pipe, not a mocked subprocess layer). Unlike Legendary, gogdl
// itself owns writing its --auth-config-path token file -- the fake here
// does the same, so gog::AccessToken's read-back path is exercised for
// real, not assumed.
void WriteFakeGogdl(const fs::path& path, const std::string& auth_success_body, const std::string& import_body) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path);
  out << "#!/bin/sh\n"
      // Global "--auth-config-path <path>" always comes first (RunGogdl's
      // own convention) -- except VersionOf's own "--version" probe,
      // which calls the binary directly with no wrapper args at all, so
      // this only shifts when that flag is actually present.
      << "if [ \"$1\" = \"--auth-config-path\" ]; then\n"
      << "  auth_path=\"$2\"\n"
      << "  shift 2\n"
      << "fi\n"
      << "case \"$1\" in\n"
      << "  auth)\n"
      << "    if [ \"$2\" = \"--code\" ]; then\n"
      << "      if [ \"$3\" = \"bad-code\" ]; then printf '{\"error\": true}\\n'; exit 0; fi\n"
      << "    fi\n"
      // Both a fresh `--code` login and a bare refresh call end up here --
      // real gogdl rewrites the token file (with a fresh loginTime) either
      // way, which is what makes AccessToken's refresh-by-invoking-auth
      // trick work.
      << "    printf '%s' '" << auth_success_body << "' > \"$auth_path\"\n"
      << "    cat \"$auth_path\"\n"
      << "    ;;\n"
      << "  import) printf '%s\\n' '" << import_body << "' ;;\n"
      << "  download) ;;\n"
      << "  --version) printf 'gogdl version 1.3.0\\n' ;;\n"
      << "esac\n"
      << "exit 0\n";
  out.close();
  fs::permissions(path, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec);
}

struct Fixture {
  fs::path dir;
  config::Config config;
  store::GameStore games;
  api::EventBus events;

  explicit Fixture(const char* name)
      : dir(TempDir(name)), config(dir / "settings.toml"), games(dir / "games.toml") {
    config.Load();
    games.Load();
  }

  // gogdl nests the real fields one level down, keyed by client_id --
  // confirmed live (see Gog.cpp's ReadAuthConfig comment), not the flat
  // shape it'd be natural to assume.
  void UseFakeGogdl(const std::string& auth_success_body =
                       R"({"46899977096215655": {"access_token": "tok", "refresh_token": "ref", )"
                       R"("expires_in": 3600, "loginTime": 9999999999}})",
                    const std::string& import_body = R"({"title": "A GOG Game"})") {
    const fs::path bin = dir / "gogdl";
    WriteFakeGogdl(bin, auth_success_body, import_body);
    REQUIRE(config.Set("gog.gogdl_bin", bin.string()));
    REQUIRE(config.Set("prefix_root", (dir / "prefixes").string()));
  }
};

}  // namespace

TEST_CASE("DetectGog honours the gog.gogdl_bin override") {
  Fixture fixture("gog-detect");
  fixture.UseFakeGogdl();

  const gog::GogStatus status = gog::DetectGog(fixture.config);
  CHECK(status.installed);
  CHECK(status.source == "override");
  CHECK(status.path == (fixture.dir / "gogdl").string());
}

TEST_CASE("DetectGog reports nothing installed when there is nothing to find") {
  Fixture fixture("gog-detect-missing");
  REQUIRE(fixture.config.Set("gog.gogdl_bin", (fixture.dir / "nope").string()));

  const gog::GogStatus status = gog::DetectGog(fixture.config);
  CHECK(status.source != "override");
}

TEST_CASE("Login stores what gogdl wrote, and Status reads it back") {
  Fixture fixture("gog-login");
  fixture.UseFakeGogdl();

  REQUIRE(gog::Login(fixture.config, "good-code"));
  const gog::GogAuthStatus status = gog::Status(fixture.config);
  REQUIRE(status.gogdl.installed);
  CHECK(status.authenticated);
}

TEST_CASE("Login fails when gogdl reports {\"error\": true} despite exiting 0") {
  // Confirmed live: `gogdl auth --code <bogus>` prints {"error": true} but
  // still exits 0 -- same exit-code lie Legendary has, and the same fix
  // (verify via a real status read-back, not the exit code).
  Fixture fixture("gog-login-bad-code");
  fixture.UseFakeGogdl();

  const Result<void> result = gog::Login(fixture.config, "bad-code");
  REQUIRE_FALSE(result);
  CHECK(result.error().code == "login_failed");
  CHECK_FALSE(gog::Status(fixture.config).authenticated);
}

TEST_CASE("AccessToken refreshes an expired token by re-invoking gogdl auth") {
  Fixture fixture("gog-token-refresh");
  fixture.UseFakeGogdl();

  // Write an already-expired token file directly, bypassing gogdl -- this
  // is the state Mira finds on a real second run after the token's
  // expires_in has elapsed.
  std::ofstream expired(gog::AuthConfigPath(fixture.config));
  expired << R"({"46899977096215655": {"access_token": "stale", "refresh_token": "ref", )"
             R"("expires_in": 60, "loginTime": 0}})";
  expired.close();

  const Result<std::string> token = gog::AccessToken(fixture.config);
  REQUIRE(token);
  CHECK(*token == "tok");  // the fake's refresh path writes this, not "stale"
}

TEST_CASE("GogImporter::ImportPath tags and provisions a title") {
  Fixture fixture("gog-import");
  fixture.UseFakeGogdl();

  gog::GogImporter importer(fixture.config, fixture.games, fixture.events);
  const Result<model::Game> imported = importer.ImportPath("123", fixture.dir / "installed" / "123");
  REQUIRE(imported);
  CHECK(imported->name == "A GOG Game");
  CHECK(imported->source == "gog");
  CHECK(imported->source_ref == "123");
  CHECK(std::ranges::find(imported->tags, "gog") != imported->tags.end());
  CHECK_FALSE(imported->data_dir.empty());

  const auto game = fixture.games.Find("gog-123");
  REQUIRE(game);
  CHECK(game->name == "A GOG Game");
}

TEST_CASE("GogImporter::Import scans install_root's subdirectories") {
  Fixture fixture("gog-import-scan");
  fixture.UseFakeGogdl();

  const fs::path root = fixture.dir / "install_root";
  fs::create_directories(root / "123");
  fs::create_directories(root / "456");
  REQUIRE(fixture.config.Set("gog.install_root", root.string()));

  gog::GogImporter importer(fixture.config, fixture.games, fixture.events);
  const Result<gog::GogImportSummary> summary = importer.Import();
  REQUIRE(summary);
  CHECK(summary->added == 2);
  CHECK(fixture.games.All().size() == 2);
}
