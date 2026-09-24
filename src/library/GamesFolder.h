#pragma once

#include <string>

#include "config/Config.h"
#include "core/Result.h"

namespace mira::library {

// Makes `folder` Mira's games folder: the first library_roots entry (replacing
// the old first one), with prefix_root and the GOG/itch/Humble install roots
// inside it. Creates the folder. `folder` may start with "~".
Result<void> SetGamesFolder(config::Config& config, const std::string& folder);

}  // namespace mira::library
