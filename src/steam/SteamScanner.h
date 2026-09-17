#pragma once

#include "api/EventBus.h"
#include "config/Config.h"
#include "core/Result.h"
#include "store/GameStore.h"

namespace mira::steam {

struct SteamScanSummary {
  int added = 0;
  int updated = 0;
};

// Detects installed Steam games and upserts them into the same GameStore
// as everything else — a Steam game is a normal model::Game with
// runner_ref "steam:<appid>", so every existing endpoint (GET /v1/games,
// PATCH, launch, stop) already works on it without modification. Manual
// (POST /v1/steam/scan), not inotify-driven: Steam's own library isn't
// under a watched library_root, and appearing/vanishing games there is
// nowhere near as frequent as it is for a game folder.
class SteamScanner {
public:
  SteamScanner(config::Config& config, store::GameStore& games, api::EventBus& events);

  Result<SteamScanSummary> Scan();

private:
  config::Config& config_;
  store::GameStore& games_;
  api::EventBus& events_;
};

}  // namespace mira::steam
