#include "library/Catalog.h"

#include <json.hpp>

#include "core/Log.h"
#include "epic/Legendary.h"
#include "steam/SteamWebApi.h"

namespace mira::library {
namespace {
using nlohmann::json;

// Marks an entry as already-tracked if Mira has a game for it. Both sources
// derive a tracked game's id the same way ("<source>-<ref>"), so this is a
// direct lookup rather than a scan.
void MarkTracked(const store::GameStore& games, CatalogEntry& entry) {
  const std::string id = entry.source + "-" + entry.ref;
  if (const auto tracked = games.Find(id)) {
    entry.installed = true;
    entry.game_id = id;
  }
}

std::vector<CatalogEntry> EpicCatalog(const config::Config& config, const store::GameStore& games) {
  std::vector<CatalogEntry> entries;
  if (!config.GetBool("epic.enabled")) return entries;

  const Result<json> listed = epic::RunLegendaryJson(config, {"list"});
  if (!listed) {
    log::Warn("epic catalog unavailable: {}", listed.error().message);
    return entries;
  }
  if (!listed->is_array()) return entries;

  for (const json& item : *listed) {
    CatalogEntry entry;
    entry.source = "epic";
    entry.ref = item.value("app_name", std::string());
    if (entry.ref.empty()) continue;
    // `list --json` calls this "app_title"; `list-installed --json` calls
    // the same thing "title" -- accept either.
    entry.title = item.value("app_title", item.value("title", std::string()));
    if (entry.title.empty()) entry.title = entry.ref;
    MarkTracked(games, entry);
    entries.push_back(std::move(entry));
  }
  return entries;
}

std::vector<CatalogEntry> SteamCatalog(const config::Config& config, const store::GameStore& games) {
  std::vector<CatalogEntry> entries;
  if (!config.GetBool("steam.enabled")) return entries;

  const Result<std::vector<steam::OwnedGame>> owned = steam::ListOwnedGames(config);
  if (!owned) {
    // Unconfigured (no key/steamid) is the normal case, not an error worth
    // failing the whole listing over -- Steam simply contributes nothing.
    log::Warn("steam catalog unavailable: {}", owned.error().message);
    return entries;
  }

  for (const steam::OwnedGame& game : *owned) {
    CatalogEntry entry;
    entry.source = "steam";
    entry.ref = game.appid;
    entry.title = game.name.empty() ? game.appid : game.name;
    entry.play_seconds = game.play_seconds;
    MarkTracked(games, entry);
    entries.push_back(std::move(entry));
  }
  return entries;
}

}  // namespace

Result<std::vector<CatalogEntry>> ListCatalog(const config::Config& config, const store::GameStore& games,
                                             const std::string& source) {
  if (!source.empty() && source != "epic" && source != "steam") {
    return Err("unknown_source", "no catalog source named \"" + source + "\"");
  }

  std::vector<CatalogEntry> entries;
  if (source.empty() || source == "epic") {
    std::vector<CatalogEntry> epic_entries = EpicCatalog(config, games);
    entries.insert(entries.end(), epic_entries.begin(), epic_entries.end());
  }
  if (source.empty() || source == "steam") {
    std::vector<CatalogEntry> steam_entries = SteamCatalog(config, games);
    entries.insert(entries.end(), steam_entries.begin(), steam_entries.end());
  }
  return entries;
}

}  // namespace mira::library
