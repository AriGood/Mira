#pragma once

// Compiled-in defaults for where Mira downloads runner builds from — same
// treatment as KnownExePatterns.h: plain data, not user-editable settings
// (those are the runner_sources.*.repo / .*.asset_pattern schema keys this
// feeds as defaults, in case the URL itself needs to change without a
// rebuild — see Schema.cpp).

#include <string_view>

namespace mira::config::runner_sources {

// GitHub "owner/repo" to list releases of, and the glob a release's asset
// list is filtered to (excludes AArch64 builds, checksum files, etc. — Proton-GE
// in particular ships both x86_64 and aarch64 tarballs per release).
inline constexpr std::string_view kProtonGERepo = "GloriousEggroll/proton-ge-custom";
inline constexpr std::string_view kProtonGEAssetPattern = "*x86_64.tar.gz";

inline constexpr std::string_view kWineGERepo = "GloriousEggroll/wine-ge-custom";
inline constexpr std::string_view kWineGEAssetPattern = "*x86_64.tar.xz";

// Legendary (a native Epic Games Store CLI client) publishes a standalone
// Linux binary per release, not an archive — see runner::InstallLegendaryBinary
// (src/epic/Legendary.h) rather than runner::DownloadAndInstall, which assumes
// a tarball.
inline constexpr std::string_view kLegendaryRepo = "derrod/legendary";
inline constexpr std::string_view kLegendaryAssetPattern = "legendary";

}  // namespace mira::config::runner_sources
