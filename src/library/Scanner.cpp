#include "library/Scanner.h"

#include "core/Log.h"
#include "core/Strings.h"
#include "library/AutoSetup.h"
#include "library/Detector.h"

namespace mira::library {
namespace {
namespace fs = std::filesystem;

// True for a directory that is itself a Wine/Proton prefix (system.reg +
// drive_c, or a pfx/ subdirectory — umu's layout) rather than a game. Needed
// because the default prefix_root sits *inside* the library root: without
// this, a scan would rediscover its own prefixes as new games and provision
// prefixes for them in turn. Also protects a library root that already has
// prefixes from some other launcher sitting alongside real games.
bool LooksLikeWinePrefix(const fs::path& dir) {
  std::error_code ec;
  const bool has_registry = fs::exists(dir / "system.reg", ec);
  const bool has_drive_c = fs::exists(dir / "drive_c", ec);
  const bool has_pfx = fs::exists(dir / "pfx", ec);
  return (has_registry && has_drive_c) || has_pfx;
}

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
    auto_setup.CreateGame(dir, detected);
    ++summary.added;
    log::Info("detected new game: {}", install_path);
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
