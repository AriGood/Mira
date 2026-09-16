#pragma once

#include "runner/IRunner.h"

namespace mira::runner {

// Runs Windows games via umu-run (github.com/Open-Wine-Components/umu-launcher),
// which wraps Proton without needing Steam. Every umu/Proton-specific detail
// (WINEPREFIX, PROTONPATH, GAMEID, the "" trick to init a prefix with no
// game) lives in this one file — see docs/architecture.md, Replaceability.
class UmuRunner final : public IRunner {
public:
  std::string kind() const override { return "proton_umu"; }
  std::vector<model::RunnerBuild> Discover(const config::Config& config) const override;
  Result<void> Provision(const model::Game& game,
                         const std::optional<model::RunnerBuild>& build) const override;
  Result<Command> BuildCommand(const model::Game& game,
                               const std::optional<model::RunnerBuild>& build) const override;
};

}  // namespace mira::runner
