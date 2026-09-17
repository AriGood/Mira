#pragma once

#include <sys/types.h>

#include <optional>
#include <string>

#include "core/Command.h"
#include "core/Result.h"

namespace mira::runner {

struct ExecResult {
  int exit_code = -1;
  std::string output;  // combined stdout+stderr
};

// Blocking: runs `command`, waits for it to exit. Provisioning only — an
// actual game launch is supervised, not blocked on (see
// proc::ProcessSupervisor).
Result<ExecResult> RunAndWait(const Command& command);

// Starts `command` and returns its pid immediately, without waiting. For
// launching a game: unlike provisioning, the caller must not block, and the
// child's output is left on the daemon's own stdout/stderr rather than
// captured, so it lands in journalctl next to everything else.
Result<pid_t> SpawnDetached(const Command& command);

// Absolute path to `name` if it's on $PATH, else nullopt. Used to check a
// runner's actual dependency (umu-run, wine) is installed, rather than
// discovering it "available" and only finding out it isn't when exec fails.
std::optional<std::string> FindOnPath(std::string_view name);

}  // namespace mira::runner
