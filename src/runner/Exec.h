#pragma once

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
// actual game launch is supervised, not blocked on (see the not-yet-built
// proc::ProcessSupervisor).
Result<ExecResult> RunAndWait(const Command& command);

// Absolute path to `name` if it's on $PATH, else nullopt. Used to check a
// runner's actual dependency (umu-run, wine) is installed, rather than
// discovering it "available" and only finding out it isn't when exec fails.
std::optional<std::string> FindOnPath(std::string_view name);

}  // namespace mira::runner
