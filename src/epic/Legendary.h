#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <json.hpp>

#include "config/Config.h"
#include "core/Result.h"
#include "runner/Downloader.h"

// Epic Games Store support: wraps Legendary, a native-Linux Epic CLI client,
// for everything protocol-shaped (auth, catalog, install, update, uninstall).
// Mira never runs Legendary's own `legendary launch` — once a game is
// installed, it's launched through Mira's own Wine/Proton runners like any
// other Windows game (see docs/architecture.md). This file only knows how to
// find or fetch the `legendary` binary and run it; it has no game-library
// knowledge (see EpicImporter/EpicInstaller for that).
namespace mira::epic {

struct LegendaryStatus {
  bool installed = false;
  std::string source;  // "override" | "managed" | "path" | "none"
  std::string path;
  std::string version;
};

// Where Mira's own managed download lives, if it ever fetches one — never
// requires the file to actually exist.
std::filesystem::path ManagedLegendaryPath(const config::Config& config);

// Detects legendary in this order: epic.legendary_bin override, Mira's own
// managed download, then $PATH. Never itself requires legendary to already
// work — safe to call before anything is set up, unlike everything below.
LegendaryStatus DetectLegendary(const config::Config& config);

// Downloads legendary's latest matching GitHub release asset (a standalone
// binary, not an archive) into ManagedLegendaryPath and marks it executable.
// Reuses runner::ListReleases (config kind "legendary") rather than
// runner::DownloadAndInstall, which assumes a tarball extracted into a
// runner search path — neither fits a single raw binary with nowhere else to
// live.
Result<void> InstallLegendaryBinary(const config::Config& config, const runner::ReleaseAsset& asset);

// Runs `legendary <args...>` using whatever DetectLegendary resolved.
// Err("legendary_missing", ...) up front if nothing resolved, pointing at
// "mira epic setup" — never assumes legendary is there. Returns combined
// stdout+stderr; Err("legendary_failed", ...) on a nonzero exit.
Result<std::string> RunLegendary(const config::Config& config, std::vector<std::string> args);

// Like RunLegendary, but appends --json and parses stdout as JSON.
Result<nlohmann::json> RunLegendaryJson(const config::Config& config, std::vector<std::string> args);

}  // namespace mira::epic
