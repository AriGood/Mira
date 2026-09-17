#include "proc/ProcessSupervisor.h"

#include <dirent.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <set>

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

// How long to wait for a Steam-launched game to actually show up in /proc
// before giving up -- Steam itself has real startup latency (client
// wakeup, update checks, Proton's own prefix work) before the game process
// exists at all.
constexpr auto kSteamDetectTimeout = std::chrono::seconds(60);

void RunScript(const std::string& script, const std::string& game_id, const char* which) {
  if (script.empty()) return;
  Command command;
  command.argv = {"sh", "-c", script};
  if (auto result = runner::RunAndWait(command); !result || result->exit_code != 0) {
    log::Warn("launch.{}_script for {} failed: {}", which, game_id,
             !result ? result.error().message : std::format("exited {}", result->exit_code));
  }
}

// Every pid under this UID whose /proc/<pid>/environ carries
// SteamAppId=<appid> or SteamGameId=<appid> -- the whole subtree Steam's
// launch produces (reaper, pressure-vessel, proton, the game itself), not
// one specific process, since which of those is "the" game process varies
// by title and none of them is a child of this daemon either way.
std::set<pid_t> FindSteamProcesses(const std::string& appid) {
  std::set<pid_t> found;
  const std::string marker_app = "SteamAppId=" + appid;
  const std::string marker_game = "SteamGameId=" + appid;

  DIR* proc_dir = ::opendir("/proc");
  if (!proc_dir) return found;
  while (const dirent* entry = ::readdir(proc_dir)) {
    const std::string name = entry->d_name;
    if (name.empty() || !std::isdigit(static_cast<unsigned char>(name[0]))) continue;
    const pid_t pid = std::atoi(name.c_str());

    std::ifstream environ_file("/proc/" + name + "/environ", std::ios::binary);
    if (!environ_file) continue;  // gone, or not our own process (permission denied)
    const std::string environ((std::istreambuf_iterator<char>(environ_file)),
                              std::istreambuf_iterator<char>());
    // environ is NUL-separated, not newline-separated -- a plain substring
    // search still works since neither marker can span a NUL boundary by
    // construction (both are single "KEY=VALUE" entries).
    if (environ.find(marker_app) != std::string::npos || environ.find(marker_game) != std::string::npos) {
      found.insert(pid);
    }
  }
  ::closedir(proc_dir);
  return found;
}

// Signals a game's process group plus every process sharing its prefix.
void SignalGame(pid_t pid, const std::string& data_dir, int signal_number) {
  if (pid > 0) {
    ::kill(-pid, signal_number);
    ::kill(pid, signal_number);
  }
  for (pid_t found : FindPrefixProcesses(data_dir)) ::kill(found, signal_number);
}

bool AnyAlive(const std::set<pid_t>& pids) {
  for (pid_t pid : pids) {
    if (::kill(pid, 0) == 0) return true;
  }
  return false;
}

}  // namespace

// Every pid whose WINEPREFIX/STEAM_COMPAT_DATA_PATH points at data_dir.
//
// The process group alone isn't enough: on Proton/Wine, setsid()/setpgid()
// during startup leaves the group with just umu-run by the time a game is
// on screen. Measured against a real launch: signalling the group reached
// 1 of 16 processes.
std::set<pid_t> FindPrefixProcesses(const std::string& data_dir) {
  std::set<pid_t> found;
  if (data_dir.empty()) return found;

  // Entry-by-entry, not substring: "…/prefix/animal" must not match
  // "…/prefix/animal-well". umu rewrites WINEPREFIX to "<data_dir>/pfx/".
  const auto matches = [&data_dir](std::string_view value) {
    if (value == data_dir) return true;
    return value.size() > data_dir.size() && value.starts_with(data_dir) &&
           value[data_dir.size()] == '/';
  };

  DIR* proc_dir = ::opendir("/proc");
  if (!proc_dir) return found;
  while (const dirent* entry = ::readdir(proc_dir)) {
    const std::string name = entry->d_name;
    if (name.empty() || !std::isdigit(static_cast<unsigned char>(name[0]))) continue;

    std::ifstream environ_file("/proc/" + name + "/environ", std::ios::binary);
    if (!environ_file) continue;  // gone, or not our own process
    const std::string environ((std::istreambuf_iterator<char>(environ_file)),
                              std::istreambuf_iterator<char>());

    for (std::size_t start = 0; start < environ.size();) {
      const std::size_t end = environ.find('\0', start);
      const std::string_view item(environ.data() + start,
                                  (end == std::string::npos ? environ.size() : end) - start);
      start = (end == std::string::npos) ? environ.size() : end + 1;

      const std::size_t equals = item.find('=');
      if (equals == std::string_view::npos) continue;
      const std::string_view key = item.substr(0, equals);
      if (key != "WINEPREFIX" && key != "STEAM_COMPAT_DATA_PATH") continue;
      if (matches(item.substr(equals + 1))) {
        found.insert(std::atoi(name.c_str()));
        break;
      }
    }
  }
  ::closedir(proc_dir);
  return found;
}


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

Result<void> ProcessSupervisor::Launch(const model::Game& game, const Command& command,
                                       std::string post_script) {
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
    prefixes_[game.id] = game.data_dir;  // for Stop(), see FindPrefixProcesses
    // A previous watcher for this id has already finished by now (it erases
    // itself from running_ before exiting), but its thread object can still
    // be sitting here unjoined.
    if (auto stale = watchers_.find(game.id); stale != watchers_.end()) {
      if (stale->second.joinable()) stale->second.detach();
      watchers_.erase(stale);
    }
    watchers_[game.id] =
        std::thread(&ProcessSupervisor::Watch, this, game.id, *pid, started_at, std::move(post_script));
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
  std::string data_dir;
  {
    std::lock_guard lock(mutex_);
    const auto it = running_.find(game_id);
    if (it == running_.end()) {
      return Err("not_running", std::format("\"{}\" is not running", game_id));
    }
    pid = it->second;
    if (const auto prefix = prefixes_.find(game_id); prefix != prefixes_.end()) {
      data_dir = prefix->second;
    }
  }
  // 0 is TrackSteamLaunch's "reserved, not confirmed yet" sentinel — kill(0,
  // ...)/kill(-0, ...) both mean "signal every process in the caller's own
  // group" per POSIX, which would hit mirad itself. A real pid is always > 0.
  if (pid <= 0) {
    return Err("not_yet_confirmed",
              std::format("\"{}\" was launched but its process isn't confirmed yet — try again shortly",
                          game_id));
  }
  // Group (see runner::SpawnDetached's setpgid note) plus the prefix — see
  // FindPrefixProcesses for why the group alone usually isn't enough.
  const std::set<pid_t> in_prefix = FindPrefixProcesses(data_dir);
  const bool group_signalled = ::kill(-pid, SIGTERM) == 0 || ::kill(pid, SIGTERM) == 0;
  for (pid_t found : in_prefix) ::kill(found, SIGTERM);
  if (!group_signalled && in_prefix.empty()) {
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

void ProcessSupervisor::Watch(std::string game_id, pid_t pid, std::int64_t started_at,
                              std::string post_script) {
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
        const auto prefix = prefixes_.find(game_id);
        SignalGame(pid, prefix == prefixes_.end() ? std::string() : prefix->second, SIGKILL);
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
    prefixes_.erase(game_id);
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

  RunScript(post_script, game_id, "post");
}

Result<void> ProcessSupervisor::TrackSteamLaunch(const model::Game& game, const std::string& appid,
                                                 std::string post_script) {
  {
    std::lock_guard lock(mutex_);
    if (running_.contains(game.id)) {
      return Err("already_running", std::format("\"{}\" is already running", game.id));
    }
    // Reserved immediately, before detection even starts, for the same
    // reason Launch() reserves it before its process even exists yet:
    // without this, firing /launch twice in quick succession for the same
    // Steam game starts two independent detection watchers that could both
    // eventually find the same real process. 0 is never a real pid (see
    // Stop()'s guard below) so it's unambiguous as "not confirmed yet".
    running_[game.id] = 0;
    if (auto stale = watchers_.find(game.id); stale != watchers_.end()) {
      if (stale->second.joinable()) stale->second.detach();
      watchers_.erase(stale);
    }
    watchers_[game.id] = std::thread(&ProcessSupervisor::WatchSteam, this, game.id, appid,
                                     model::NowSeconds(), std::move(post_script));
  }
  return {};
}

void ProcessSupervisor::WatchSteam(std::string game_id, std::string appid, std::int64_t requested_at,
                                   std::string post_script) {
  std::set<pid_t> matched;
  std::int64_t started_at = 0;

  // Detection phase: wait for the launch to actually produce a process.
  while (!stopping_.load(std::memory_order_relaxed) && matched.empty()) {
    matched = FindSteamProcesses(appid);
    if (!matched.empty()) break;
    if (model::NowSeconds() - requested_at >= kSteamDetectTimeout.count()) {
      log::Warn("never detected a process for {} (appid {}) after {}s -- giving up", game_id, appid,
               kSteamDetectTimeout.count());
      std::lock_guard lock(mutex_);
      running_.erase(game_id);
      prefixes_.erase(game_id);
      return;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  if (stopping_.load(std::memory_order_relaxed)) return;  // daemon going away

  started_at = model::NowSeconds();
  {
    std::lock_guard lock(mutex_);
    running_[game_id] = *matched.begin();
  }
  auto stamped = games_.Update(game_id, [&](model::Game& stored) { stored.last_played_at = started_at; });
  if (!stamped) log::Error("failed to record launch time for {}: {}", game_id, stamped.error().message);
  log::Info("detected {} running (appid {}, {} process(es))", game_id, appid, matched.size());
  events_.Publish("game.state", {{"id", game_id}, {"state", "running"}});

  // Liveness phase: re-scan every tick rather than just poll the pids
  // already found, since the process tree can reshape early on (pressure-
  // vessel/proton forking further children) and a stale pid set would
  // report "exited" the moment the first-seen process happens to reap.
  std::int64_t credited = 0;
  while (!stopping_.load(std::memory_order_relaxed)) {
    const std::set<pid_t> current = FindSteamProcesses(appid);
    if (current.empty() && !AnyAlive(matched)) break;
    if (!current.empty()) matched = current;

    const std::int64_t elapsed = model::NowSeconds() - started_at;
    if (elapsed - credited >= kCheckpointSeconds) {
      const std::int64_t delta = elapsed - credited;
      auto checkpoint = games_.Update(game_id, [&](model::Game& game) { game.play_seconds += delta; });
      if (checkpoint) credited = elapsed;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  if (stopping_.load(std::memory_order_relaxed)) return;  // daemon going away

  const std::int64_t ended_at = model::NowSeconds();
  const std::int64_t played = ended_at > started_at ? ended_at - started_at : 0;

  {
    std::lock_guard lock(mutex_);
    running_.erase(game_id);
    prefixes_.erase(game_id);
    kill_deadlines_.erase(game_id);
    stop_requested_.erase(game_id);
  }

  // No real exit code/signal available for a process Mira didn't spawn, so
  // no crash detection here -- Steam's own client already shows that;
  // last_error is left alone rather than guessed at.
  auto updated =
      games_.Update(game_id, [&](model::Game& game) { game.play_seconds += played - credited; });
  if (!updated) log::Error("failed to record playtime for {}: {}", game_id, updated.error().message);

  log::Info("{} (Steam-launched) exited after {}s", game_id, played);
  events_.Publish("game.state",
                 {{"id", game_id}, {"state", "exited"}, {"played_seconds", played}});

  RunScript(post_script, game_id, "post");
}

}  // namespace mira::proc
