// mira-run: the process that actually sits between mirad and a running
// game. Owns the whole session end to end — pre_script, the game itself,
// post_script, and the session record — so none of that depends on mirad
// staying alive for the session's whole duration. See docs/architecture.md
// and proc/Session.h.
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/Result.h"
#include "model/Types.h"
#include "proc/Session.h"
#include "runner/GameMode.h"

using namespace mira;

namespace {

struct Args {
  std::string game_id;
  std::filesystem::path session_dir;
  std::filesystem::path log_file;  // empty disables per-game logging entirely
  int log_max_mb = 64;
  int status_fd = -1;
  std::string pre;
  std::string post;
  int pre_timeout_s = 30;
  int post_timeout_s = 30;
  bool gamemode = false;
  std::vector<std::string> game_argv;
};

std::optional<Args> ParseArgs(int argc, char** argv) {
  Args args;
  int i = 1;
  for (; i < argc; ++i) {
    const std::string_view a = argv[i];
    const auto next = [&]() -> std::optional<std::string> {
      if (i + 1 >= argc) return std::nullopt;
      return std::string(argv[++i]);
    };
    if (a == "--game-id") {
      auto v = next();
      if (!v) return std::nullopt;
      args.game_id = *v;
    } else if (a == "--session-dir") {
      auto v = next();
      if (!v) return std::nullopt;
      args.session_dir = *v;
    } else if (a == "--log-file") {
      auto v = next();
      if (!v) return std::nullopt;
      args.log_file = *v;
    } else if (a == "--log-max-mb") {
      auto v = next();
      if (!v) return std::nullopt;
      args.log_max_mb = std::atoi(v->c_str());
    } else if (a == "--gamemode") {
      args.gamemode = true;
    } else if (a == "--status-fd") {
      auto v = next();
      if (!v) return std::nullopt;
      args.status_fd = std::atoi(v->c_str());
    } else if (a == "--pre") {
      auto v = next();
      if (!v) return std::nullopt;
      args.pre = *v;
    } else if (a == "--post") {
      auto v = next();
      if (!v) return std::nullopt;
      args.post = *v;
    } else if (a == "--pre-timeout") {
      auto v = next();
      if (!v) return std::nullopt;
      args.pre_timeout_s = std::atoi(v->c_str());
    } else if (a == "--post-timeout") {
      auto v = next();
      if (!v) return std::nullopt;
      args.post_timeout_s = std::atoi(v->c_str());
    } else if (a == "--") {
      ++i;
      break;
    } else {
      return std::nullopt;
    }
  }
  for (; i < argc; ++i) args.game_argv.emplace_back(argv[i]);
  if (args.game_id.empty() || args.session_dir.empty() || args.game_argv.empty()) return std::nullopt;
  return args;
}

void WriteStatus(int fd, std::string_view line) {
  if (fd < 0) return;
  std::size_t written = 0;
  while (written < line.size()) {
    const ssize_t n = ::write(fd, line.data() + written, line.size() - written);
    if (n <= 0) return;
    written += static_cast<std::size_t>(n);
  }
}

struct ScriptResult {
  int exit_code = -1;
  bool timed_out = false;
  std::string output;
};

// Runs `sh -c script` with a wall-clock timeout, SIGKILLing the whole group
// if it hangs — pre/post must never be able to wedge mira-run forever, since
// that would wedge the launch (pre) or the game's own exit bookkeeping
// (post). Separate from runner::RunAndWait, which has no timeout at all.
ScriptResult RunScriptWithTimeout(const std::string& script, int timeout_s) {
  ScriptResult result;
  int pipe_fds[2];
  if (::pipe2(pipe_fds, O_CLOEXEC) != 0) return result;

  const pid_t pid = fork();
  if (pid < 0) {
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
    return result;
  }
  if (pid == 0) {
    ::setpgid(0, 0);
    ::dup2(pipe_fds[1], STDOUT_FILENO);
    ::dup2(pipe_fds[1], STDERR_FILENO);
    ::execl("/bin/sh", "sh", "-c", script.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  ::close(pipe_fds[1]);

  // Non-blocking so the read loop below can interleave with the WNOHANG
  // waitpid poll instead of a chatty-but-hung script wedging the read past
  // the timeout.
  const int flags = ::fcntl(pipe_fds[0], F_GETFL, 0);
  ::fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
  int status = 0;
  bool exited = false;
  char buffer[4096];
  while (std::chrono::steady_clock::now() < deadline) {
    ssize_t n;
    while ((n = ::read(pipe_fds[0], buffer, sizeof(buffer))) > 0) result.output.append(buffer, static_cast<std::size_t>(n));
    if (::waitpid(pid, &status, WNOHANG) == pid) {
      exited = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (!exited) {
    result.timed_out = true;
    ::kill(-pid, SIGKILL);
    ::waitpid(pid, &status, 0);
  }
  ssize_t n;
  while ((n = ::read(pipe_fds[0], buffer, sizeof(buffer))) > 0) result.output.append(buffer, static_cast<std::size_t>(n));
  ::close(pipe_fds[0]);

  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return result;
}

// Rotates at session start, not live: renames the existing log to .1,
// dropping whatever .1 was already there, unless the file that would become
// .1 is already over the cap -- then it's just dropped instead of kept
// forever. Caps total disk use at roughly 2x log_max_mb per game without
// needing a live-truncation pass while the game is writing to it.
void RotateLog(const std::filesystem::path& log_path, int max_mb) {
  std::error_code ec;
  if (!std::filesystem::exists(log_path, ec)) return;
  const auto size = std::filesystem::file_size(log_path, ec);
  const std::string previous = log_path.string() + ".1";
  std::filesystem::remove(previous, ec);
  if (!ec && size <= static_cast<std::uintmax_t>(max_mb) * 1024 * 1024) {
    std::filesystem::rename(log_path, previous, ec);
  } else {
    std::filesystem::remove(log_path, ec);
  }
}

void WriteLogLine(int fd, std::string_view line) {
  if (fd < 0) return;
  std::size_t written = 0;
  while (written < line.size()) {
    const ssize_t n = ::write(fd, line.data() + written, line.size() - written);
    if (n <= 0) return;
    written += static_cast<std::size_t>(n);
  }
}

}  // namespace

int main(int argc, char** argv) {
  const auto parsed = ParseArgs(argc, argv);
  if (!parsed) {
    std::cerr << "mira-run: usage: mira-run --game-id ID --session-dir DIR --status-fd N "
                 "[--pre CMD] [--post CMD] [--pre-timeout S] [--post-timeout S] -- <argv...>\n";
    return 2;
  }
  const Args& args = *parsed;

  // Computed before --pre runs (not after), so the session path can ride
  // along on the same "ok" status message mirad is already blocking on —
  // mirad otherwise has no way to know which of possibly several files in
  // --session-dir belongs to this particular launch.
  const auto monotonic_start = std::chrono::steady_clock::now();
  const std::int64_t started_at = model::NowSeconds();
  const auto session_path = proc::SessionFilePath(args.session_dir, args.game_id, started_at);

  // Opened before --pre runs so its output lands in the same file as
  // everything else about this session -- one file that explains the whole
  // thing, not a fragment of it. Rule 1 (bookkeeping never blocks play): a
  // log that can't be opened just means no logging this session, not a
  // failed launch.
  int log_fd = -1;
  if (!args.log_file.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(args.log_file.parent_path(), ec);
    RotateLog(args.log_file, args.log_max_mb);
    log_fd = ::open(args.log_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (log_fd < 0) {
      std::cerr << "mira-run: could not open log file " << args.log_file << ": " << std::strerror(errno) << "\n";
    } else {
      WriteLogLine(log_fd, std::format("[mira-run] session start, game_id={}\n", args.game_id));
    }
  }

  if (!args.pre.empty()) {
    const ScriptResult pre = RunScriptWithTimeout(args.pre, args.pre_timeout_s);
    if (log_fd >= 0) {
      WriteLogLine(log_fd, std::format("[mira-run] pre_script (exit {}):\n{}\n", pre.exit_code, pre.output));
    }
    if (pre.timed_out) {
      WriteStatus(args.status_fd, "pre_timeout\n");
      return 1;
    }
    if (pre.exit_code != 0) {
      WriteStatus(args.status_fd, std::format("pre_failed\n{}", pre.output));
      return 1;
    }
  }
  WriteStatus(args.status_fd, std::format("ok\n{}\n", session_path.string()));
  if (args.status_fd >= 0) ::close(args.status_fd);

  proc::SessionRecord record;
  record.game_id = args.game_id;
  record.wrapper_pid = ::getpid();
  record.started_at = started_at;

  if (log_fd >= 0) {
    std::string argv_line = "[mira-run] launching:";
    for (const std::string& a : args.game_argv) argv_line += " " + a;
    argv_line += "\n";
    WriteLogLine(log_fd, argv_line);
  }

  std::vector<char*> game_argv;
  game_argv.reserve(args.game_argv.size() + 1);
  for (const std::string& a : args.game_argv) game_argv.push_back(const_cast<char*>(a.c_str()));
  game_argv.push_back(nullptr);

  const pid_t game_pid = fork();
  if (game_pid < 0) {
    // Bookkeeping never blocks play, but there's no play possible here at
    // all — fork() itself failed, so this is a genuine launch failure.
    record.finished = true;
    record.launch_error = std::strerror(errno);
    record.exit_code = 127;
    record.ended_at = model::NowSeconds();
    [[maybe_unused]] auto _ = proc::WriteSessionRecord(session_path, record);
    std::cerr << "mira-run: fork failed: " << std::strerror(errno) << "\n";
    return 1;
  }
  if (game_pid == 0) {
    // Deliberately no setpgid(0, 0) here: mirad's own spawn of mira-run
    // already made mira-run the leader of a fresh process group (see
    // runner::SpawnDetachedWithStatus), and the game is meant to inherit
    // that group, not start a new one -- Stop()'s kill(-pid) targets that
    // one shared group to reach the whole umu -> proton -> wine -> game
    // tree, mira-run included.
    if (log_fd >= 0) {
      ::dup2(log_fd, STDOUT_FILENO);
      ::dup2(log_fd, STDERR_FILENO);
    }
    ::execvp(game_argv[0], game_argv.data());
    _exit(127);  // only reached if exec itself failed
  }

  record.game_pid = game_pid;
  // Best-effort (Rule 1: bookkeeping never blocks play) — the game is
  // already running regardless of whether this succeeds.
  [[maybe_unused]] auto write_start = proc::WriteSessionRecord(session_path, record);

  if (args.gamemode) gamemode::RegisterGame(game_pid);

  // No setpgid() above means the game shares mira-run's own process group
  // (mira-run is that group's leader — see runner::SpawnDetachedWithStatus),
  // so mirad's Stop() reaches the whole tree (mira-run included) with one
  // external kill(-pid). mira-run must survive that to still run --post and
  // write the final record, so SIGTERM/SIGINT are ignored here rather than
  // handled — no forwarding needed, the game gets the same signal directly
  // from mirad's own kill(-pid) call, at the same moment mira-run does.
  struct sigaction sa {};
  sa.sa_handler = SIG_IGN;
  ::sigemptyset(&sa.sa_mask);
  ::sigaction(SIGTERM, &sa, nullptr);
  ::sigaction(SIGINT, &sa, nullptr);

  int status = 0;
  pid_t waited;
  do {
    waited = ::waitpid(game_pid, &status, 0);
  } while (waited < 0 && errno == EINTR);

  if (args.gamemode) gamemode::UnregisterGame(game_pid);

  record.finished = true;
  record.ended_at = model::NowSeconds();
  // A monotonic clock, not the wall-clock started_at/ended_at pair above:
  // immune to a system clock jump mid-session, which would otherwise
  // silently produce a nonsensical or negative duration.
  record.duration_seconds =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - monotonic_start).count();
  if (WIFEXITED(status)) {
    record.exit_code = WEXITSTATUS(status);
    if (record.exit_code == 127) {
      record.launch_error = std::format("could not start \"{}\"", args.game_argv.front());
    }
  } else if (WIFSIGNALED(status)) {
    record.signal = WTERMSIG(status);
  }

  if (log_fd >= 0) {
    WriteLogLine(log_fd, std::format("[mira-run] game exited: exit_code={} signal={} duration={}s\n",
                                     record.exit_code, record.signal, record.duration_seconds));
  }

  if (!args.post.empty()) {
    const ScriptResult post = RunScriptWithTimeout(args.post, args.post_timeout_s);
    record.post_exit_code = post.exit_code;
    record.post_timed_out = post.timed_out;
    if (log_fd >= 0) {
      WriteLogLine(log_fd, std::format("[mira-run] post_script (exit {}):\n{}\n", post.exit_code, post.output));
    }
  }

  [[maybe_unused]] auto write_end = proc::WriteSessionRecord(session_path, record);
  if (log_fd >= 0) ::close(log_fd);
  return 0;
}
