#pragma once

#include <optional>
#include <string>
#include <vector>

#include <json.hpp>

#include "core/Command.h"
#include "core/Result.h"
#include "model/Types.h"

namespace mira::config {
class Config;
}

namespace mira::runner {

// One way to run a game: native exec, or Proton via umu today. Every
// umu/Proton-specific detail lives inside ProtonRunner — this interface knows
// none of it, so a future custom Proton runner is a new file, not a
// redesign (see docs/architecture.md, Replaceability).
class IRunner {
public:
  virtual ~IRunner() = default;

  virtual std::string kind() const = 0;

  // Installed builds of this kind (Proton versions, for umu). Empty for a
  // runner with no such concept (native).
  virtual std::vector<model::RunnerBuild> Discover(const config::Config& config) const = 0;

  // False for a runner with no concept of separate installed builds. Without
  // this, "discovery returned nothing" and "this kind has no builds" are
  // indistinguishable, and a reference naming an uninstalled build resolves
  // as success-with-no-build instead of a clear error.
  virtual bool UsesBuilds() const { return true; }

  // Sets up whatever the game needs before it can launch (a Wine prefix).
  // No-op for runners that need nothing.
  virtual Result<void> Provision(const model::Game& game,
                                 const std::optional<model::RunnerBuild>& build) const = 0;

  virtual Result<Command> BuildCommand(const model::Game& game,
                                       const std::optional<model::RunnerBuild>& build) const = 0;

  // Declares what game.runner_config accepts for this kind, so a frontend
  // can render it generically instead of hardcoding per-runner knowledge
  // (see docs/architecture.md, Replaceability) — a custom runner with
  // entirely different knobs needs zero frontend changes. Empty for a
  // runner with no runner_config fields at all, which is most of them.
  virtual nlohmann::json SettingsSchema() const { return nlohmann::json::array(); }
};

}  // namespace mira::runner
