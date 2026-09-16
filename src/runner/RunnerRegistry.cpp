#include "runner/RunnerRegistry.h"

#include <algorithm>
#include <charconv>
#include <format>

#include "core/Log.h"
#include "runner/NativeRunner.h"
#include "runner/UmuRunner.h"
#include "runner/WineRunner.h"

namespace mira::runner {
namespace {

std::int64_t ParseVersion(const std::string& version) {
  std::int64_t value = 0;
  const auto result = std::from_chars(version.data(), version.data() + version.size(), value);
  return result.ec == std::errc{} ? value : 0;
}

}  // namespace

RunnerRegistry::RunnerRegistry(config::Config& config) : config_(config) {
  auto native = std::make_unique<NativeRunner>();
  auto umu = std::make_unique<UmuRunner>();
  auto wine = std::make_unique<WineRunner>();
  runners_[native->kind()] = std::move(native);
  runners_[umu->kind()] = std::move(umu);
  runners_[wine->kind()] = std::move(wine);
}

std::vector<model::RunnerBuild> RunnerRegistry::DiscoverAll() const {
  std::vector<model::RunnerBuild> all;
  for (const auto& [kind, runner] : runners_) {
    std::vector<model::RunnerBuild> found = runner->Discover(config_);
    all.insert(all.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
  }
  return all;
}

Result<RunnerRegistry::Resolved> RunnerRegistry::Resolve(const std::string& runner_ref) const {
  const auto colon = runner_ref.find(':');
  if (colon == std::string::npos) {
    return Err("invalid_runner_ref", std::format("\"{}\" is not \"kind:name\"", runner_ref));
  }
  const std::string kind = runner_ref.substr(0, colon);
  const std::string name = runner_ref.substr(colon + 1);

  const auto runner_it = runners_.find(kind);
  if (runner_it == runners_.end()) {
    return Err("unknown_runner_kind", std::format("no runner of kind \"{}\"", kind));
  }
  const IRunner* runner = runner_it->second.get();

  std::vector<model::RunnerBuild> builds = runner->Discover(config_);
  if (builds.empty()) return Resolved{runner, std::nullopt};  // e.g. native: no builds to pick

  if (name == "auto" || name == "latest") {
    auto newest = std::ranges::max_element(builds, {}, [](const model::RunnerBuild& build) {
      return ParseVersion(build.version);
    });
    return Resolved{runner, *newest};
  }

  const auto match = std::ranges::find(builds, name, &model::RunnerBuild::name);
  if (match == builds.end()) {
    return Err("runner_build_not_found", std::format("no \"{}\" build of \"{}\" is installed", name, kind));
  }
  return Resolved{runner, *match};
}

model::Game RunnerRegistry::ProvisionGame(model::Game game) const {
  std::string ref = game.runner_ref;
  if (ref.empty()) {
    ref = config_.GetString(game.platform == model::Platform::Native ? "default_runner.native"
                                                                     : "default_runner.windows");
  }
  // "auto" isn't itself "kind:name" — it means "the best available windows
  // runner": Proton via umu if a build is installed (gets protonfixes for
  // free), else plain Wine, else there's nothing usable and Resolve below
  // reports that clearly.
  if (ref == "auto") {
    const bool has_proton = std::ranges::any_of(
        runners_.at("proton_umu")->Discover(config_), [](const auto&) { return true; });
    ref = has_proton ? "proton_umu:latest" : "wine:latest";
  }

  const Result<Resolved> resolved = Resolve(ref);
  if (!resolved) {
    game.status = model::GameStatus::Broken;
    game.last_error = resolved.error().message;
    return game;
  }

  // Pin the concrete build, not "latest" — a Proton update afterwards must
  // not silently change a working game's runtime.
  game.runner_ref = resolved->build ? std::format("{}:{}", resolved->runner->kind(), resolved->build->name)
                                    : ref;

  const Result<void> provisioned = resolved->runner->Provision(game, resolved->build);
  if (!provisioned) {
    game.status = model::GameStatus::Broken;
    game.last_error = provisioned.error().message;
    log::Warn("provisioning {} failed: {}", game.id, provisioned.error().message);
  } else {
    game.status = model::GameStatus::Ready;
    game.last_error.clear();
  }
  return game;
}

}  // namespace mira::runner
