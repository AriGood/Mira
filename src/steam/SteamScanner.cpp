#include "steam/SteamScanner.h"

#include "core/Log.h"
#include "steam/SteamDetector.h"

namespace mira::steam {

SteamScanner::SteamScanner(config::Config& config, store::GameStore& games, api::EventBus& events)
    : config_(config), games_(games), events_(events) {}

Result<SteamScanSummary> SteamScanner::Scan() {
  SteamScanSummary summary;
  if (!config_.GetBool("steam.enabled")) return summary;

  const auto root = FindSteamRoot(config_);
  if (!root) return Err("steam_not_found", "no Steam installation found");

  for (const SteamApp& app : ListApps(*root)) {
    const std::string id = "steam-" + app.appid;
    const auto existing = games_.Find(id);

    // Preserve anything the user already configured across a rescan
    // (exe_path/args/env for "direct" mode, overrides, reviewed) — only the
    // fields Steam itself owns get overwritten.
    model::Game game = existing.value_or(model::Game{});
    game.id = id;
    game.name = app.name;
    game.install_path = app.install_dir.string();
    game.platform = app.is_native ? model::Platform::Native : model::Platform::Windows;
    game.data_dir = app.compat_data_dir.string();
    game.runner_ref = "steam:" + app.appid;
    game.status = model::GameStatus::Ready;  // Steam already installed and provisioned it
    game.last_error.clear();
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
      events_.Publish("game.added", model::ToJson(game));
    }
  }
  return summary;
}

}  // namespace mira::steam
