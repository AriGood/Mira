#include "runner/NativeRunner.h"

#include <cctype>
#include <filesystem>
#include <format>

#include "core/Strings.h"
#include "runner/Exec.h"

namespace mira::runner {
namespace {
namespace fs = std::filesystem;

// Only meaningful for a file that actually exists — a nonexistent exe_path
// (misconfigured, or a unit test using a fictitious path) is left to fail at
// exec time as before, not reported as "not executable".
bool MissingExecuteBit(const fs::path& path) {
  std::error_code ec;
  const auto status = fs::status(path, ec);
  if (ec || !fs::exists(status)) return false;
  return (status.permissions() & fs::perms::owner_exec) == fs::perms::none;
}

std::string ToLower(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

// A FUSE-less system needs "--appimage-extract-and-run" as the AppImage's
// own first argument (see AppImage's own docs) — /dev/fuse absent, or
// neither fusermount nor fusermount3 on PATH, both mean no FUSE.
bool HasFuse() {
  return std::filesystem::exists("/dev/fuse") || FindOnPath("fusermount") || FindOnPath("fusermount3");
}

}  // namespace

Result<Command> NativeRunner::BuildCommand(const model::Game& game,
                                            const std::optional<model::RunnerBuild>&) const {
  if (game.exe_path.empty()) return Err("no_executable", "no exe_path set for this game");

  const std::filesystem::path install_path = game.install_path;
  const std::filesystem::path exe = install_path / game.exe_path;
  const std::string ext = ToLower(exe.extension().string());

  Command command;
  if (ext == ".sh" || ext == ".bash") {
    // A .sh routinely loses its executable bit in an archive, so it's still
    // accepted as a candidate without one (see library::Detector) — run it
    // through sh explicitly instead of relying on exec's own +x check.
    command.argv = {"sh", exe.string()};
  } else if (ext == ".appimage" && !HasFuse()) {
    command.argv = {exe.string(), "--appimage-extract-and-run"};
  } else {
    if (MissingExecuteBit(exe)) {
      return Err("not_executable",
                std::format("\"{}\" is not marked executable — chmod +x it, or point exe_path at "
                            "the actual launcher", exe.string()));
    }
    command.argv.push_back(exe.string());
  }

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
