#include "runner/Downloader.h"

#include <filesystem>
#include <format>
#include <optional>

#include <json.hpp>

#include "core/Log.h"
#include "core/Strings.h"
#include "runner/Exec.h"

namespace mira::runner {
namespace {
namespace fs = std::filesystem;
using nlohmann::json;

struct Source {
  std::string repo;
  std::string asset_pattern;
};

Result<Source> SourceFor(const config::Config& config, const std::string& kind) {
  if (kind == "proton") {
    return Source{.repo = config.GetString("runner_sources.proton_ge.repo"),
                 .asset_pattern = config.GetString("runner_sources.proton_ge.asset_pattern")};
  }
  if (kind == "wine") {
    return Source{.repo = config.GetString("runner_sources.wine_ge.repo"),
                 .asset_pattern = config.GetString("runner_sources.wine_ge.asset_pattern")};
  }
  // Not a runner kind — Legendary is the Epic Games Store CLI client
  // (see src/epic/Legendary.h) — but it shares the same "list a GitHub
  // repo's releases, filter assets by glob" shape, so it reuses ListReleases
  // rather than duplicating the GitHub API call.
  if (kind == "legendary") {
    return Source{.repo = config.GetString("runner_sources.legendary.repo"),
                 .asset_pattern = config.GetString("runner_sources.legendary.asset_pattern")};
  }
  if (kind == "gog") {
    return Source{.repo = config.GetString("runner_sources.gog.repo"),
                 .asset_pattern = config.GetString("runner_sources.gog.asset_pattern")};
  }
  if (kind == "itch") {
    return Source{.repo = config.GetString("runner_sources.itch.repo"),
                 .asset_pattern = config.GetString("runner_sources.itch.asset_pattern")};
  }
  if (kind == "amazon") {
    return Source{.repo = config.GetString("runner_sources.amazon.repo"),
                 .asset_pattern = config.GetString("runner_sources.amazon.asset_pattern")};
  }
  if (kind == "humble") {
    return Source{.repo = config.GetString("runner_sources.humble.repo"),
                 .asset_pattern = config.GetString("runner_sources.humble.asset_pattern")};
  }
  return Err("unknown_runner_kind", std::format("no downloadable source for kind \"{}\"", kind));
}

std::filesystem::path InstallDirFor(const config::Config& config, const std::string& kind) {
  const auto paths = config.GetPathArray(kind == "proton" ? "runner_search_paths" : "wine_search_paths");
  return paths.empty() ? fs::path() : paths.front();
}

// A release's matching checksum asset, if it shipped one — same stem as
// the tarball, ".sha512sum" instead of its archive extension.
std::string FindChecksumUrl(const json& assets, const std::string& tarball_name) {
  const std::string stem = fs::path(tarball_name).stem().string();  // strips one extension (.gz/.xz)
  for (const auto& asset : assets) {
    const std::string name = asset.value("name", std::string());
    if (name.ends_with(".sha512sum") && name.starts_with(fs::path(stem).stem().string())) {
      return asset.value("browser_download_url", std::string());
    }
  }
  return {};
}

// Downloads `asset` to `target` and verifies it against
// asset.checksum_url if the release had one -- the part DownloadAndInstall
// and InstallToolBinary share before they diverge on what to do with the
// downloaded file (extract a tarball vs. chmod a bare/extracted binary).
// `target`'s own filename must equal asset.asset_name for the checksum
// file's recorded name to match what sha512sum -c finds on disk --
// callers that want a different final name rename after this returns.
Result<void> DownloadVerified(const ReleaseAsset& asset, const fs::path& target) {
  std::error_code ec;
  Command download;
  download.argv = {"curl", "-sSL", "-o", target.string(), asset.download_url};
  if (Result<ExecResult> result = RunAndWait(download); !result || result->exit_code != 0) {
    fs::remove(target, ec);
    return Err("download_failed",
              !result ? result.error().message : std::format("curl exited {}: {}", result->exit_code, result->output));
  }

  if (asset.checksum_url.empty()) {
    log::Warn("no checksum available for {} — installing unverified", asset.asset_name);
    return {};
  }

  const fs::path checksum_file = target.parent_path() / (asset.asset_name + ".sha512sum");
  Command fetch_checksum;
  fetch_checksum.argv = {"curl", "-sSL", "-o", checksum_file.string(), asset.checksum_url};
  if (Result<ExecResult> result = RunAndWait(fetch_checksum); !result || result->exit_code != 0) {
    fs::remove(target, ec);
    fs::remove(checksum_file, ec);
    return Err("checksum_fetch_failed", "couldn't fetch the checksum file to verify the download");
  }

  Command verify;
  verify.argv = {"sha512sum", "-c", checksum_file.filename().string()};
  verify.cwd = target.parent_path();
  const Result<ExecResult> verified = RunAndWait(verify);
  fs::remove(checksum_file, ec);
  if (!verified || verified->exit_code != 0) {
    fs::remove(target, ec);
    return Err("checksum_mismatch",
              "downloaded file's checksum didn't match — discarded rather than installing a "
              "corrupted or tampered build");
  }
  return {};
}

// Recursively searches `dir` for a regular file named `name`, for pulling
// a known binary out of an archive whose internal layout isn't assumed
// (e.g. butler-linux-amd64.zip nests everything under "linux-amd64/").
std::optional<fs::path> FindFileNamed(const fs::path& dir, const std::string& name) {
  std::error_code ec;
  for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
    if (entry.is_regular_file(ec) && entry.path().filename() == name) return entry.path();
  }
  return std::nullopt;
}

bool IsZip(const std::string& asset_name) { return asset_name.ends_with(".zip"); }
bool IsTarball(const std::string& asset_name) {
  return asset_name.ends_with(".tar.gz") || asset_name.ends_with(".tar.xz") || asset_name.ends_with(".tgz");
}

Result<void> Chmod(const fs::path& path) {
  std::error_code ec;
  fs::permissions(path,
                  fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                      fs::perms::others_exec,
                  ec);
  if (ec) return Err("chmod_failed", ec.message());
  return {};
}

}  // namespace

Result<std::vector<ReleaseAsset>> ListReleases(const config::Config& config, const std::string& kind) {
  const Result<Source> source = SourceFor(config, kind);
  if (!source) return std::unexpected(source.error());

  Command command;
  command.argv = {"curl", "-sSL", std::format("https://api.github.com/repos/{}/releases?per_page=10", source->repo)};
  const Result<ExecResult> result = RunAndWait(command);
  if (!result) return std::unexpected(result.error());

  const json parsed = json::parse(result->output, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_array()) {
    return Err("github_api_error", std::format("couldn't list releases for {}: {}", source->repo, result->output));
  }

  std::vector<ReleaseAsset> releases;
  for (const auto& release : parsed) {
    const json& assets = release.value("assets", json::array());
    for (const auto& asset : assets) {
      const std::string name = asset.value("name", std::string());
      if (!strings::GlobMatch(source->asset_pattern, name)) continue;

      ReleaseAsset entry;
      entry.tag = release.value("tag_name", std::string());
      entry.asset_name = name;
      entry.download_url = asset.value("browser_download_url", std::string());
      entry.size_bytes = asset.value("size", std::int64_t{0});
      entry.published_at = release.value("published_at", std::string());
      entry.checksum_url = FindChecksumUrl(assets, name);
      releases.push_back(std::move(entry));
      break;  // one matching asset per release is expected
    }
  }
  return releases;
}

Result<void> DownloadAndInstall(const config::Config& config, const std::string& kind,
                                const ReleaseAsset& asset) {
  const fs::path install_dir = InstallDirFor(config, kind);
  if (install_dir.empty()) {
    return Err("no_search_path", std::format("no {} search path configured to install into", kind));
  }
  std::error_code ec;
  fs::create_directories(install_dir, ec);
  if (ec) return Err("install_dir_failed", ec.message());

  // Downloaded into the install dir itself so a same-filesystem rename
  // isn't a concern and there's nothing to clean up across filesystems —
  // both the archive and (if present) its checksum file are removed again
  // once extraction succeeds.
  const fs::path archive = install_dir / asset.asset_name;
  if (auto downloaded = DownloadVerified(asset, archive); !downloaded) return downloaded;

  Command extract;
  extract.argv = {"tar", "-xf", archive.string(), "-C", install_dir.string()};
  const Result<ExecResult> extracted = RunAndWait(extract);
  fs::remove(archive, ec);
  if (!extracted || extracted->exit_code != 0) {
    return Err("extract_failed",
              !extracted ? extracted.error().message
                        : std::format("tar exited {}: {}", extracted->exit_code, extracted->output));
  }
  return {};
}

Result<fs::path> InstallToolBinary(const config::Config& config, const std::string& tool_name,
                                   const ReleaseAsset& asset, const std::string& binary_name) {
  const fs::path tool_dir = config.File().parent_path() / "tools" / tool_name;
  std::error_code ec;
  fs::create_directories(tool_dir, ec);
  if (ec) return Err("install_dir_failed", ec.message());

  const fs::path downloaded = tool_dir / asset.asset_name;
  if (auto result = DownloadVerified(asset, downloaded); !result) return std::unexpected(result.error());

  const fs::path target = tool_dir / binary_name;
  if (IsZip(asset.asset_name) || IsTarball(asset.asset_name)) {
    Command extract;
    if (IsZip(asset.asset_name)) {
      extract.argv = {"unzip", "-o", "-q", downloaded.string(), "-d", tool_dir.string()};
    } else {
      extract.argv = {"tar", "-xf", downloaded.string(), "-C", tool_dir.string()};
    }
    const Result<ExecResult> extracted = RunAndWait(extract);
    fs::remove(downloaded, ec);
    if (!extracted || extracted->exit_code != 0) {
      return Err("extract_failed", !extracted ? extracted.error().message
                                              : std::format("extract exited {}: {}", extracted->exit_code,
                                                            extracted->output));
    }
    const auto found = FindFileNamed(tool_dir, binary_name);
    if (!found) {
      return Err("binary_not_found",
                std::format("{} didn't contain a file named \"{}\"", asset.asset_name, binary_name));
    }
    if (auto chmodded = Chmod(*found); !chmodded) return std::unexpected(chmodded.error());
    return *found;
  }

  fs::rename(downloaded, target, ec);
  if (ec) {
    fs::remove(downloaded, ec);
    return Err("install_failed", ec.message());
  }
  if (auto chmodded = Chmod(target); !chmodded) return std::unexpected(chmodded.error());
  return target;
}

}  // namespace mira::runner
