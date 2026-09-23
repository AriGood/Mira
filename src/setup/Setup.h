#pragma once

#include <filesystem>
#include <vector>

#include "core/Result.h"

namespace mira::setup {

// Where `mira setup` writes its user-level integration files.
struct SetupPaths {
  std::filesystem::path bin_dir;           // ~/.local/bin
  std::filesystem::path applications_dir;  // $XDG_DATA_HOME/applications
  std::filesystem::path icons_dir;         // $XDG_DATA_HOME/icons
  std::filesystem::path systemd_dir;       // $XDG_CONFIG_HOME/systemd/user
};

SetupPaths DefaultPaths();

// Writes a `mira` wrapper (with `mirad`/`mira-run` symlinks) that runs the
// binaries bundled in `appimage`, a desktop entry, the icon, and a systemd
// user unit. Refuses to replace a `mira` in bin_dir it didn't write.
// Returns the paths written.
Result<std::vector<std::filesystem::path>> Install(const SetupPaths& paths, const std::filesystem::path& appimage,
                                                   const std::filesystem::path& icon);

// Removes what Install wrote. Returns the paths removed.
Result<std::vector<std::filesystem::path>> Remove(const SetupPaths& paths);

}  // namespace mira::setup
