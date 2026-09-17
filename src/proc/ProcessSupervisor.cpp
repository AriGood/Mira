#include "proc/ProcessSupervisor.h"

#include <signal.h>
#include <string.h>
#include <sys/wait.h>

#include <chrono>
#include <format>

#include "core/Log.h"
#include "runner/Exec.h"

namespace mira::proc {
namespace {

constexpr auto kPollInterval = std::chrono::milliseconds(1000);

// Playtime is written back as it accrues, not only at exit: if mirad is
// killed or crashes while a game is running, its watcher dies with it, and
// anything not yet checkpointed is lost. This bounds that loss to a minute
// instead of the whole session.
constexpr std::int64_t kCheckpointSeconds = 60;

}  // namespace

ProcessSupervisor::ProcessSupervisor(store::GameStore& games, api::EventBus& events,
                                     std::int64_t stop_timeout_s)
    : games_(games), events_(events), stop_timeout_s_(stop_timeout_s) {}

ProcessSupervisor::~ProcessSupervisor() {
  stopping_.store(true, std::memory_order_relaxed);
  // Games are deliberately left running — quitting the daemon shouldn't kill
  // what the player is playing. The watchers just stop watching.
  std::map<std::string, std::thread> watchers;
  {
    std::lock_guard lock(mutex_);
    watchers.swap(watchers_);
  }
  for (auto& [id, thread] : watchers) {
    if (thread.joinable()) thread.join();
  }
}

Result<void> ProcessSupervisor::Launch(const model::Game& game, const Command& command) {
  {
    std::lock_guard lock(mutex_);
    if (running_.contains(game.id)) {
      return Err("already_running", std::format("\"{}\" is already running", game.id));
    }
  }

  auto pid = runner::SpawnDetached(command);
  if (!pid) return std::unexpected(pid.error());

  const std::int64_t started_at = model::NowSeconds();
  {
    std::lock_guard lock(mutex_);
    running_[game.id] = *pid;
    // A previous watcher for this id has already finished by now (it erases
    // itself from running_ before exiting), but its thread object can still
    // be sitting here unjoined.
    if (auto stale = watchers_.find(game.id); stale != watchers_.end()) {
      if (stale->second.joinable()) stale->second.detach();
      watchers_.erase(stale);
    }
    watchers_[game.id] = std::thread(&ProcessSupervisor::Watch, this, game.id, *pid, started_at);
  }

  // Written now, not at exit: this is "when you last started playing", and
  // recording it immediately means it survives the daemon dying mid-session.
  auto stamped = games_.Update(game.id, [&](model::Game& stored) {
    stored.last_played_at = started_at;
  });
  if (!stamped) {
    log::Error("failed to record launch time for {}: {}", game.id, stamped.error().message);
  }

  log::Info("launched {} (pid {})", game.id, *pid);
  events_.Publish("game.state", {{"id", game.id}, {"state", "running"}, {"pid", *pid}});
  return {};
}

Result<void> ProcessSupervisor::Stop(const std::string& game_id) {
  pid_t pid = 0;
  {
    std::lock_guard lock(mutex_);
    const auto it = running_.find(game_id);
    if (it == running_.end()) {
      return Err("not_running", std::format("\"{}\" is not running", game_id));
    }
    pid = it->second;
  }
  // Negative pid signals the whole process group — see the setpgid note in
  // runner::SpawnDetached.
  if (::kill(-pid, SIGTERM) != 0 && ::kill(pid, SIGTERM) != 0) {
    return Err("stop_failed", std::format("could not signal pid {}", pid));
  }
  {
    std::lock_guard lock(mutex_);
    kill_deadlines_[game_id] = model::NowSeconds() + stop_timeout_s_;
    stop_requested_.insert(game_id);
  }
  return {};
}

bool ProcessSupervisor::IsRunning(const std::string& game_id) const {
  std::lock_guard lock(mutex_);
  return running_.contains(game_id);
}

void ProcessSupervisor::Watch(std::string game_id, pid_t pid, std::int64_t started_at) {
  int status = 0;
  std::int64_t credited = 0;  // seconds already written to the store
  while (!stopping_.load(std::memory_order_relaxed)) {
    const pid_t result = ::waitpid(pid, &status, WNOHANG);
    if (result == pid) break;                    // exited
    if (result < 0) { status = -1; break; }      // vanished; treat as exited

    // A game that ignores SIGTERM gets SIGKILL once its deadline passes.
    {
      std::lock_guard lock(mutex_);
      const auto deadline = kill_deadlines_.find(game_id);
      if (deadline != kill_deadlines_.end() && model::NowSeconds() >= deadline->second) {
        log::Warn("{} ignored SIGTERM; sending SIGKILL", game_id);
        ::kill(-pid, SIGKILL);
        ::kill(pid, SIGKILL);
        kill_deadlines_.erase(deadline);
      }
    }

    const std::int64_t elapsed = model::NowSeconds() - started_at;
    if (elapsed - credited >= kCheckpointSeconds) {
      const std::int64_t delta = elapsed - credited;
      auto checkpoint = games_.Update(game_id, [&](model::Game& game) {
        game.play_seconds += delta;
      });
      if (checkpoint) credited = elapsed;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  if (stopping_.load(std::memory_order_relaxed)) return;  // daemon going away

  const std::int64_t ended_at = model::NowSeconds();
  const std::int64_t played = ended_at > started_at ? ended_at - started_at : 0;

  // A crash is a different outcome from a clean exit and has to be reported
  // as one: killed by a signal, or exited non-zero. Playtime is recorded
  // either way — the session still happened.
  const bool signalled = WIFSIGNALED(status);
  const int signal_number = signalled ? WTERMSIG(status) : 0;
  const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  // SIGTERM/SIGINT is how Stop() asks a game to quit, so that's a stop, not
  // a crash.
  bool requested_stop;
  {
    std::lock_guard lock(mutex_);
    requested_stop = stop_requested_.contains(game_id);
  }
  const bool crashed = !requested_stop && ((signalled && signal_number != SIGTERM) ||
                                           (!signalled && exit_code != 0));

  std::string error;
  if (crashed) {
    error = signalled ? std::format("Crashed on signal {} ({}) after {}s", signal_number,
                                    ::strsignal(signal_number), played)
                      : std::format("Exited with code {} after {}s", exit_code, played);
  }

  {
    std::lock_guard lock(mutex_);
    running_.erase(game_id);
    kill_deadlines_.erase(game_id);
    stop_requested_.erase(game_id);
  }

  auto updated = games_.Update(game_id, [&](model::Game& game) {
    game.play_seconds += played - credited;  // the rest was checkpointed already
    // Surfaced by the frontend as "last run didn't go well"; cleared on a
    // clean run so a one-off crash doesn't stick around forever.
    game.last_error = error;
  });
  if (!updated) {
    log::Error("failed to record playtime for {}: {}", game_id, updated.error().message);
  }

  if (crashed) {
    log::Warn("{} {}", game_id, error);
  } else {
    log::Info("{} exited cleanly after {}s", game_id, played);
  }
  events_.Publish("game.state", {{"id", game_id},
                                 {"state", crashed ? "crashed" : "exited"},
                                 {"exit_code", exit_code},
                                 {"signal", signal_number},
                                 {"played_seconds", played},
                                 {"error", error}});
}

}  // namespace mira::proc
