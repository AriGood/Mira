#pragma once

#include <vector>

#include "api/EventBus.h"
#include "config/Config.h"
#include "core/Result.h"
#include "model/Types.h"
#include "store/GameStore.h"

namespace mira::flatpak {

struct FlatpakScanSummary {
  int added = 0;
  int updated = 0;

  // See ScanSummary::added_games (library/Scanner.h) for why a metadata
  // fetch isn't triggered from in here.
  std::vector<model::Game> added_games;
};

// Lists installed Flatpak apps (via runner::FlatpakRunner's own Discover(),
// so there's exactly one place that shells out to `flatpak list`) and
// upserts them into the same GameStore as everything else — a Flatpak game
// is a normal model::Game with runner_ref "flatpak:<app-id>", so every
// existing endpoint already works on it. Manual (POST /v1/flatpak/scan), not
// inotify-driven: mirroring SteamScanner/LutrisImporter's posture — this is
// a request-time snapshot of another tool's own data, not something Mira
// owns.
class FlatpakScanner {
public:
  FlatpakScanner(config::Config& config, store::GameStore& games, api::EventBus& events);

  Result<FlatpakScanSummary> Scan();

private:
  config::Config& config_;
  store::GameStore& games_;
  api::EventBus& events_;
};

}  // namespace mira::flatpak
