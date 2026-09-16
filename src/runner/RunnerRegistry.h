#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>

#include "config/Config.h"
#include "core/Command.h"
#include "core/Result.h"
#include "model/Types.h"
#include "runner/IRunner.h"

namespace mira::runner {

// Owns every IRunner and resolves a "kind:name" reference (name may be
// "auto"/"latest" for the newest discovered build) to a concrete runner +
// build. The one place a game's runner_ref gets turned into actual work.
class RunnerRegistry {
public:
  explicit RunnerRegistry(config::Config& config);

  std::vector<model::RunnerBuild> DiscoverAll() const;

  struct Resolved {
    const IRunner* runner;
    std::optional<model::RunnerBuild> build;  // unset for a runner with no builds (native)
  };
  Result<Resolved> Resolve(const std::string& runner_ref) const;

  // Resolves game.runner_ref (or, if unset, default_runner.<platform>),
  // pins the concrete reference onto the returned copy, and provisions it.
  // Can't fail in the Result sense — "no runner found" or "provisioning
  // failed" are both normal outcomes, reflected as status=broken with
  // last_error set, same as AutoSetup does for "no executable found".
  // Doesn't persist anything; the caller saves the result via GameStore.
  model::Game ProvisionGame(model::Game game) const;

private:
  config::Config& config_;
  std::map<std::string, std::unique_ptr<IRunner>> runners_;
};

}  // namespace mira::runner
