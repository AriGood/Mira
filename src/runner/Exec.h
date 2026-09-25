#pragma once

#include <sys/types.h>

#include <filesystem>
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

// Like SpawnDetached, but the child's file descriptor 3 is connected to a
// pipe whose read end is returned via `status_read_fd` — for spawning
// mira-run with a --status-fd 3 it can report "ready" or "pre_launch
// failed" on, without mirad blocking on the whole process the way
// RunAndWait does. The caller owns the returned fd and must close it.
Result<pid_t> SpawnDetachedWithStatus(const Command& command, int& status_read_fd);

// Extracts `archive` into `out_dir`: tarballs with tar, which keeps symlinks
// and permissions, and everything else (zip, rar, 7z, split volumes) with
// 7-Zip.
Result<void> Extract(const std::filesystem::path& archive, const std::filesystem::path& out_dir);

// Absolute path to `name` if it's on $PATH, else nullopt. Used to check a
// runner's actual dependency (umu-run, wine) is installed, rather than
// discovering it "available" and only finding out it isn't when exec fails.
std::optional<std::string> FindOnPath(std::string_view name);

// Resolves `name` next to `own_binary_dir` first (an install where every
// Mira binary sits in one directory -- an AppImage, a dev build), else falls
// back to $PATH. A pure function so it's testable without touching
// /proc/self/exe -- the caller resolves its own directory and passes it in,
// same split as frontend/ui/DaemonSupervisor.cpp's ResolveMiradPath.
std::optional<std::string> ResolveSiblingBinary(const std::filesystem::path& own_binary_dir, std::string_view name);

}  // namespace mira::runner
