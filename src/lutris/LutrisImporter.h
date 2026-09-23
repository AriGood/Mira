#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "api/EventBus.h"
#include "config/Config.h"
#include "core/Result.h"
#include "model/Types.h"
#include "store/GameStore.h"

namespace mira::lutris {

// Lutris's own data dir (~/.local/share/lutris, or lutris.data_dir if set),
// found by locating pga.db inside it. Shared with metadata::FetchLutrisOwned,
// which joins Lutris's own cached banner/coverart/icon files by slug under
// this same directory.
std::optional<std::filesystem::path> FindLutrisDataDir(const config::Config& config);

struct LutrisImportSummary {
  int added = 0;
  int updated = 0;
  int skipped = 0;  // rows Lutris runs through something other than wine (steam, linux, ...)

  // See ScanSummary::added_games (library/Scanner.h) for why a metadata
  // fetch isn't triggered from in here.
  std::vector<model::Game> added_games;
};

// Reads Lutris's own game database (pga.db, sqlite) and per-game YAML
// configs and upserts them into the same GameStore as everything else — a
// Lutris game is a normal model::Game, so every existing endpoint already
// works on it. Manual (POST /v1/lutris/import), not inotify-driven: Lutris
// owns this data, Mira only reads a snapshot of it on request.
class LutrisImporter {
public:
  LutrisImporter(config::Config& config, store::GameStore& games, api::EventBus& events);

  Result<LutrisImportSummary> Import();

private:
  config::Config& config_;
  store::GameStore& games_;
  api::EventBus& events_;
};

}  // namespace mira::lutris
