#include "runner/Winetricks.h"

#include <filesystem>
#include <format>

#include "core/Command.h"
#include "runner/Exec.h"
#include "steam/SteamDetector.h"

namespace mira::runner {
namespace {
namespace fs = std::filesystem;

Result<fs::path> ResolveWineBinary(const RunnerRegistry& runners, const model::Game& game) {
  const std::string ref = game.runner_ref.empty() ? "native:native" : game.runner_ref;
  const auto resolved = runners.Resolve(ref);
  if (!resolved) return std::unexpected(resolved.error());

  const std::string kind = resolved->runner->kind();
  if (kind == "wine") {
    if (!resolved->build) return Err("no_runner_build", "no Wine build resolved for this game");
    return fs::path(resolved->build->path);
  }
  if (kind == "proton") {
    if (!resolved->build) return Err("no_runner_build", "no Proton build resolved for this game");
    return fs::path(resolved->build->path) / "files" / "bin" / "wine";
  }
  if (kind == "steam") {
    if (game.data_dir.empty()) {
      return Err("not_windows", "this Steam game is native — winetricks needs a Wine/Proton prefix");
    }
    const auto info = steam::ResolveProtonCompatInfo(game.data_dir);
    if (!info) {
      return Err("steam_proton_unresolved",
                "couldn't determine which Proton build this prefix uses — run this game once "
                "through Steam first");
    }
    return info->proton_path.parent_path() / "files" / "bin" / "wine";
  }
  return Err("not_wine_based",
            std::format("winetricks only applies to a Wine or Proton runner, not \"{}\"", kind));
}

}  // namespace

Result<void> RunTricksVerb(const RunnerRegistry& runners, const model::Game& game, const std::string& verb) {
  if (game.data_dir.empty()) return Err("no_data_dir", "game has no prefix to run winetricks against");
  std::error_code ec;
  if (!fs::exists(fs::path(game.data_dir) / "drive_c", ec)) {
    return Err("not_provisioned", "this game's prefix hasn't been provisioned yet");
  }

  const Result<fs::path> wine_binary = ResolveWineBinary(runners, game);
  if (!wine_binary) return std::unexpected(wine_binary.error());

  const auto winetricks = FindOnPath("winetricks");
  if (!winetricks) {
    return Err("winetricks_missing", "winetricks isn't installed — install it from your distro's package manager");
  }

  Command command;
  command.argv = {*winetricks, "--unattended", verb};
  command.env["WINE"] = wine_binary->string();
  command.env["WINEPREFIX"] = game.data_dir;
  const fs::path wineserver = wine_binary->parent_path() / "wineserver";
  if (fs::exists(wineserver, ec)) command.env["WINESERVER"] = wineserver.string();

  const Result<ExecResult> result = RunAndWait(command);
  if (!result) return std::unexpected(result.error());
  if (result->exit_code != 0) {
    return Err("tricks_failed", std::format("winetricks {} exited {}: {}", verb, result->exit_code, result->output));
  }
  return {};
}

}  // namespace mira::runner
