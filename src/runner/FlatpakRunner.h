#pragma once

#include <string>
#include <vector>

#include "core/Result.h"
#include "runner/IRunner.h"

namespace mira::runner {

// One row of `flatpak list --app`. Shared with flatpak::FlatpakScanner
// (which additionally wants the friendly `name`, not just the app id
// RunnerBuild carries) so there's exactly one place that shells out to
// `flatpak list`.
struct FlatpakApp {
  std::string app_id;
  std::string name;
  std::string version;
};

// Empty (not an error) if flatpak itself isn't installed — same posture as
// ProtonRunner::Discover gating on umu-run.
Result<std::vector<FlatpakApp>> ListInstalledFlatpakApps();

// Runs a Flatpak app by ref. The installed app *is* the build — there's no
// separate "which build" choice the way Proton/Wine have (see
// docs/architecture.md, Replaceability) — so a build's `name` is the app id
// itself and `runner_ref` ends up "flatpak:<app-id>", e.g.
// "flatpak:org.libretro.RetroArch".
//
// Unlike every other runner, exe_path is never required: a Flatpak app has
// no separate executable to point at, just its own app id.
class FlatpakRunner final : public IRunner {
public:
  std::string kind() const override { return "flatpak"; }

  std::vector<model::RunnerBuild> Discover(const config::Config& config) const override;

  Result<void> Provision(const model::Game& game,
                         const std::optional<model::RunnerBuild>& build) const override;
  Result<Command> BuildCommand(const model::Game& game,
                               const std::optional<model::RunnerBuild>& build) const override;
};

}  // namespace mira::runner
