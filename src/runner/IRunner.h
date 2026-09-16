#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/Command.h"
#include "core/Result.h"
#include "model/Types.h"

namespace mira::config {
class Config;
}

namespace mira::runner {

// One way to run a game: native exec, or Proton via umu today. Every
// umu/Proton-specific detail lives inside UmuRunner — this interface knows
// none of it, so a future custom Proton runner is a new file, not a
// redesign (see docs/architecture.md, Replaceability).
class IRunner {
public:
  virtual ~IRunner() = default;

  virtual std::string kind() const = 0;

  // Installed builds of this kind (Proton versions, for umu). Empty for a
  // runner with no such concept (native).
  virtual std::vector<model::RunnerBuild> Discover(const config::Config& config) const = 0;

  // Sets up whatever the game needs before it can launch (a Wine prefix).
  // No-op for runners that need nothing.
  virtual Result<void> Provision(const model::Game& game,
                                 const std::optional<model::RunnerBuild>& build) const = 0;

  virtual Result<Command> BuildCommand(const model::Game& game,
                                       const std::optional<model::RunnerBuild>& build) const = 0;
};

}  // namespace mira::runner
