#pragma once

#include <filesystem>

#include "config/Config.h"
#include "model/Types.h"

namespace mira::library {

// A game's name-or-id leaf directory under `root`, per the prefix_naming
// setting: game.name slugified via strings::Slugify (with a "-2", "-3", ...
// suffix on a real directory collision under `root`) when "name" (the
// default; a name with no letters or digits gets Slugify's own "game"
// fallback), or game.id verbatim when "id". Shared by PrefixDir below and
// library::Relocate's canonical install_path (under a library root instead
// of prefix_root).
std::filesystem::path NamedDir(const config::Config& config, const model::Game& game,
                               const std::filesystem::path& root);

// Where a newly-provisioned game's data directory goes: NamedDir under
// prefix_root. Only for a fresh provision -- an already-provisioned game's
// data_dir is left alone until explicitly relocated (see library/Relocate.h).
std::filesystem::path PrefixDir(const config::Config& config, const model::Game& game);

}  // namespace mira::library
