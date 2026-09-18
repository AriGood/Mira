#include "library/Scanner.h"

#include <algorithm>
#include <iterator>

#include "core/Log.h"
#include "core/Strings.h"
#include "desktop/DesktopEntries.h"
#include "library/AutoSetup.h"
#include "library/Detector.h"
#include "library/WinePrefix.h"
#include "runner/RunnerRegistry.h"

namespace mira::library {
namespace {
namespace fs = std::filesystem;

// What a game's status should be when its folder reappears after being
// marked Missing. Derived from the game's own data rather than assumed:
// "has an exe_path" is not the same as "launchable" — an installer has one
// too, and a Windows game that was never provisioned has no prefix behind
// it. Getting this wrong silently un-did the installer guard.
model::GameStatus RestoredStatus(const model::Game& game) {
  const auto chosen = std::ranges::find(game.candidates, true, &model::Candidate::chosen);
  if (chosen != game.candidates.end() && chosen->is_installer) {
    return model::GameStatus::NeedsInstall;
  }
  if (game.exe_path.empty()) return model::GameStatus::SettingUp;
  if (game.platform == model::Platform::Native) return model::GameStatus::Ready;

  // Windows: only ready if something actually provisioned it.
  std::error_code ec;
  const bool provisioned =
      !game.runner_ref.empty() && !game.data_dir.empty() && fs::exists(game.data_dir, ec);
  return provisioned ? model::GameStatus::Ready : model::GameStatus::SettingUp;
}

DetectorSettings SettingsFromConfig(const config::Config& config) {
  DetectorSettings settings;
  settings.max_depth = static_cast<int>(config.GetInt("scan.max_depth"));
  settings.rules = config.GetStringArray("detect.rules");
  settings.name_match_bonus = config.GetDouble("detect.name_match_bonus");
  settings.depth_penalty = config.GetDouble("detect.depth_penalty");
  settings.low_confidence_threshold = config.GetDouble("detect.low_confidence_threshold");
  settings.deny_name_patterns = config.GetStringArray("detect.deny_name_patterns");
  settings.installer_name_patterns = config.GetStringArray("detect.installer_name_patterns");
  settings.installer_min_size_mb = config.GetInt("detect.installer_min_size_mb");
  settings.ignore_globs = config.GetStringArray("scan.ignore_globs");
  return settings;
}

// Provisions `game` if it's a Windows game still SettingUp, and writes back
// only the fields provisioning owns (see the writeback comment at the call
// site for why not a full Upsert). No-op for anything already past SettingUp.
void TryProvision(model::Game game, const runner::RunnerRegistry& runners, store::GameStore& games,
                  api::EventBus& events) {
  if (game.status != model::GameStatus::SettingUp) return;
  const model::Game provisioned = runners.ProvisionGame(game);
  auto result = games.Update(game.id, [&](model::Game& stored) {
    stored.runner_ref = provisioned.runner_ref;
    stored.status = provisioned.status;
    stored.last_error = provisioned.last_error;
  });
  if (!result) {
    log::Error("failed to save provisioning result for {}: {}", game.id, result.error().message);
    return;
  }
  events.Publish("game.updated", model::ToJson(*result));
}

}  // namespace

Scanner::Scanner(config::Config& config, store::GameStore& games, api::EventBus& events)
    : config_(config), games_(games), events_(events) {}

ScanSummary Scanner::ScanAll() {
  ScanSummary total;
  for (const fs::path& root : config_.GetPathArray("library_roots")) {
    ScanSummary partial = ScanRoot(root);
    total.added += partial.added;
    total.missing += partial.missing;
    total.restored += partial.restored;
    std::ranges::move(partial.added_games, std::back_inserter(total.added_games));
  }
  return total;
}

ScanSummary Scanner::ScanRoot(const fs::path& root) {
  ScanSummary summary;
  std::error_code ec;
  if (!fs::is_directory(root, ec)) {
    log::Warn("library root {} does not exist — skipping", root.string());
    return summary;
  }

  const fs::path prefix_root = config_.GetPath("prefix_root");
  const DetectorSettings detector_settings = SettingsFromConfig(config_);
  const Detector detector(detector_settings);
  AutoSetup auto_setup(config_, games_, events_);
  const runner::RunnerRegistry runners(config_);

  std::vector<std::string> seen_install_paths;

  for (const auto& entry : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
    if (!entry.is_directory(ec)) continue;
    const fs::path& dir = entry.path();

    std::error_code eq;
    if (fs::equivalent(dir, prefix_root, eq) || (!eq && dir == prefix_root)) continue;
    if (LooksLikeWinePrefix(dir)) continue;

    const std::string basename = dir.filename().string();
    const bool ignored = std::ranges::any_of(detector_settings.ignore_globs, [&](const std::string& glob) {
      return strings::GlobMatch(strings::ToLower(glob), strings::ToLower(basename));
    });
    if (ignored) continue;

    const std::string install_path = dir.string();
    seen_install_paths.push_back(install_path);

    if (auto existing = games_.FindByInstallPath(install_path)) {
      if (existing->status == model::GameStatus::Missing) {
        auto result = games_.Update(existing->id, [](model::Game& game) {
          game.status = RestoredStatus(game);
          if (game.status != model::GameStatus::NeedsInstall) game.last_error.clear();
        });
        if (!result) {
          log::Error("failed to restore {}: {}", existing->id, result.error().message);
        } else {
          existing = *result;
          // Same reasoning as game.removed below: a listener that already
          // has this game (now Missing) needs to hear about it coming back,
          // without waiting on a caller to relist the whole library.
          events_.Publish("game.updated", model::ToJson(*existing));
        }
        ++summary.restored;
      }
      // Retry provisioning for a game still stuck at SettingUp: auto_setup
      // may have been off when it was first detected and turned on since, a
      // previous attempt may have failed transiently, or the daemon may have
      // restarted mid-provision last time. Without this, SettingUp is a dead
      // end reachable only by the one provisioning attempt at detection time.
      if (config_.GetBool("auto_setup")) TryProvision(*existing, runners, games_, events_);
      continue;  // already known; never re-detect over a user's configuration
    }

    const Detector::Result detected = detector.Detect(dir);
    const model::Game game = auto_setup.CreateGame(dir, detected);
    ++summary.added;
    summary.added_games.push_back(game);
    log::Info("detected new game: {}", install_path);

    // With auto_setup off, a game is still detected and stored (so it shows
    // up for the frontend to configure) but never auto-provisioned.
    // Only Windows games reach here still SettingUp (AutoSetup marks native
    // ready immediately, broken if nothing was found). Provisioning blocks —
    // umu/Proton's first-run init is a real few-second cost — but there's no
    // job queue yet to move it off this thread; see docs/architecture.md.
    if (config_.GetBool("auto_setup")) TryProvision(game, runners, games_, events_);
  }

  // Anything previously known under this root but not seen this pass has
  // disappeared. Default: mark it, don't delete it, so configuration and
  // playtime survive an unplugged drive or a temporarily-offline network
  // share. `library.remove_missing` trades that safety net for an
  // always-current list — still never touches the game's files themselves,
  // same as DELETE /v1/games/{id}.
  const bool remove_missing = config_.GetBool("library.remove_missing");
  for (const model::Game& game : games_.All()) {
    if (fs::path(game.install_path).parent_path() != root) continue;
    if (std::ranges::find(seen_install_paths, game.install_path) != seen_install_paths.end()) continue;

    // Checked before the already-Missing skip below, not after: otherwise
    // turning the toggle on would only ever catch a game the *next* time it
    // disappears, leaving anything already sitting at Missing stuck there
    // forever — surprising, since the whole point of flipping it on is to
    // clean up what's already gone.
    if (remove_missing) {
      auto removed = games_.Remove(game.id);
      if (!removed) {
        log::Error("failed to remove missing game {}: {}", game.id, removed.error().message);
        continue;
      }
      events_.Publish("game.removed", {{"id", game.id}});
      ++summary.missing;
      log::Info("game folder disappeared, removing (library.remove_missing): {}", game.install_path);
      continue;
    }

    if (game.status == model::GameStatus::Missing) continue;
    auto result = games_.Update(game.id, [](model::Game& g) { g.status = model::GameStatus::Missing; });
    if (!result) {
      log::Error("failed to mark {} missing: {}", game.id, result.error().message);
    } else {
      // Same reasoning as the restore/remove-missing cases above: a listener
      // needs to hear about this without a caller having to relist.
      events_.Publish("game.updated", model::ToJson(*result));
    }
    ++summary.missing;
    log::Info("game folder disappeared, marking missing: {}", game.install_path);
  }

  // The application menu follows the library: a game that just became
  // launchable gains an entry, one that vanished loses it.
  if (auto synced = desktop::DesktopEntries(config_).Sync(games_.All()); !synced) {
    log::Warn("could not update application menu entries: {}", synced.error().message);
  }

  return summary;
}

}  // namespace mira::library
