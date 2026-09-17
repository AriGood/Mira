#pragma once

#include <filesystem>

#include "core/Result.h"

namespace mira::library {

// True if `path`'s extension is a recognized archive type — regardless of
// whether the tool needed to actually extract it is installed (see
// ExtractAndRemove, which is where that's checked and reported).
bool LooksLikeArchive(const std::filesystem::path& path);

// `archive`'s filename with its recognized archive suffix removed — exactly
// the matched suffix (".tar.gz", ".zip", ...), not repeated std::filesystem
// ::path::stem() calls, which would mis-split a name with its own dots in
// it (e.g. "My.Game.zip"). Returns the filename unchanged if it isn't a
// recognized archive at all.
std::filesystem::path StemWithoutArchiveExtension(const std::filesystem::path& archive);

// Extracts `archive` into `dest_dir` (created if it doesn't exist yet)
// using whichever external tool its extension needs (tar, unzip, unrar,
// 7z), then deletes `archive` — but only on success. Nothing is deleted or
// left half-extracted on failure, including a missing extractor tool: that
// fails with a clear, specific message (which package to install) rather
// than silently doing nothing, same as a missing runner build does
// elsewhere in this codebase.
Result<void> ExtractAndRemove(const std::filesystem::path& archive, const std::filesystem::path& dest_dir);

}  // namespace mira::library
