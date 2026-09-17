#include "runner/Exec.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

#include "core/Strings.h"

extern char** environ;

namespace mira::runner {

std::optional<std::string> FindOnPath(std::string_view name) {
  const char* path_env = std::getenv("PATH");
  if (path_env == nullptr) return std::nullopt;

  for (const std::string& dir : strings::Split(path_env, ':')) {
    if (dir.empty()) continue;
    const std::filesystem::path candidate = std::filesystem::path(dir) / name;
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec) && access(candidate.c_str(), X_OK) == 0) {
      return candidate.string();
    }
  }
  return std::nullopt;
}

namespace {

// Command.env is an overlay on the daemon's own environment, not a
// replacement — merge them for the child process.
std::vector<std::string> MergedEnv(const Command& command) {
  std::vector<std::string> merged;
  for (char** e = environ; *e != nullptr; ++e) merged.emplace_back(*e);
  for (const auto& [key, value] : command.env) merged.push_back(key + "=" + value);
  return merged;
}

// argv/envp built before fork() and only indexed into afterwards — see the
// deadlock note in RunAndWait.
struct PreparedCommand {
  std::vector<std::string> env_strings;
  std::vector<char*> argv;
  std::vector<char*> envp;
  std::string cwd;
};

PreparedCommand Prepare(const Command& command) {
  PreparedCommand prepared;
  prepared.env_strings = MergedEnv(command);
  prepared.argv.reserve(command.argv.size() + 1);
  for (const std::string& arg : command.argv) {
    prepared.argv.push_back(const_cast<char*>(arg.c_str()));
  }
  prepared.argv.push_back(nullptr);
  prepared.envp.reserve(prepared.env_strings.size() + 1);
  for (const std::string& entry : prepared.env_strings) {
    prepared.envp.push_back(const_cast<char*>(entry.c_str()));
  }
  prepared.envp.push_back(nullptr);
  prepared.cwd = command.cwd.string();
  return prepared;
}

}  // namespace

Result<pid_t> SpawnDetached(const Command& command) {
  if (command.argv.empty()) return Err("exec_empty_argv", "no command to run");
  PreparedCommand prepared = Prepare(command);

  const pid_t pid = fork();
  if (pid < 0) return Err("exec_fork_failed", std::strerror(errno));
  if (pid == 0) {
    // Child: async-signal-safe calls only. stdout/stderr are inherited.
    // Its own process group, so stopping the game can signal the whole tree:
    // a real launch is umu -> proton -> wine -> game.exe, and signalling just
    // the direct child leaves the actual game running.
    setpgid(0, 0);
    if (!prepared.cwd.empty() && chdir(prepared.cwd.c_str()) != 0) _exit(127);
    execvpe(prepared.argv[0], prepared.argv.data(), prepared.envp.data());
    _exit(127);
  }
  return pid;
}

Result<ExecResult> RunAndWait(const Command& command) {
  if (command.argv.empty()) return Err("exec_empty_argv", "no command to run");

  // Everything the child needs is built *before* fork(). mirad forks from
  // threads (the scan thread, httplib workers) while others run, and only
  // async-signal-safe calls are legal in the child: a malloc there deadlocks
  // forever if another thread happened to hold the arena lock at fork time,
  // and the parent then blocks forever too, since its read() waits for an
  // EOF the wedged child will never deliver.
  const std::vector<std::string> env_strings = MergedEnv(command);
  std::vector<char*> argv;
  argv.reserve(command.argv.size() + 1);
  for (const std::string& arg : command.argv) argv.push_back(const_cast<char*>(arg.c_str()));
  argv.push_back(nullptr);
  std::vector<char*> envp;
  envp.reserve(env_strings.size() + 1);
  for (const std::string& entry : env_strings) envp.push_back(const_cast<char*>(entry.c_str()));
  envp.push_back(nullptr);
  const std::string cwd = command.cwd.string();

  // O_CLOEXEC: two RunAndWait calls can overlap (a scan thread provisioning
  // while an HTTP request probes `wine --version`). Without it, one child
  // inherits the other's write end and holds the reader open until it exits.
  int pipe_fds[2];
  if (pipe2(pipe_fds, O_CLOEXEC) != 0) return Err("exec_pipe_failed", std::strerror(errno));

  const pid_t pid = fork();
  if (pid < 0) {
    const std::string message = std::strerror(errno);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return Err("exec_fork_failed", message);
  }

  if (pid == 0) {
    // Child: async-signal-safe calls only from here down. dup2 clears
    // O_CLOEXEC on the copies, so stdout/stderr survive exec while the
    // originals close themselves.
    dup2(pipe_fds[1], STDOUT_FILENO);
    dup2(pipe_fds[1], STDERR_FILENO);
    if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(127);
    execvpe(argv[0], argv.data(), envp.data());
    _exit(127);  // only reached if exec failed
  }

  // Parent.
  close(pipe_fds[1]);
  ExecResult result;
  char buffer[4096];
  ssize_t n;
  while ((n = read(pipe_fds[0], buffer, sizeof(buffer))) > 0) result.output.append(buffer, static_cast<size_t>(n));
  close(pipe_fds[0]);

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) return Err("exec_wait_failed", std::strerror(errno));
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return result;
}

}  // namespace mira::runner
