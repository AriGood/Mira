#pragma once

#include <filesystem>

namespace mira::library {

// True for a directory that is itself a Wine/Proton prefix (system.reg +
// drive_c, or a pfx/ subdirectory — umu's layout) rather than game content.
// Used two places: Scanner excludes a prefix from being treated as a game
// folder at all, and Detector must skip the same check at every level of
// its own recursive walk — a wrapper folder whose actual prefix sits one
// level deeper (e.g. umu's own <root>/umu/umu-default/) would otherwise
// have its prefix's internal .exe files (drive_c is full of them) picked up
// as "candidates" for a game that doesn't exist. Found by testing against a
// real library, not by review — see tests/library_wineprefix_test.cpp.
bool LooksLikeWinePrefix(const std::filesystem::path& dir);

}  // namespace mira::library
