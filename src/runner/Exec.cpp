#include "runner/Exec.h"

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

}  // namespace

Result<ExecResult> RunAndWait(const Command& command) {
  if (command.argv.empty()) return Err("exec_empty_argv", "no command to run");

  int pipe_fds[2];
  if (pipe(pipe_fds) != 0) return Err("exec_pipe_failed", std::strerror(errno));

  const pid_t pid = fork();
  if (pid < 0) return Err("exec_fork_failed", std::strerror(errno));

  if (pid == 0) {
    // Child: stdout+stderr -> pipe, then exec.
    dup2(pipe_fds[1], STDOUT_FILENO);
    dup2(pipe_fds[1], STDERR_FILENO);
    close(pipe_fds[0]);
    close(pipe_fds[1]);

    if (!command.cwd.empty()) {
      if (chdir(command.cwd.c_str()) != 0) _exit(127);
    }

    std::vector<char*> argv;
    for (const std::string& arg : command.argv) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    const std::vector<std::string> env_strings = MergedEnv(command);
    std::vector<char*> envp;
    for (const std::string& entry : env_strings) envp.push_back(const_cast<char*>(entry.c_str()));
    envp.push_back(nullptr);

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
