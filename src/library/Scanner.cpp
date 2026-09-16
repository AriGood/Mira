#include "library/Scanner.h"

#include "core/Log.h"
#include "core/Strings.h"
#include "library/AutoSetup.h"
#include "library/Detector.h"
#include "library/WinePrefix.h"
#include "runner/RunnerRegistry.h"

namespace mira::library {
namespace {
namespace fs = std::filesystem;

DetectorSettings SettingsFromConfig(const config::Config& config) {
  DetectorSettings settings;
  settings.max_depth = static_cast<int>(config.GetInt("scan.max_depth"));
  settings.rules = config.GetStringArray("detect.rules");
  settings.name_match_bonus = config.GetDouble("detect.name_match_bonus");
  settings.depth_penalty = config.GetDouble("detect.depth_penalty");
  settings.low_confidence_threshold = config.GetDouble("detect.low_confidence_threshold");
  settings.deny_name_patterns = config.GetStringArray("detect.deny_name_patterns");
  settings.ignore_globs = config.GetStringArray("scan.ignore_globs");
  return settings;
}

}  // namespace

Scanner::Scanner(config::Config& config, store::GameStore& games, api::EventBus& events)
    : config_(config), games_(games), events_(events) {}

ScanSummary Scanner::ScanAll() {
  ScanSummary total;
  for (const fs::path& root : config_.GetPathArray("library_roots")) {
    const ScanSummary partial = ScanRoot(root);
    total.added += partial.added;
    total.missing += partial.missing;
    total.restored += partial.restored;
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
          game.status = game.exe_path.empty() ? model::GameStatus::SettingUp : model::GameStatus::Ready;
          game.last_error.clear();
        });
        if (!result) log::Error("failed to restore {}: {}", existing->id, result.error().message);
        ++summary.restored;
      }
      continue;  // already known; never re-detect over a user's configuration
    }

    const Detector::Result detected = detector.Detect(dir);
    const model::Game game = auto_setup.CreateGame(dir, detected);
    ++summary.added;
    log::Info("detected new game: {}", install_path);

    // With auto_setup off, a game is still detected and stored (so it shows
    // up for the frontend to configure) but never auto-provisioned.
    if (game.status == model::GameStatus::SettingUp && config_.GetBool("auto_setup")) {
      // Only Windows games reach here (AutoSetup marks native ready
      // immediately, broken if nothing was found). Provisioning blocks —
      // umu/Proton's first-run init is a real few-second cost — but there's
      // no job queue yet to move it off this thread; see docs/architecture.md.
      const model::Game provisioned = runners.ProvisionGame(game);
      if (auto result = games_.Upsert(provisioned); !result) {
        log::Error("failed to save provisioning result for {}: {}", provisioned.id,
                  result.error().message);
      }
      events_.Publish("game.updated", model::ToJson(provisioned));
    }
  }

  // Anything previously known under this root but not seen this pass has
  // disappeared — mark it, don't delete it, so configuration and playtime
  // survive an unplugged drive or a temporarily-offline network share.
  for (const model::Game& game : games_.All()) {
    if (fs::path(game.install_path).parent_path() != root) continue;
    if (game.status == model::GameStatus::Missing) continue;
    if (std::ranges::find(seen_install_paths, game.install_path) != seen_install_paths.end()) continue;

    auto result = games_.Update(game.id, [](model::Game& g) { g.status = model::GameStatus::Missing; });
    if (!result) log::Error("failed to mark {} missing: {}", game.id, result.error().message);
    ++summary.missing;
    log::Info("game folder disappeared, marking missing: {}", game.install_path);
  }

  return summary;
}

}  // namespace mira::library
