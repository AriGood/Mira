#pragma once

#include "runner/IRunner.h"

namespace mira::runner {

// Runs something through the exact Proton build and prefix Steam itself
// already set up — game.data_dir is Steam's own compatdata/<appid>
// directory, not one Mira created. "Which Proton build" isn't a choice
// here: it's resolved fresh from compat_data_dir/config_info at
// BuildCommand time, the same way Steam itself would, so there is nothing
// to Discover() and no build to pick.
//
// Used for a Steam game's steam.launch_mode == "direct" (Mira launches it
// itself, normal ProcessSupervisor tracking) and for running an arbitrary
// exe inside a Steam-owned prefix — steam.launch_mode == "steam" (the
// default) never reaches this class at all, since that mode asks the
// Steam client to launch it via steam://rungameid/<appid> instead.
class SteamRunner : public IRunner {
public:
  std::string kind() const override { return "steam"; }

  std::vector<model::RunnerBuild> Discover(const config::Config& config) const override;
  bool UsesBuilds() const override { return false; }

  Result<void> Provision(const model::Game& game,
                         const std::optional<model::RunnerBuild>& build) const override;
  Result<Command> BuildCommand(const model::Game& game,
                               const std::optional<model::RunnerBuild>& build) const override;
};

}  // namespace mira::runner
