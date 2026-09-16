#pragma once

#include "runner/IRunner.h"

namespace mira::runner {

// Runs a native Linux binary directly. No prefix, no builds to discover.
class NativeRunner final : public IRunner {
public:
  std::string kind() const override { return "native"; }
  std::vector<model::RunnerBuild> Discover(const config::Config&) const override { return {}; }
  Result<void> Provision(const model::Game&, const std::optional<model::RunnerBuild>&) const override {
    return {};
  }
  Result<Command> BuildCommand(const model::Game& game,
                               const std::optional<model::RunnerBuild>&) const override;
};

}  // namespace mira::runner
