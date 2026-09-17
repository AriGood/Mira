#pragma once

#include "runner/IRunner.h"

namespace mira::runner {

// Runs Windows games through Proton — any build: Proton-GE, Proton-CachyOS,
// UMU-Proton, Valve's own.
//
// umu-run is the *mechanism*, not the identity: Proton outside Steam needs
// the Steam Linux Runtime container and a pile of STEAM_COMPAT_* env that
// umu supplies, plus protonfixes for free. Running `proton run` directly is
// possible and loses both, so umu is used whenever it's installed. That is
// an implementation detail of this file — nothing outside it knows umu
// exists (see docs/architecture.md, Replaceability).
class ProtonRunner final : public IRunner {
public:
  std::string kind() const override { return "proton"; }
  std::vector<model::RunnerBuild> Discover(const config::Config& config) const override;
  Result<void> Provision(const model::Game& game,
                         const std::optional<model::RunnerBuild>& build) const override;
  Result<Command> BuildCommand(const model::Game& game,
                               const std::optional<model::RunnerBuild>& build) const override;
  nlohmann::json SettingsSchema() const override;
};

}  // namespace mira::runner
