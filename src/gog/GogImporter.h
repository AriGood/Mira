#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "api/EventBus.h"
#include "config/Config.h"
#include "core/Result.h"
#include "model/Types.h"
#include "store/GameStore.h"

namespace mira::gog {

struct GogImportSummary {
  int added = 0;
  int updated = 0;
  std::vector<model::Game> added_games;
};

// Unlike epic::EpicImporter, gogdl has no "list what's installed" of its
// own to read through (see Gog.h's class comment) — the only source of
// truth for "what GOG games are here" is gog.install_root, the directory
// Mira itself installs into (GogInstaller.h). Import() walks that
// directory's immediate subdirectories and runs `gogdl import <dir>` on
// each one to identify it, so a game GogInstaller already put there is
// picked up the same way on a fresh `mirad` start as any other source's
// pre-existing install, and a game a user drops in by hand (e.g. moved
// from another GOG tool) is picked up too.
class GogImporter {
public:
  GogImporter(config::Config& config, store::GameStore& games, api::EventBus& events);

  Result<GogImportSummary> Import();

  // Identifies and upserts one already-unpacked install at `path`,
  // without touching the rest of install_root — what GogInstaller calls
  // right after a fresh download (which already knows `id`, GOG's own
  // product id, from what it just asked gogdl to download), and what
  // `mira gog import <id> <path>` uses for an install living somewhere
  // else entirely. `gogdl import <path>` fills in the title (best-effort
  // — its exact output hasn't been confirmed live against a real account
  // yet; a missing/unparseable title just falls back to `id`, same as
  // every other source here does).
  Result<model::Game> ImportPath(const std::string& id, const std::filesystem::path& path);

private:
  config::Config& config_;
  store::GameStore& games_;
  api::EventBus& events_;
};

}  // namespace mira::gog
