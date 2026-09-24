#include "runner/ProtonRunner.h"

#include <cctype>

#include <filesystem>
#include <format>
#include <fstream>

#include "config/Config.h"
#include "core/Strings.h"
#include "runner/Exec.h"

namespace mira::runner {
namespace {
namespace fs = std::filesystem;

// Without a GAMEID every game runs as umu-default, so Proton gives all of
// their windows the same class (steam_app_default) and desktops group them
// as one app. umu only accepts letters, digits and underscores after "umu-".
void ApplyGameId(Command& command, const model::Game& game) {
  if (auto it = game.runner_config.find("gameid"); it != game.runner_config.end() && it->is_string()) {
    command.env["GAMEID"] = it->get<std::string>();
  } else {
    std::string id = "umu-mira_";
    for (const char ch : game.id) {
      if (std::isalnum(static_cast<unsigned char>(ch))) id.push_back(ch);
    }
    command.env["GAMEID"] = id;
  }
  if (auto it = game.runner_config.find("store"); it != game.runner_config.end() && it->is_string()) {
    command.env["STORE"] = it->get<std::string>();
  }
}

}  // namespace

std::vector<model::RunnerBuild> ProtonRunner::Discover(const config::Config& config) const {
  std::vector<model::RunnerBuild> builds;
  // A Proton build is useless without the launcher that runs it.
  if (!FindOnPath("umu-run")) return builds;

  std::error_code ec;

  for (const fs::path& search_dir : config.GetPathArray("runner_search_paths")) {
    if (!fs::is_directory(search_dir, ec)) continue;
    for (const auto& entry : fs::directory_iterator(search_dir, fs::directory_options::skip_permission_denied, ec)) {
      if (!entry.is_directory(ec)) continue;
      const fs::path& dir = entry.path();
      if (!fs::exists(dir / "proton", ec) || !fs::exists(dir / "toolmanifest.vdf", ec)) continue;

      model::RunnerBuild build;
      build.kind = "proton";
      build.path = dir.string();

      // The version file is "<unix timestamp> <name>", e.g.
      // "1789520217 GE-Proton11-7" — timestamp for sorting "latest", name
      // for the human-facing "kind:name" reference.
      std::ifstream version_file(dir / "version");
      std::string line;
      if (version_file && std::getline(version_file, line)) {
        if (const auto space = line.find(' '); space != std::string::npos) {
          build.version = line.substr(0, space);
          build.name = strings::Trim(line.substr(space + 1));
        }
      }
      if (build.name.empty()) build.name = dir.filename().string();  // fallback

      builds.push_back(std::move(build));
    }
  }
  return builds;
}

Result<void> ProtonRunner::Provision(const model::Game& game,
                                  const std::optional<model::RunnerBuild>& build) const {
  if (!build) return Err("no_runner_build", "no Proton build resolved for this game");
  if (game.data_dir.empty()) return Err("no_data_dir", "game has no data_dir set");

  std::error_code ec;
  fs::create_directories(game.data_dir, ec);
  if (ec) return Err("prefix_create_failed", ec.message());

  // "" as the exe is umu's documented way to initialise a prefix with no
  // game to run (see `man umu`, Example 4).
  Command command;
  command.argv = {"umu-run", ""};
  command.env["WINEPREFIX"] = game.data_dir;
  command.env["PROTONPATH"] = build->path;
  ApplyGameId(command, game);

  auto result = RunAndWait(command);
  if (!result) return std::unexpected(result.error());

  // umu-run's exit code conflates "prefix init failed" with "no game to
  // launch" — it's 1 even on a fully successful run here, since we
  // deliberately gave it nothing to launch. The only reliable success
  // signal is whether the prefix actually appeared on disk.
  if (!fs::exists(fs::path(game.data_dir) / "drive_c", ec)) {
    return Err("provision_failed",
              std::format("umu-run produced no prefix (exit {}): {}", result->exit_code, result->output));
  }
  return {};
}

Result<Command> ProtonRunner::BuildCommand(const model::Game& game,
                                        const std::optional<model::RunnerBuild>& build) const {
  if (!build) return Err("no_runner_build", "no Proton build resolved for this game");
  if (game.exe_path.empty()) return Err("no_executable", "no exe_path set for this game");
  if (game.data_dir.empty()) return Err("no_data_dir", "game has no data_dir set");

  const fs::path install_path = game.install_path;
  const fs::path exe = install_path / game.exe_path;

  Command command;
  command.argv = {"umu-run", exe.string()};
  for (const std::string& arg : strings::Split(game.args, ' ')) {
    if (!arg.empty()) command.argv.push_back(arg);
  }

  command.env["WINEPREFIX"] = game.data_dir;
  command.env["PROTONPATH"] = build->path;
  ApplyGameId(command, game);
  for (const auto& [key, value] : game.env) command.env[key] = value;  // game-specific wins

  command.cwd = game.working_dir.empty() ? exe.parent_path() : install_path / game.working_dir;
  return command;
}

nlohmann::json ProtonRunner::SettingsSchema() const {
  return nlohmann::json::array({
      {{"key", "gameid"},
       {"type", "string"},
       {"doc", "Steam AppID umu should report via the GAMEID env var — affects which "
               "protonfixes/compat-DB entry Proton applies. Optional; umu falls back to a "
               "generic default (\"umu-default\") without it."}},
      {{"key", "store"},
       {"type", "string"},
       {"doc", "Store umu should report via the STORE env var (e.g. \"ubisoft\", \"battlenet\", \"ea\"), "
               "so store-specific protonfixes apply. Set automatically for store launcher games."}},
  });
}

}  // namespace mira::runner
