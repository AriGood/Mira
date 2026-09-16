#pragma once

#include <filesystem>

#include "api/EventBus.h"
#include "config/Config.h"
#include "store/GameStore.h"

namespace mira::library {

struct ScanSummary {
  int added = 0;    // new games detected and auto-configured
  int missing = 0;  // previously-known games whose folder is now gone
  int restored = 0; // a previously-missing game's folder reappeared
};

// Walks every enabled library root one level deep — each immediate
// subdirectory is treated as one game, matching "drop a game folder in and
// it's picked up". Recursing further is Detector's job, scoped to inside a
// single already-identified game folder.
//
// A directory already known to GameStore (by install_path) is never
// re-detected: once a game exists, only PATCH or the (planned) `resetup`
// endpoint touches its configuration. Scan only adds new games and
// reconciles missing/restored ones.
class Scanner {
public:
  Scanner(config::Config& config, store::GameStore& games, api::EventBus& events);

  ScanSummary ScanAll();
  ScanSummary ScanRoot(const std::filesystem::path& root);

private:
  config::Config& config_;
  store::GameStore& games_;
  api::EventBus& events_;
};

}  // namespace mira::library
