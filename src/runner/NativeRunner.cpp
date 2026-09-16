#include "runner/NativeRunner.h"

#include <filesystem>

#include "core/Strings.h"

namespace mira::runner {

Result<Command> NativeRunner::BuildCommand(const model::Game& game,
                                            const std::optional<model::RunnerBuild>&) const {
  if (game.exe_path.empty()) return Err("no_executable", "no exe_path set for this game");

  const std::filesystem::path install_path = game.install_path;
  const std::filesystem::path exe = install_path / game.exe_path;

  Command command;
  command.argv.push_back(exe.string());
  // game.args is a plain space-separated string, not shell-quoted — no
  // support for an argument containing a literal space yet.
  for (const std::string& arg : strings::Split(game.args, ' ')) {
    if (!arg.empty()) command.argv.push_back(arg);
  }
  command.env = game.env;
  command.cwd = game.working_dir.empty() ? exe.parent_path() : install_path / game.working_dir;
  return command;
}

}  // namespace mira::runner
