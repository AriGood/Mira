#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "config/Config.h"
#include "core/Result.h"
#include "runner/Downloader.h"

// Wraps gogdl (github.com/Heroic-Games-Launcher/heroic-gogdl), the GOG
// downloader Heroic itself uses. Confirmed live against a real release
// (v1.3.0, no account needed for --help): gogdl ships as a self-executing
// Python zipapp ("#!/usr/bin/env python3" shebang baked in), not a static
// binary the way Legendary/butler are -- it needs a system python3, a real
// new failure mode Legendary's own detection never had.
//
// Unlike Legendary, gogdl has no "list what's installed" or "list owned"
// subcommand at all -- {import,redist,dependencies,auth,download,repair,
// update,info,launch,save-sync,save-clear,lang-match} is the whole set
// (confirmed via `gogdl --help`). "import" reads metadata for a game
// already unpacked at a given local path; it doesn't discover installs on
// its own. Catalog/ownership instead comes from GOG's own embed.gog.com
// API directly (confirmed against heroic-gogdl's own source,
// gogdl/api.py/auth.py: GET embed.gog.com/user/data/games with the access
// token gogdl's own `auth` step already obtained and wrote to
// --auth-config-path -- see GogCatalog.h), the same way every other GOG
// Linux tool (lgogdownloader included) gets a library listing.
namespace mira::gog {

struct GogStatus {
  bool installed = false;
  std::string source;  // "override" | "managed" | "path" | "none"
  std::string path;
  std::string version;
};

std::filesystem::path ManagedGogPath(const config::Config& config);

GogStatus DetectGog(const config::Config& config);

Result<void> InstallGogBinary(const config::Config& config, const runner::ReleaseAsset& asset);

// Where Mira tells gogdl to keep its own OAuth token file
// (--auth-config-path, passed to every invocation below). Mira-managed,
// alongside settings.toml -- not shared with any other GOG client.
std::filesystem::path AuthConfigPath(const config::Config& config);

// Runs gogdl with --auth-config-path already inserted. Blocking, same
// convention as epic::RunLegendary.
Result<std::string> RunGogdl(const config::Config& config, std::vector<std::string> args);

struct GogAuthStatus {
  GogStatus gogdl;
  bool authenticated = false;
};

// installed-only if gogdl itself isn't present; otherwise checks whether
// AuthConfigPath holds an unexpired access token -- never shells out just
// to check this, unlike epic::Status (gogdl has no cheap "status" call).
GogAuthStatus Status(const config::Config& config);

// `code` is the authorization code from GOG's own login page redirect,
// same shape as Epic's pasted code. gogdl bakes in GOG Galaxy's own public
// client_id/client_secret (confirmed in heroic-gogdl's auth.py) -- Mira
// never needs its own.
Result<void> Login(const config::Config& config, const std::string& code);

// Just removes AuthConfigPath -- gogdl has no "auth --delete" the way
// Legendary does.
Result<void> Logout(const config::Config& config);

// The access token from AuthConfigPath, refreshing once via a bare
// `gogdl auth` call (no --code) if it looks expired -- gogdl's own load
// path already knows how to use its stored refresh_token, confirmed in
// auth.py's is_credential_expired/refresh_credentials; Mira just triggers
// that load by invoking gogdl. Returns Err("not_authenticated", ...) if
// refreshing doesn't produce a usable token either.
Result<std::string> AccessToken(const config::Config& config);

}  // namespace mira::gog
