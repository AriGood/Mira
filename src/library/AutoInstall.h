#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "config/Config.h"
#include "core/Result.h"
#include "model/Types.h"
#include "store/GameStore.h"

namespace mira::library {

enum class InstallerFormat { kUnknown, kInnoSetup, kNsis, kMsi };

InstallerFormat DetectInstallerFormat(const std::filesystem::path& file);
std::string_view ToString(InstallerFormat format);

struct InstallerInfo {
  std::filesystem::path path;
  std::uintmax_t size_bytes = 0;
  InstallerFormat format = InstallerFormat::kUnknown;
  bool silent = false;       // a known format Mira can run unattended
  std::string silent_args;  // empty unless silent
};

// The installer a needs_install game points at (its exe_path).
Result<InstallerInfo> DescribeInstaller(const config::Config& config, const model::Game& game);

struct InstallProgress {
  std::string state;  // queued, running, finished, failed
  std::string mode;   // silent or interactive, once running
  std::int64_t started_at = 0;
  std::int64_t finished_at = 0;
  std::string error;
  std::uintmax_t bytes_written = 0;  // growth of the install folder and new drive_c folders
};

// The latest install for `id` since mirad started, if any.
std::optional<InstallProgress> Progress(const std::string& id);

enum class InstallMode {
  kSilentOnly,   // known formats only, unattended (scanner)
  kAuto,         // silent when the format is known, otherwise shown for the user to click through
  kInteractive,  // always shown
};

// Queues `id` for an install; false if one is already queued or running.
bool BeginInstall(const std::string& id);

// Runs a needs_install (or broken) game's installer in its prefix, one at a
// time, then finds the game exe in install_path or a new drive_c folder.
// `installer` overrides exe_path and is saved as the game's installer.
// Saves the result: Ready on success, last_error on failure.
Result<model::Game> Install(config::Config& config, store::GameStore& games, const std::string& id,
                            InstallMode mode, const std::optional<std::filesystem::path>& installer);

}  // namespace mira::library
