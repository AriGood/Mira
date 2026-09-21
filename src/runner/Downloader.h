#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "config/Config.h"
#include "core/Result.h"

namespace mira::runner {

// One release asset offered for download — already filtered to the single
// asset per release that matches the source's asset_pattern (see
// RunnerSources.h / the runner_sources.* settings), so a caller never has
// to know GitHub's release/asset JSON shape.
struct ReleaseAsset {
  std::string tag;
  std::string asset_name;
  std::string download_url;
  std::string checksum_url;  // empty if the release had no matching .sha512sum asset
  std::int64_t size_bytes = 0;
  std::string published_at;
};

// Lists releases of `kind` ("proton", "wine", or "legendary" — the last one
// isn't a runner, but shares the same "GitHub repo + asset glob" source
// shape; see src/epic/Legendary.h) from its configured GitHub source, newest
// first. Shells out to curl for the GitHub API request rather than linking
// libcurl — consistent with how umu-run/wine are already run as
// subprocesses, and this is occasional, human-triggered traffic, not a hot
// path.
Result<std::vector<ReleaseAsset>> ListReleases(const config::Config& config, const std::string& kind);

// Downloads `asset` (verifying its sha512sum first, if it has one) and
// extracts it into the right search path for `kind` —
// runner_search_paths[0] for "proton", wine_search_paths[0] for "wine" —
// the same locations Proton-GE/Wine-GE's own install instructions use, so
// the very next Discover() call finds it with no extra wiring. Shells out
// to curl + tar rather than linking an archive/TLS library, for the same
// reason as ListReleases.
Result<void> DownloadAndInstall(const config::Config& config, const std::string& kind,
                                const ReleaseAsset& asset);

// Downloads and installs a tool binary that isn't a runner build (gogdl,
// butler, ...) into "<config dir>/tools/<tool_name>/<binary_name>",
// returning that path. How `asset` unpacks depends on its own name: a
// ".zip"/".tar.*" asset is extracted into that directory first and
// `binary_name` is searched for inside it (some tools ship shared
// libraries the binary needs to find alongside itself at runtime --
// butler's 7z.so/libc7zip.so, for one -- so the archive's own internal
// layout is never assumed, just searched); anything else is installed as
// the bare file directly, renamed to `binary_name`. Verifies
// asset.checksum_url first if the release has one, same as
// DownloadAndInstall.
Result<std::filesystem::path> InstallToolBinary(const config::Config& config, const std::string& tool_name,
                                               const ReleaseAsset& asset, const std::string& binary_name);

}  // namespace mira::runner
