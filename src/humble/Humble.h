#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "config/Config.h"
#include "core/Result.h"
#include "runner/Downloader.h"

// Wraps humble-cli (github.com/smbl64/humble-cli, unofficial), the one
// real native-Linux CLI for Humble Bundle. Deliberately not built on
// library::ILibrarySource the way epic/steam/gog/itch are: Humble Bundle
// has no "installed" concept of its own at all, just purchased bundles of
// downloadable files (a mix of installers, archives, and DRM-free
// builds), so there's no catalog/install/update lifecycle to wrap —
// GET /v1/humble/library lists what's purchased, POST
// /v1/humble/download fetches files from one bundle into a plain
// directory, and that's the whole surface. Confirmed live against a real
// release (v0.23.2, no account needed for --help): humble-cli has no
// --json output, only human-readable tables (`--field` selects columns) —
// parsed defensively (see HumbleSource.cpp... actually Humble.cpp's
// ParseTable) rather than assumed stable.
namespace mira::humble {

struct HumbleStatus {
  bool installed = false;
  std::string source;  // "override" | "managed" | "path" | "none"
  std::string path;
  std::string version;
};

std::filesystem::path ManagedHumbleCliPath(const config::Config& config);

HumbleStatus DetectHumbleCli(const config::Config& config);

Result<void> InstallHumbleCliBinary(const config::Config& config, const runner::ReleaseAsset& asset);

Result<std::string> RunHumbleCli(const config::Config& config, std::vector<std::string> args);

struct HumbleAuthStatus {
  HumbleStatus humble_cli;
  bool authenticated = false;
};

// Unlike epic/gog, there's no separate "is a token stored" file to check —
// humble-cli owns its own config file entirely. The only way to know if
// it's authenticated is to actually ask it something (`list`), so this
// one *does* shell out, unlike epic::Status/gog::Status's "installed
// check is free" posture.
HumbleAuthStatus Status(const config::Config& config);

// `session_key` is the _simpleauth_sess cookie value, copied from a
// logged-in browser session (documented in humble-cli's own README —
// there's no login URL/code flow the way Epic/GOG have).
Result<void> Login(const config::Config& config, const std::string& session_key);

struct BundleSummary {
  std::string key;
  std::string name;
  bool claimed = false;
};

Result<std::vector<BundleSummary>> ListBundles(const config::Config& config);

// Where one bundle's downloaded items land: "<humble.download_root>/<bundle_key>/".
// Not auto-imported as a model::Game -- Humble's items are arbitrary
// archives/installers, not a provisioned prefix (see Humble.h's class
// comment) -- the existing manual-add flow (POST /v1/games, or
// AutoSetup's detection of the extracted folder) takes it from there.
std::filesystem::path DownloadDir(const config::Config& config, const std::string& bundle_key);

// Runs `humble-cli download <bundle_key> --cur-dir` into DownloadDir(),
// optionally narrowed by `item_numbers` (humble-cli's own "1,3,5-7" range
// syntax, passed through as-is). Blocking, same convention as every other
// install-shaped call here -- the caller runs it on a detached thread.
Result<void> Download(const config::Config& config, const std::string& bundle_key, const std::string& item_numbers);

}  // namespace mira::humble
