#include "flatpak/FlatpakScanner.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "runner/FlatpakRunner.h"

namespace mira::flatpak {

FlatpakScanner::FlatpakScanner(config::Config& config, store::GameStore& games, api::EventBus& events)
    : config_(config), games_(games), events_(events) {}

Result<FlatpakScanSummary> FlatpakScanner::Scan() {
  FlatpakScanSummary summary;
  if (!config_.GetBool("flatpak.enabled")) return summary;

  const auto apps = runner::ListInstalledFlatpakApps();
  if (!apps) return std::unexpected(apps.error());

  for (const runner::FlatpakApp& app : *apps) {
    const std::string id = "flatpak-" + app.app_id;
    const auto existing = games_.Find(id);

    // Preserve anything the user already configured across a rescan
    // (overrides, tags, reviewed) — only the fields Flatpak itself owns
    // get overwritten, same contract as SteamScanner::Scan.
    model::Game game = existing.value_or(model::Game{});
    game.id = id;
    game.name = app.name;
    // No install folder to point at (the sandbox owns its own storage) --
    // the app's own per-app data directory is the closest real, on-disk
    // stand-in, so FindByInstallPath and DELETE containment have something
    // real to work with, mirroring the Steam precedent (compat_data_dir).
    game.install_path = (paths::Home() / ".var" / "app" / app.app_id).string();
    game.platform = model::Platform::Native;  // deliberately no new enum value; see docs/architecture.md
    game.runner_ref = "flatpak:" + app.app_id;
    game.status = model::GameStatus::Ready;  // flatpak already installed it
    game.last_error.clear();
    game.updated_at = model::NowSeconds();
    if (!existing) game.created_at = game.updated_at;

    auto result = games_.Upsert(game);
    if (!result) {
      log::Error("failed to save flatpak game {}: {}", id, result.error().message);
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

}  // namespace mira::flatpak
