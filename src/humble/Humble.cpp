#include "humble/Humble.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <ranges>

#include "config/RunnerSources.h"
#include "runner/Exec.h"

namespace mira::humble {
namespace {
namespace fs = std::filesystem;

std::string Trim(std::string text) {
  const auto not_space = [](unsigned char c) { return !std::isspace(c); };
  text.erase(text.begin(), std::ranges::find_if(text, not_space));
  text.erase(std::ranges::find_if(text | std::views::reverse, not_space).base(), text.end());
  return text;
}

std::string VersionOf(const std::string& path) {
  Command command;
  command.argv = {path, "--version"};
  const Result<runner::ExecResult> result = runner::RunAndWait(command);
  if (!result || result->exit_code != 0) return {};
  return Trim(result->output);
}

// Splits one table line on runs of 2+ spaces -- Go's text/tabwriter (what
// a Cobra CLI's own table output is virtually always built with) pads
// columns with spaces, not tabs, in a rendered terminal-width table.
// Not confirmed against real purchase data (no account available), so
// this is a best-effort convention match, not a verified contract —
// callers treat a row that doesn't split into the expected number of
// fields as unparseable and skip it rather than guessing.
std::vector<std::string> SplitColumns(const std::string& line) {
  std::vector<std::string> columns;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && line[i] == ' ') ++i;
    const size_t start = i;
    while (i < line.size()) {
      if (line[i] == ' ' && i + 1 < line.size() && line[i + 1] == ' ') break;
      ++i;
    }
    if (i > start) columns.push_back(Trim(line.substr(start, i - start)));
  }
  return columns;
}

}  // namespace

std::filesystem::path ManagedHumbleCliPath(const config::Config& config) {
  return config.File().parent_path() / "tools" / "humble" / std::string(config::runner_sources::kHumbleCliBinaryName);
}

HumbleStatus DetectHumbleCli(const config::Config& config) {
  const std::string override_path = config.GetString("humble.humble_cli_bin");
  if (!override_path.empty() && fs::exists(override_path)) {
    return {.installed = true, .source = "override", .path = override_path, .version = VersionOf(override_path)};
  }
  const fs::path managed = ManagedHumbleCliPath(config);
  if (fs::exists(managed)) {
    return {.installed = true, .source = "managed", .path = managed.string(), .version = VersionOf(managed.string())};
  }
  if (const auto on_path = runner::FindOnPath("humble-cli")) {
    return {.installed = true, .source = "path", .path = *on_path, .version = VersionOf(*on_path)};
  }
  return {.installed = false, .source = "none", .path = "", .version = ""};
}

Result<void> InstallHumbleCliBinary(const config::Config& config, const runner::ReleaseAsset& asset) {
  auto installed =
    runner::InstallToolBinary(config, "humble", asset, std::string(config::runner_sources::kHumbleCliBinaryName));
  if (!installed) return std::unexpected(installed.error());
  return {};
}

Result<std::string> RunHumbleCli(const config::Config& config, std::vector<std::string> args) {
  const HumbleStatus status = DetectHumbleCli(config);
  if (!status.installed) {
    return Err("humble_cli_missing",
               "humble-cli isn't installed — run \"mira humble setup\" to download it, or install it "
               "yourself and set humble.humble_cli_bin");
  }
  Command command;
  command.argv = {status.path};
  command.argv.insert(command.argv.end(), args.begin(), args.end());
  const Result<runner::ExecResult> result = runner::RunAndWait(command);
  if (!result) return std::unexpected(result.error());
  if (result->exit_code != 0) {
    return Err("humble_cli_failed", std::format("humble-cli exited {}: {}", result->exit_code, result->output));
  }
  return result->output;
}

HumbleAuthStatus Status(const config::Config& config) {
  HumbleAuthStatus status;
  status.humble_cli = DetectHumbleCli(config);
  if (!status.humble_cli.installed) return status;

  // No separate "am I logged in" call exists -- `list` is the cheapest
  // real one, and fails with a specific, recognizable message
  // ("config file not found...") when no session key has been set yet
  // (confirmed live).
  const Result<std::string> listed = RunHumbleCli(config, {"list", "--field", "key"});
  status.authenticated = listed.has_value();
  return status;
}

Result<void> Login(const config::Config& config, const std::string& session_key) {
  if (auto output = RunHumbleCli(config, {"auth", session_key}); !output) return std::unexpected(output.error());
  if (const HumbleAuthStatus status = Status(config); !status.authenticated) {
    return Err("login_failed", "humble-cli didn't accept that session key");
  }
  return {};
}

Result<std::vector<BundleSummary>> ListBundles(const config::Config& config) {
  const Result<std::string> output = RunHumbleCli(config, {"list", "--field", "key", "--field", "name",
                                                          "--field", "claimed"});
  if (!output) return std::unexpected(output.error());

  std::vector<BundleSummary> bundles;
  size_t pos = 0;
  bool skipped_header = false;
  while (pos <= output->size()) {
    const size_t newline = output->find('\n', pos);
    const std::string line = output->substr(pos, newline == std::string::npos ? std::string::npos : newline - pos);
    if (newline == std::string::npos) pos = output->size() + 1;
    else pos = newline + 1;

    const std::string trimmed = Trim(line);
    if (trimmed.empty()) continue;
    const std::vector<std::string> columns = SplitColumns(trimmed);
    if (columns.size() < 2) continue;

    // A table's own header row ("KEY  NAME  CLAIMED") isn't a bundle --
    // skip the first row whose first column case-insensitively reads
    // "key", rather than assuming a fixed number of header lines.
    if (!skipped_header) {
      std::string lowered = columns[0];
      std::ranges::transform(lowered, lowered.begin(), [](unsigned char c) { return std::tolower(c); });
      if (lowered == "key") {
        skipped_header = true;
        continue;
      }
    }

    BundleSummary bundle;
    bundle.key = columns[0];
    bundle.name = columns[1];
    bundle.claimed = columns.size() > 2 && (columns[2] == "yes" || columns[2] == "true");
    bundles.push_back(std::move(bundle));
  }
  return bundles;
}

std::filesystem::path DownloadDir(const config::Config& config, const std::string& bundle_key) {
  return config.GetPath("humble.download_root") / bundle_key;
}

Result<void> Download(const config::Config& config, const std::string& bundle_key, const std::string& item_numbers) {
  const fs::path dir = DownloadDir(config, bundle_key);
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) return Err("download_dir_failed", ec.message());

  const HumbleStatus status = DetectHumbleCli(config);
  if (!status.installed) return Err("humble_cli_missing", "run \"mira humble setup\" first");

  Command command;
  command.argv = {status.path, "download", bundle_key, "--cur-dir"};
  if (!item_numbers.empty()) {
    command.argv.push_back("--item-numbers");
    command.argv.push_back(item_numbers);
  }
  command.cwd = dir;
  const Result<runner::ExecResult> result = runner::RunAndWait(command);
  if (!result) return std::unexpected(result.error());
  if (result->exit_code != 0) {
    return Err("download_failed", std::format("humble-cli exited {}: {}", result->exit_code, result->output));
  }
  return {};
}

}  // namespace mira::humble
