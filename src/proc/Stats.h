#pragma once

#include <filesystem>

#include "core/Result.h"
#include "proc/Session.h"

namespace mira::proc {

// Appends one completed session to stats.toml's [[session]] array.
// Single-writer (mirad only), so a
// plain read-modify-write of the whole file, matching store::GameStore::Save's
// shape, is enough — no concurrent-writer story needed. A corrupt file is
// moved aside to stats.toml.bad and started fresh, never failing the caller
// or blocking mirad startup over one bad file.
Result<void> AppendSession(const std::filesystem::path& stats_file, const SessionRecord& record);

}  // namespace mira::proc
