#include <doctest.h>

#include <fstream>
#include <filesystem>

#include "config/Config.h"
#include "config/Resolver.h"
#include "config/Schema.h"

using namespace mira::config;
namespace fs = std::filesystem;

TEST_CASE("every schema entry is well-formed") {
  // Guards the registry itself: a key with no doc string or an invalid
  // default would silently produce a broken settings UI and undocumented
  // config.md, so this is checked mechanically rather than by review.
  for (const Entry& entry : Schema::Instance().Entries()) {
    INFO("key: ", entry.key);
    CHECK_FALSE(entry.doc.empty());
    CHECK_FALSE(Schema::Instance().Validate(entry.key, entry.default_value).has_value());
  }
}

TEST_CASE("Schema::Validate rejects bad values with a reason") {
  const Schema& schema = Schema::Instance();
  CHECK_FALSE(schema.Validate("scan.debounce_ms", 5000).has_value());
  CHECK(schema.Validate("scan.debounce_ms", -1).has_value());
  CHECK(schema.Validate("scan.debounce_ms", "not a number").has_value());
  CHECK(schema.Validate("prefix_provider", "not_a_real_option").has_value());
  CHECK(schema.Validate("nonexistent.key", 1).has_value());
}

namespace {
fs::path TempFile(const char* name) {
  const fs::path dir = fs::temp_directory_path() / "mira-tests";
  fs::create_directories(dir);
  return dir / name;
}
}  // namespace

TEST_CASE("Config materialises defaults when no file exists") {
  const fs::path file = TempFile("settings-defaults.toml");
  fs::remove(file);
  Config config(file);
  config.Load();
  CHECK(fs::exists(file));
  CHECK(config.GetInt("scan.debounce_ms") == 3000);
}

TEST_CASE("Config round-trips a value and preserves frontend settings") {
  const fs::path file = TempFile("settings-roundtrip.toml");
  fs::remove(file);
  Config config(file);
  config.Load();
  CHECK(config.Set("scan.debounce_ms", 9000).has_value());
  config.SetFrontendSettings({{"theme", "dark"}});

  Config reloaded(file);
  reloaded.Load();
  CHECK(reloaded.GetInt("scan.debounce_ms") == 9000);
  CHECK(reloaded.FrontendSettings().value("theme", "") == "dark");
}

TEST_CASE("Config::Set rejects an invalid value and changes nothing") {
  const fs::path file = TempFile("settings-invalid.toml");
  fs::remove(file);
  Config config(file);
  config.Load();
  const auto before = config.GetInt("scan.debounce_ms");
  auto result = config.Set("scan.debounce_ms", -5);
  CHECK_FALSE(result.has_value());
  CHECK(config.GetInt("scan.debounce_ms") == before);
}

TEST_CASE("an unparseable settings file is quarantined, not fatal") {
  const fs::path file = TempFile("settings-corrupt.toml");
  {
    std::ofstream out(file);
    out << "this is not [ valid toml";
  }
  Config config(file);
  config.Load();  // must not throw or crash
  CHECK(config.GetInt("scan.debounce_ms") == 3000);  // falls back to defaults
  CHECK(fs::exists(file.string() + ".bad"));
  fs::remove(file.string() + ".bad");
}

TEST_CASE("Resolver layers game overrides above the config file above defaults") {
  const fs::path file = TempFile("settings-resolver.toml");
  fs::remove(file);
  Config config(file);
  config.Load();
  REQUIRE(config.Set("scan.max_depth", 6).has_value());

  Resolver no_override(config, nlohmann::json::object());
  CHECK(no_override.GetInt("scan.max_depth") == 6);
  CHECK(no_override.Resolve("scan.max_depth").layer == Layer::ConfigFile);

  Resolver with_override(config, nlohmann::json{{"scan.max_depth", 10}});
  CHECK(with_override.GetInt("scan.max_depth") == 10);
  CHECK(with_override.Resolve("scan.max_depth").layer == Layer::Game);

  // Daemon-only keys must not be overridable per game, even if a game
  // document somehow carries one.
  Resolver bogus(config, nlohmann::json{{"library_roots", nlohmann::json::array({"/tmp"})}});
  CHECK_FALSE(Resolver::IsOverridable("library_roots"));
  CHECK(bogus.Resolve("library_roots").layer != Layer::Game);
}
