#pragma once

#include <filesystem>
#include <string>

namespace mira::paths {

// Everything the user might want to back up or put under version control lives
// in one directory: $XDG_CONFIG_HOME/mira (default ~/.config/mira). This is a
// deliberate departure from the usual XDG three-way config/data/cache split —
// backup-friendliness was asked for explicitly, and a single folder of TOML
// files serves that better than state scattered across three directories.
std::filesystem::path UserDir();

// The UDS socket is transient IPC, not user data, so it stays under
// XDG_RUNTIME_DIR rather than in UserDir().
std::filesystem::path RuntimeDir();

std::filesystem::path Home();

std::filesystem::path SettingsFile();  // <UserDir>/settings.toml
std::filesystem::path GamesFile();     // <UserDir>/games.toml
std::filesystem::path DefaultSocket(); // <RuntimeDir>/mirad.sock

// Expands a leading "~" and any $VAR references, so config files can be
// written the way a user would naturally type a path.
std::filesystem::path Expand(std::string_view raw);

}  // namespace mira::paths
