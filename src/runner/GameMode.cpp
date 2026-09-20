#include "runner/GameMode.h"

#include <string>
#include <utility>
#include <vector>

#include "core/Command.h"
#include "core/Log.h"
#include "runner/Exec.h"

namespace mira::gamemode {
namespace {

constexpr char kBusName[] = "com.feralinteractive.GameMode";
constexpr char kObjectPath[] = "/com/feralinteractive/GameMode";

// gdbus, not libgamemode: shelling out to a call matches this project's
// established convention (curl, sqlite3, flatpak, ...) over linking a new
// library for what's a handful of one-shot calls. gdbus itself ships with
// glib2, which is already an implicit dependency of any GTK/Qt desktop this
// runs on.
Result<runner::ExecResult> CallDBus(std::vector<std::string> args) {
  std::vector<std::string> argv = {"gdbus", "call", "--session"};
  argv.insert(argv.end(), std::make_move_iterator(args.begin()), std::make_move_iterator(args.end()));
  Command command;
  command.argv = std::move(argv);
  return runner::RunAndWait(command);
}

}  // namespace

bool IsInstalled() { return runner::FindOnPath("gamemoded") || runner::FindOnPath("gamemoderun"); }

bool IsDaemonRunning() {
  const auto result = CallDBus({"--dest", "org.freedesktop.DBus", "--object-path", "/org/freedesktop/DBus",
                               "--method", "org.freedesktop.DBus.NameHasOwner", kBusName});
  // The desktop-bus-standard NameHasOwner call, not anything GameMode-
  // specific -- works the same regardless of which bus daemon owns the
  // session bus, and correctly answers "not running" rather than erroring
  // when gdbus itself is missing (result is simply unset in that case).
  return result && result->exit_code == 0 && result->output.find("(true,)") != std::string::npos;
}

void RegisterGame(pid_t pid) {
  const auto result = CallDBus({"--dest", kBusName, "--object-path", kObjectPath, "--method",
                                std::string(kBusName) + ".RegisterGame", std::to_string(pid)});
  if (!result || result->exit_code != 0) {
    // Never a launch failure -- GameMode not being reachable is exactly as
    // harmless as the game requesting it directly and getting no answer.
    log::Warn("GameMode registration for pid {} failed (daemon not running?): {}", pid,
             !result ? result.error().message : result->output);
  }
}

void UnregisterGame(pid_t pid) {
  const auto result = CallDBus({"--dest", kBusName, "--object-path", kObjectPath, "--method",
                                std::string(kBusName) + ".UnregisterGame", std::to_string(pid)});
  if (!result || result->exit_code != 0) {
    log::Warn("GameMode unregistration for pid {} failed: {}", pid,
             !result ? result.error().message : result->output);
  }
}

}  // namespace mira::gamemode
