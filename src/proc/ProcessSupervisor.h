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
  Result<void> Launch(const model::Game& game, const Command& command);

  // SIGTERM to the running game's whole process group, if any. Returns as
  // soon as the signal is sent; if the game ignores it, the watcher escalates
  // to SIGKILL after stop_timeout_s rather than blocking the caller.
  Result<void> Stop(const std::string& game_id);

  bool IsRunning(const std::string& game_id) const;

private:
  void Watch(std::string game_id, pid_t pid, std::int64_t started_at);

  store::GameStore& games_;
  api::EventBus& events_;
  std::int64_t stop_timeout_s_;

  mutable std::mutex mutex_;
  std::map<std::string, pid_t> running_;
  std::map<std::string, std::int64_t> kill_deadlines_;  // game id -> when to SIGKILL
  std::set<std::string> stop_requested_;  // Stop() was called; the exit isn't a crash
  std::map<std::string, std::thread> watchers_;
  std::atomic<bool> stopping_{false};
};

}  // namespace mira::proc
