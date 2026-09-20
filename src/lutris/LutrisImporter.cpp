#include "lutris/LutrisImporter.h"

#include <fkYAML.hpp>
#include <json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "core/Log.h"
#include "core/Paths.h"
#include "runner/Exec.h"

namespace mira::lutris {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

fs::path EnvOr(const char* name, const fs::path& fallback) {
  const char* value = std::getenv(name);
  return (value && *value) ? fs::path(value) : fallback;
}

// Lutris's own settings.py: CONFIG_DIR = get_user_config_dir()/lutris, but
// falls back to DATA_DIR when that doesn't exist — which is where the
// per-game YAML actually lives on a system that never had ~/.config/lutris
// (confirmed against a real install: no ~/.config/lutris at all, configs
// under ~/.local/share/lutris/games/).
std::optional<fs::path> FindLutrisDataDir(const config::Config& config) {
  std::vector<fs::path> candidates;
  if (const fs::path configured = config.GetPath("lutris.data_dir"); !configured.empty()) {
    candidates.push_back(configured);
  }
  candidates.push_back(EnvOr("XDG_DATA_HOME", paths::Home() / ".local" / "share") / "lutris");

  std::error_code ec;
  for (const fs::path& candidate : candidates) {
    if (fs::exists(candidate / "pga.db", ec)) return candidate;
  }
  return std::nullopt;
}

struct LutrisRow {
  int id = 0;
  std::string name;
  std::string slug;
  std::string runner;
  std::string configpath;
};

Result<std::vector<LutrisRow>> ReadCatalog(const std::string& sqlite3_bin, const fs::path& pga_db) {
  Command command;
  command.argv = {sqlite3_bin, "-json", pga_db.string(), "SELECT id, name, slug, runner, configpath FROM games;"};
  const Result<runner::ExecResult> result = runner::RunAndWait(command);
  if (!result || result->exit_code != 0) {
    return Err("lutris_db_read_failed",
               !result ? result.error().message
                       : std::format("sqlite3 exited {}: {}", result->exit_code, result->output));
  }

  // An empty result set prints nothing at all, not "[]".
  if (result->output.find_first_not_of(" \t\r\n") == std::string::npos) return std::vector<LutrisRow>{};

  const json parsed = json::parse(result->output, nullptr, false);
  if (!parsed.is_array()) {
    return Err("lutris_db_parse_failed", "unexpected output from sqlite3 -json");
  }

  std::vector<LutrisRow> rows;
  for (const json& row : parsed) {
    rows.push_back(LutrisRow{
        .id = row.value("id", 0),
        .name = row.value("name", ""),
        .slug = row.value("slug", ""),
        .runner = row.value("runner", ""),
        .configpath = row.value("configpath", ""),
    });
  }
  return rows;
}

// Lutris's "hidden" is a category named ".hidden", and "favorite" is
// "favorites" — both discovered from a real pga.db, not Lutris's docs (see
// lutris/game.py's is_hidden/mark_as_hidden). Everything else maps straight
// across as a same-named tag: a category is meant to organize games, which
// is exactly what a tag already does in Mira.
std::string TagForCategory(const std::string& category) {
  if (category == ".hidden") return "hidden";
  if (category == "favorites") return "favorite";
  return category;
}

// categories/games_categories don't exist on every Lutris version — a
// missing table degrades to "no categories" rather than failing the whole
// import; only a real sqlite3 dependency failure (already reported by
// ReadCatalog above) is worth surfacing loudly.
std::map<int, std::vector<std::string>> ReadCategories(const std::string& sqlite3_bin, const fs::path& pga_db) {
  Command command;
  command.argv = {sqlite3_bin, "-json", pga_db.string(),
                  "SELECT games_categories.game_id AS game_id, categories.name AS name FROM "
                  "games_categories JOIN categories ON categories.id = games_categories.category_id;"};
  const Result<runner::ExecResult> result = runner::RunAndWait(command);
  std::map<int, std::vector<std::string>> by_game;
  if (!result || result->exit_code != 0) return by_game;
  if (result->output.find_first_not_of(" \t\r\n") == std::string::npos) return by_game;

  const json parsed = json::parse(result->output, nullptr, false);
  if (!parsed.is_array()) return by_game;
  for (const json& row : parsed) {
    const std::string category = row.value("name", "");
    if (category.empty()) continue;
    by_game[row.value("game_id", 0)].push_back(TagForCategory(category));
  }
  return by_game;
}

// Union, not replace — a re-import must keep tags the user added by hand,
// same contract as every other field Lutris doesn't own (see Import()'s
// comment below).
std::vector<std::string> MergeTags(std::vector<std::string> existing, const std::vector<std::string>& lutris_tags) {
  for (const std::string& tag : lutris_tags) {
    if (std::ranges::find(existing, tag) == existing.end()) existing.push_back(tag);
  }
  return existing;
}

std::string NodeToString(const fkyaml::node& node) {
  if (node.is_string()) return node.get_value<std::string>();
  if (node.is_boolean()) return node.get_value<bool>() ? "true" : "false";
  if (node.is_integer()) return std::to_string(node.get_value<std::int64_t>());
  if (node.is_float_number()) return std::to_string(node.get_value<double>());
  return "";
}

struct LutrisGameConfig {
  std::string exe;
  std::string prefix;
  std::string args;
  std::map<std::string, std::string> env;
};

std::optional<LutrisGameConfig> ReadGameConfig(const fs::path& yaml_path) {
  std::ifstream in(yaml_path);
  if (!in) return std::nullopt;
  std::stringstream buffer;
  buffer << in.rdbuf();

  LutrisGameConfig cfg;
  try {
    const fkyaml::node root = fkyaml::node::deserialize(buffer.str());
    if (!root.is_mapping() || !root.contains("game")) return std::nullopt;
    const fkyaml::node& game = root["game"];
    if (game.contains("exe") && game["exe"].is_string()) cfg.exe = game["exe"].get_value<std::string>();
    if (game.contains("prefix") && game["prefix"].is_string()) cfg.prefix = game["prefix"].get_value<std::string>();
    if (game.contains("args") && game["args"].is_string()) cfg.args = game["args"].get_value<std::string>();

    if (root.contains("system") && root["system"].is_mapping() && root["system"].contains("env") &&
        root["system"]["env"].is_mapping()) {
      for (const auto& [key, value] : root["system"]["env"].map_items()) {
        cfg.env[key.get_value<std::string>()] = NodeToString(value);
      }
    }
  } catch (const std::exception& e) {
    log::Warn("failed to parse Lutris config {}: {}", yaml_path.string(), e.what());
    return std::nullopt;
  }

  // No inference beyond what the yaml itself declares: a game with no
  // prefix recorded is skipped rather than guessed at (Lutris's own
  // fallback for this case is a filesystem walk-up heuristic, not
  // something read from the yaml tree).
  if (cfg.exe.empty() || cfg.prefix.empty()) return std::nullopt;
  return cfg;
}

}  // namespace

LutrisImporter::LutrisImporter(config::Config& config, store::GameStore& games, api::EventBus& events)
    : config_(config), games_(games), events_(events) {}

Result<LutrisImportSummary> LutrisImporter::Import() {
  LutrisImportSummary summary;
  if (!config_.GetBool("lutris.enabled")) return summary;

  const auto data_dir = FindLutrisDataDir(config_);
  if (!data_dir) return Err("lutris_not_found", "no Lutris installation found");

  const auto sqlite3 = runner::FindOnPath("sqlite3");
  if (!sqlite3) {
    return Err("sqlite3_missing",
               "sqlite3 isn't installed — install it from your distro's package manager to read "
               "Lutris's game database");
  }
  const fs::path pga_db = *data_dir / "pga.db";

  const auto rows = ReadCatalog(*sqlite3, pga_db);
  if (!rows) return std::unexpected(rows.error());
  const std::map<int, std::vector<std::string>> categories = ReadCategories(*sqlite3, pga_db);

  for (const LutrisRow& row : *rows) {
    // A Lutris row run through anything other than wine is a different,
    // unhandled case for now (a "steam" row is already covered by
    // SteamScanner; a native-Linux row would need no runner_ref resolution
    // at all) — note it, don't guess at it.
    if (row.runner != "wine") {
      ++summary.skipped;
      continue;
    }

    const fs::path yaml_path = *data_dir / "games" / (row.configpath + ".yml");
    const auto cfg = ReadGameConfig(yaml_path);
    if (!cfg) {
      ++summary.skipped;
      continue;
    }

    // Nothing here is inferred from how exe and prefix relate to each other
    // on disk — an exe given relative is relative to prefix because that's
    // what Lutris's own config format declares (see Epic's real config:
    // exe under drive_c, prefix $GAMEDIR), not because Mira went looking.
    // install_path is always the exe's own directory; data_dir is always
    // exactly what the yaml's prefix says, verbatim, wherever that is.
    const fs::path prefix = fs::path(cfg->prefix);
    const fs::path exe_raw = fs::path(cfg->exe);
    const fs::path exe_abs = exe_raw.is_absolute() ? exe_raw : prefix / exe_raw;

    const std::string install_path = exe_abs.parent_path().string();
    const std::string exe_path = exe_abs.filename().string();
    const std::string data_dir_path = prefix.string();

    // Not a layout guess — a safety floor for one specific real shape: an
    // exe referenced with no subdirectory at all under drive_c (a launcher
    // script dropped straight at the C: drive root, e.g. a second game
    // riding along in another game's prefix). That makes install_path the
    // whole C: drive, shared by every other game in that prefix — Mira's
    // DELETE /v1/games removes install_path's contents, so handing out a
    // scope that broad would let deleting this game take the others with
    // it. A combined install+prefix layout (install_path == prefix itself,
    // e.g. Batman) is fine and left alone: that prefix belongs to this game
    // alone.
    if (install_path == (prefix / "drive_c").string()) {
      log::Warn("skipping lutris game {}: install path {} is drive_c's own root, not something game-specific",
               row.name, install_path);
      ++summary.skipped;
      continue;
    }

    const auto existing = games_.FindByInstallPath(install_path);

    // Preserve anything the user already configured across a re-import —
    // only the fields Lutris itself owns get overwritten, same contract as
    // SteamScanner::Scan.
    model::Game game = existing.value_or(model::Game{});
    game.id = existing ? game.id : games_.NextId(row.slug.empty() ? row.name : row.slug);
    game.source = "lutris";
    game.name = row.name;
    game.install_path = install_path;
    game.exe_path = exe_path;
    game.args = cfg->args;
    game.data_dir = data_dir_path;
    game.platform = model::Platform::Windows;
    game.env = cfg->env;
    if (const auto it = categories.find(row.id); it != categories.end()) {
      game.tags = MergeTags(game.tags, it->second);
    }
    // Lutris's own wine.version is often a generic alias ("ge-proton"), not
    // an exact installed build name Mira can resolve — leave runner_ref
    // alone (empty for a new game) and let default_runner.windows pick one.
    game.status = model::GameStatus::Ready;  // Lutris already installed and configured it
    game.last_error.clear();
    game.updated_at = model::NowSeconds();
    if (!existing) game.created_at = game.updated_at;

    auto result = games_.Upsert(game);
    if (!result) {
      log::Error("failed to save lutris game {}: {}", game.id, result.error().message);
      continue;
    }
    if (existing) {
      ++summary.updated;
      events_.Publish("game.updated", model::ToJson(game));
    } else {
      ++summary.added;
      summary.added_games.push_back(game);
      events_.Publish("game.added", model::ToJson(game));
    }
  }

  return summary;
}

}  // namespace mira::lutris
