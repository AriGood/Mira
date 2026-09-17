#pragma once

#include "runner/IRunner.h"

namespace mira::runner {

// Runs Windows games via plain system Wine — no Proton, no umu. A
// standalone alternative to ProtonRunner (see docs/architecture.md on why
// having two real implementations, not just one, is what actually proves
// IRunner is swappable), not a replacement for it: umu stays the default.
class WineRunner final : public IRunner {
public:
  std::string kind() const override { return "wine"; }
  std::vector<model::RunnerBuild> Discover(const config::Config& config) const override;
  Result<void> Provision(const model::Game& game,
                         const std::optional<model::RunnerBuild>& build) const override;
  Result<Command> BuildCommand(const model::Game& game,
                               const std::optional<model::RunnerBuild>& build) const override;
};

}  // namespace mira::runner
