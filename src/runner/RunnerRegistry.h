#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "config/Config.h"
#include "core/Command.h"
#include "core/Result.h"
#include "model/Types.h"
#include "runner/IRunner.h"

namespace mira::runner {

// Collapses builds that are the same runner found more than once — the same
// directory reached through a symlinked search path, or two entries sharing
// a "kind:name" reference, which no client can tell apart. Order is
// preserved and the first occurrence wins. Exposed for its own test; every
// discovery goes through it.
std::vector<model::RunnerBuild> DeduplicateBuilds(std::vector<model::RunnerBuild> builds);

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
  // Discovered builds for one kind, computed once per registry instance.
  const std::vector<model::RunnerBuild>& BuildsFor(const std::string& kind) const;

  config::Config& config_;
  std::map<std::string, std::unique_ptr<IRunner>> runners_;

  mutable std::mutex cache_mutex_;
  mutable std::map<std::string, std::vector<model::RunnerBuild>> cache_;
};

}  // namespace mira::runner
