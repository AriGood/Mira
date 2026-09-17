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

// Every pid under this UID running inside one game's Wine prefix, found by
// the WINEPREFIX/STEAM_COMPAT_DATA_PATH every process in the tree inherits.
//
// Exposed (rather than kept private to Stop) because the prefix-collision
// rule is the subtle part -- a game whose data_dir is a string prefix of
// another's must not match its neighbour -- and that deserves a test.
std::set<pid_t> FindPrefixProcesses(const std::string& data_dir);


// Tracks the games currently running. One watcher thread per running game,
// which is fine at a launcher's scale (usually one) and — unlike a single
// blanket waitpid(-1) reaper — cannot steal the exit status of the
// provisioning children runner::RunAndWait waits on itself.
//
// The watcher polls with WNOHANG rather than blocking in waitpid() so daemon
// shutdown doesn't have to wait for the player to quit their game. That tick
// only runs while a game is actually running, never at rest — the same
// exemption as the SSE liveness check and the scan debounce timer (see
// docs/architecture.md, Idle cost).
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

  // For a game Mira didn't spawn itself -- Steam's own client did, via
  // steam://rungameid/<appid> (steam.launch_mode "steam"). Mira can't
  // waitpid() a process it isn't the parent of, so instead this polls /proc
  // for a process carrying SteamAppId=<appid> or SteamGameId=<appid> in its
  // environment -- the same variable the Steamworks API itself reads
  // (confirmed against a real Proton install's protonfixes/fix.py), not one
  // specific pid, since the actual game process sits under a
  // steam -> reaper -> pressure-vessel -> proton chain that varies by title.
  // Reports running/exited the same way Launch() does, minus a real exit
  // code/signal (not obtainable for a non-child process -- Steam's own
  // client already has that; this exists only so Mira's own state is
  // accurate). Gives up quietly, no error, if nothing matching ever shows up
  // within a startup window -- Steam itself may still be launching, or the
  // player cancelled it, and there is no caller left waiting on this by the
  // time it would find out either way.
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
  // game id -> data_dir, so Stop() can find a Proton/Wine tree that has
  // already left the process group mirad launched it in.
  std::map<std::string, std::string> prefixes_;
  std::map<std::string, std::int64_t> kill_deadlines_;  // game id -> when to SIGKILL
  std::set<std::string> stop_requested_;  // Stop() was called; the exit isn't a crash
  std::map<std::string, std::thread> watchers_;
  std::atomic<bool> stopping_{false};
};

}  // namespace mira::proc
