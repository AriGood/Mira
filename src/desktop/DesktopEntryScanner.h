#pragma once

#include <string>
#include <vector>

#include "api/EventBus.h"
#include "config/Config.h"
#include "core/Result.h"
#include "model/Types.h"
#include "store/GameStore.h"

namespace mira::desktop {

// One already-installed application-menu entry that could become a game.
struct DesktopEntryCandidate {
  std::string id;    // freedesktop "desktop file ID" -- stable, no stored state needed
  std::string name;
  std::string icon;  // Icon= value verbatim, empty if absent
};

struct DesktopEntryImportSummary {
  int added = 0;
  int updated = 0;
  std::vector<model::Game> added_games;  // for the caller's metadata-fetch enqueue
};

// Reads other apps' .desktop files (never mira-*.desktop, which
// DesktopEntries itself writes) and offers them as game candidates -- the
// reverse direction of DesktopEntries. Covers Flatpak apps for free: every
// Flatpak-exported .desktop file carries X-Flatpak=<app-id>, so no
// Flatpak-specific runner or scanner is needed at all.
//
// Deliberately manual, not auto-import: ListCandidates() only lists, nothing
// is added to the library until Import() is called with specific ids picked
// by the user.
class DesktopEntryScanner {
public:
  DesktopEntryScanner(config::Config& config, store::GameStore& games, api::EventBus& events);

  Result<std::vector<DesktopEntryCandidate>> ListCandidates() const;
  Result<DesktopEntryImportSummary> Import(const std::vector<std::string>& ids);

private:
  config::Config& config_;
  store::GameStore& games_;
  api::EventBus& events_;
};

}  // namespace mira::desktop
