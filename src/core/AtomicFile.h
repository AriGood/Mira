#pragma once

#include <filesystem>
#include <string_view>

#include "core/Result.h"

namespace mira {

// Writes `content` beside `path` and renames it over, so a failed write (a full
// disk included) never replaces a working file with a truncated one. Creates
// the parent folder. `code` names the error on failure.
// Writes through a temp file and rename. `durable` fsyncs before the rename.
Result<void> WriteFileAtomic(const std::filesystem::path& path, std::string_view content, std::string code,
                             bool durable = false);

}  // namespace mira
