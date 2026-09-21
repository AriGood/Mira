#include "steam/SteamScanner.h"

#include <algorithm>
#include <map>

#include "core/Log.h"
#include "steam/SteamDetector.h"
#include "steam/SteamWebApi.h"

namespace mira::steam {
namespace {

// appid -> Steam's own playtime_forever, in seconds. Empty when the Web API
// isn't configured (the normal case) or unreachable — playtime import is
// strictly an enrichment on top of a scan that works fine without it.
std::map<std::string, std::int64_t> PlaytimeByAppid(const config::Config& config) {
  std::map<std::string, std::int64_t> playtime;
  if (!config.GetBool("steam.import_playtime")) return playtime;

  const Result<std::vector<OwnedGame>> owned = ListOwnedGames(config);
  if (!owned) {
    log::Warn("steam playtime not imported: {}", owned.error().message);
    return playtime;
  }
  for (const OwnedGame& game : *owned) playtime[game.appid] = game.play_seconds;
  return playtime;
}

}  // namespace

SteamScanner::SteamScanner(config::Config& config, store::GameStore& games, api::EventBus& events)
    : config_(config), games_(games), events_(events) {}

Result<SteamScanSummary> SteamScanner::Scan() {
  SteamScanSummary summary;
  if (!config_.GetBool("steam.enabled")) return summary;

  const auto root = FindSteamRoot(config_);
  if (!root) return Err("steam_not_found", "no Steam installation found");

  const std::map<std::string, std::int64_t> steam_playtime = PlaytimeByAppid(config_);

  for (const SteamApp& app : ListApps(*root)) {
    const std::string id = "steam-" + app.appid;
    const auto existing = games_.Find(id);

    // Preserve anything the user already configured across a rescan
    // (exe_path/args/env for "direct" mode, overrides, reviewed) — only the
    // fields Steam itself owns get overwritten.
    model::Game game = existing.value_or(model::Game{});
    game.id = id;
    game.source = "steam";
    game.name = app.name;
    game.install_path = app.install_dir.string();
    game.platform = app.is_native ? model::Platform::Native : model::Platform::Windows;
    game.data_dir = app.compat_data_dir.string();
    game.runner_ref = "steam:" + app.appid;
    game.status = model::GameStatus::Ready;  // Steam already installed and provisioned it
    game.last_error.clear();
    // Steam's own total counts play on any machine, and from long before
    // Mira existed -- but Mira's own tracked sessions must never be lost to
    // a stale Steam figure, so the larger of the two wins rather than
    // Steam's simply overwriting.
    if (const auto it = steam_playtime.find(app.appid); it != steam_playtime.end()) {
      game.play_seconds = std::max(game.play_seconds, it->second);
    }
    game.updated_at = model::NowSeconds();
    if (!existing) game.created_at = game.updated_at;

    auto result = games_.Upsert(game);
    if (!result) {
      log::Error("failed to save steam game {}: {}", id, result.error().message);
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

}  // namespace mira::steam
