#pragma once

#include <filesystem>

#include "api/EventBus.h"
#include "config/Config.h"
#include "library/Detector.h"
#include "store/GameStore.h"

namespace mira::library {

// Turns one Detector::Result for a freshly-found folder into a stored,
// auto-configured model::Game, and publishes `game.added`. It does not
// provision anything (create a prefix, resolve a real runner build) — that
// is the runner layer's job, not yet built; a Windows game is stored as
// `setting_up` and simply waits there until provisioning exists. A native
// game needs no provisioning and is stored `ready` immediately.
class AutoSetup {
public:
  AutoSetup(config::Config& config, store::GameStore& games, api::EventBus& events);

  model::Game CreateGame(const std::filesystem::path& install_path, const Detector::Result& detected);

private:
  config::Config& config_;
  store::GameStore& games_;
  api::EventBus& events_;
};

}  // namespace mira::library
