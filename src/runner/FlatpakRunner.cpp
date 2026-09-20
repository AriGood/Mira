#include "runner/FlatpakRunner.h"

#include <filesystem>
#include <format>

#include "config/Config.h"
#include "core/Strings.h"
#include "runner/Exec.h"

namespace mira::runner {

Result<std::vector<FlatpakApp>> ListInstalledFlatpakApps() {
  const auto flatpak_bin = FindOnPath("flatpak");
  if (!flatpak_bin) return std::vector<FlatpakApp>{};  // not installed -- not an error, see FlatpakRunner.h

  Command command;
  command.argv = {*flatpak_bin, "list", "--app", "--columns=application,name,version"};
  const Result<ExecResult> result = RunAndWait(command);
  if (!result || result->exit_code != 0) {
    return Err("flatpak_list_failed",
              !result ? result.error().message : std::format("flatpak list exited {}: {}", result->exit_code,
                                                              result->output));
  }

  std::vector<FlatpakApp> apps;
  for (const std::string& line : strings::Split(result->output, '\n')) {
    if (line.empty()) continue;
    const std::vector<std::string> columns = strings::Split(line, '\t');
    if (columns.empty() || columns[0].empty()) continue;

    apps.push_back(FlatpakApp{
        .app_id = columns[0],
        .name = columns.size() > 1 ? columns[1] : columns[0],
        .version = columns.size() > 2 ? columns[2] : "",
    });
  }
  return apps;
}

std::vector<model::RunnerBuild> FlatpakRunner::Discover(const config::Config&) const {
  const auto apps = ListInstalledFlatpakApps();
  if (!apps) return {};

  std::vector<model::RunnerBuild> builds;
  builds.reserve(apps->size());
  for (const FlatpakApp& app : *apps) {
    model::RunnerBuild build;
    build.kind = "flatpak";
    build.name = app.app_id;  // also what forms runner_ref, see FlatpakRunner.h
    build.version = app.version;
    builds.push_back(std::move(build));
  }
  return builds;
}

Result<void> FlatpakRunner::Provision(const model::Game&, const std::optional<model::RunnerBuild>& build) const {
  if (!build) return Err("no_runner_build", "no Flatpak app resolved for this game");
  const auto flatpak_bin = FindOnPath("flatpak");
  if (!flatpak_bin) return Err("flatpak_missing", "flatpak isn't installed");

  // Nothing to set up -- Flatpak already installed and sandboxed the app.
  // Just confirm it's still actually there, since Mira's own record of it
  // (game.runner_ref) can go stale if it's uninstalled behind Mira's back.
  Command command;
  command.argv = {*flatpak_bin, "info", build->name};
  const Result<ExecResult> result = RunAndWait(command);
  if (!result || result->exit_code != 0) {
    return Err("flatpak_app_missing",
              std::format("flatpak app \"{}\" is no longer installed", build->name));
  }
  return {};
}

Result<Command> FlatpakRunner::BuildCommand(const model::Game& game,
                                            const std::optional<model::RunnerBuild>& build) const {
  if (!build) return Err("no_runner_build", "no Flatpak app resolved for this game");

  Command command;
  command.argv = {"flatpak", "run"};
  // A flatpak sandbox doesn't inherit the daemon's environment the way a
  // plain exec does (see runner::Exec.cpp's MergedEnv) -- env has to be
  // handed to flatpak explicitly, as its own --env= flags.
  for (const auto& [key, value] : game.env) command.argv.push_back(std::format("--env={}={}", key, value));
  command.argv.push_back(build->name);

  // strings::Split("", ' ') returns one empty element, not an empty vector —
  // filter blanks out first, or an unset game.args still adds a pointless
  // trailing "--" to every single flatpak launch (confirmed live).
  std::vector<std::string> args;
  for (std::string& arg : strings::Split(game.args, ' ')) {
    if (!arg.empty()) args.push_back(std::move(arg));
  }
  if (!args.empty()) {
    command.argv.push_back("--");  // keeps a leading "-" in a game's own args from confusing flatpak's own parser
    command.argv.insert(command.argv.end(), args.begin(), args.end());
  }

  // Not merged into command.env: everything the app sees is the --env=
  // flags above, which flatpak run passes into the sandbox itself. Left
  // empty here so runner::Exec.cpp's MergedEnv only overlays mirad's own
  // environment onto the "flatpak" launcher process, not into the sandbox.
  command.cwd = game.working_dir.empty() ? std::filesystem::path() : std::filesystem::path(game.install_path) / game.working_dir;
  return command;
}

}  // namespace mira::runner
