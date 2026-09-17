#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "config/Config.h"

namespace mira::steam {

// One installed Steam app, found by reading Steam's own files directly —
// never by asking the Steam client, which may not even be running.
struct SteamApp {
  std::string appid;
  std::string name;
  std::filesystem::path install_dir;      // absolute
  bool is_native = false;                 // no compatdata dir -> a native Linux Steam game
  std::filesystem::path compat_data_dir;  // <library>/steamapps/compatdata/<appid>; empty if native
  std::filesystem::path proton_path;      // the "proton" script Steam actually used for this app;
                                          // empty if it couldn't be resolved (e.g. never launched yet)
  std::filesystem::path client_install_path;  // STEAM_COMPAT_CLIENT_INSTALL_PATH Steam itself used
  std::filesystem::path steam_root;
  std::filesystem::path library_root;
};

// What's needed to run something through the same Proton build Steam
// already set up a prefix with: the "proton" script itself, and the Steam
// client install path Proton needs as STEAM_COMPAT_CLIENT_INSTALL_PATH.
// Both come from the same compat_data_dir/config_info file, which is why
// they're resolved together rather than separately.
struct ProtonCompatInfo {
  std::filesystem::path proton_path;
  std::filesystem::path client_install_path;
};

// Finds the Steam installation: config's steam.root override if set,
// otherwise the two real locations in use across distros.
std::optional<std::filesystem::path> FindSteamRoot(const config::Config& config);

// Every library folder Steam knows about, including steam_root's own — read
// from libraryfolders.vdf, falling back to just steam_root if that file is
// missing or unparseable rather than finding nothing at all.
std::vector<std::filesystem::path> LibraryFolders(const std::filesystem::path& steam_root);

// Every installed app across every library folder, minus Valve's own
// tooling — compat tools (detected structurally: install_dir/proton
// exists), the fixed-appid Steamworks Common Redistributables, and Steam
// Linux Runtime containers. Never game data, so nothing here needs pruning
// by the caller.
std::vector<SteamApp> ListApps(const std::filesystem::path& steam_root);

// Resolves what a Windows app's compat_data_dir was actually set up with,
// by reading its config_info file — the same mechanism Steam itself uses,
// so it's exactly the build (and client path) that already provisioned the
// prefix. Returns nullopt if compat_data_dir has no config_info yet (the
// app has never actually been launched through Steam).
std::optional<ProtonCompatInfo> ResolveProtonCompatInfo(const std::filesystem::path& compat_data_dir);

}  // namespace mira::steam
