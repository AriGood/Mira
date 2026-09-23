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

// gogdl (Heroic's GOG downloader) also publishes a standalone Linux
// binary per release, same shape as Legendary — confirmed against a real
// release (v1.3.0): "gogdl_linux_x86_64", no archive.
inline constexpr std::string_view kGogdlRepo = "Heroic-Games-Launcher/heroic-gogdl";
inline constexpr std::string_view kGogdlAssetPattern = "gogdl_linux_x86_64";

// butler (itch.io's own launcher-integration daemon) ships zipped, with
// two shared libraries (7z.so/libc7zip.so) alongside the binary it needs
// at runtime — confirmed against a real release (v15.31.0). See
// runner::InstallToolBinary, which extracts and searches rather than
// assuming a bare binary the way Legendary/gogdl's asset is.
inline constexpr std::string_view kButlerRepo = "itchio/butler";
inline constexpr std::string_view kButlerAssetPattern = "butler-linux-amd64.zip";

// humble-cli (unofficial Humble Bundle CLI) ships tarred, with the binary
// itself named for its own platform ("humble-cli-linux-amd64", not just
// "humble-cli") — confirmed against a real release (v0.23.2).
inline constexpr std::string_view kHumbleCliRepo = "smbl64/humble-cli";
inline constexpr std::string_view kHumbleCliAssetPattern = "humble-cli-linux-amd64.tar.gz";
inline constexpr std::string_view kHumbleCliBinaryName = "humble-cli-linux-amd64";

// nile (Heroic's Amazon Games client) ships one standalone binary per
// release, same shape as gogdl.
inline constexpr std::string_view kNileRepo = "imLinguin/nile";
inline constexpr std::string_view kNileAssetPattern = "nile_linux_x86_64";

}  // namespace mira::config::runner_sources
