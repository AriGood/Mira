#pragma once

#include <sys/types.h>

#include <atomic>
#include <map>
#include <set>
#include <mutex>
#include <string>
#include <thread>

#include "api/EventBus.h"
#include "core/Command.h"
#include "core/Result.h"
#include "store/GameStore.h"

namespace mira::proc {

// Every pid whose WINEPREFIX/STEAM_COMPAT_DATA_PATH points at data_dir.
// Exposed for testing the prefix-collision rule (see the .cpp).
std::set<pid_t> FindPrefixProcesses(const std::string& data_dir);

// Tracks the games currently running. One watcher thread per running game —
// fine at launcher scale, and unlike a blanket waitpid(-1) reaper it can't
// steal the exit status of runner::RunAndWait's own provisioning children.
//
// Polls with WNOHANG (not a blocking waitpid()) so shutdown doesn't wait on
// the player quitting; only ticks while a game is actually running.
class ProcessSupervisor {
public:
  ProcessSupervisor(store::GameStore& games, api::EventBus& events,
                    std::int64_t stop_timeout_s = 10);
  ~ProcessSupervisor();
  ProcessSupervisor(const ProcessSupervisor&) = delete;
  ProcessSupervisor& operator=(const ProcessSupervisor&) = delete;

  // Starts the game and returns as soon as it's running. Publishes
  // game.state running now, and exited later, with playtime recorded.
  // post_script (see launch.post_script) runs once the game exits, before
  // playtime is finalized in the store.
  Result<void> Launch(const model::Game& game, const Command& command, std::string post_script = "");

  // For a game Steam's own client launched (steam.launch_mode "steam"), which
  // Mira can't waitpid() on. Polls /proc for SteamAppId=<appid> or
  // SteamGameId=<appid> in a process's environment, since the actual game
  // sits under a steam -> reaper -> pressure-vessel -> proton chain with no
  // fixed pid. Reports running/exited like Launch(), minus a real exit
  // code/signal. Gives up quietly if nothing matches within a startup
  // window — Steam may still be launching, or the player cancelled.
  Result<void> TrackSteamLaunch(const model::Game& game, const std::string& appid,
                                std::string post_script = "");

  // SIGTERM to the running game's whole process group, if any. Returns as
  // soon as the signal is sent; if the game ignores it, the watcher escalates
  // to SIGKILL after stop_timeout_s rather than blocking the caller.
  Result<void> Stop(const std::string& game_id);

  bool IsRunning(const std::string& game_id) const;

private:
  void Watch(std::string game_id, pid_t pid, std::int64_t started_at, std::string post_script);
  void WatchSteam(std::string game_id, std::string appid, std::int64_t requested_at,
                  std::string post_script);

  store::GameStore& games_;
  api::EventBus& events_;
  std::int64_t stop_timeout_s_;

  mutable std::mutex mutex_;
  std::map<std::string, pid_t> running_;
  std::map<std::string, std::string> prefixes_;  // game id -> data_dir, for Stop()
  std::map<std::string, std::int64_t> kill_deadlines_;  // game id -> when to SIGKILL
  std::set<std::string> stop_requested_;  // Stop() was called; the exit isn't a crash
  std::map<std::string, std::thread> watchers_;
  std::atomic<bool> stopping_{false};
};

}  // namespace mira::proc
