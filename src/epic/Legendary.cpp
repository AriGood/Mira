#include "epic/Legendary.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <ranges>
#include <string>

#include "core/Log.h"
#include "runner/Exec.h"

namespace mira::epic {
namespace {
namespace fs = std::filesystem;
using nlohmann::json;

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

}  // namespace

std::filesystem::path ManagedLegendaryPath(const config::Config& config) {
  return config.File().parent_path() / "tools" / "legendary";
}

LegendaryStatus DetectLegendary(const config::Config& config) {
  const std::string override_path = config.GetString("epic.legendary_bin");
  if (!override_path.empty() && fs::exists(override_path)) {
    return {.installed = true, .source = "override", .path = override_path, .version = VersionOf(override_path)};
  }

  const fs::path managed = ManagedLegendaryPath(config);
  if (fs::exists(managed)) {
    return {.installed = true, .source = "managed", .path = managed.string(), .version = VersionOf(managed.string())};
  }

  if (const auto on_path = runner::FindOnPath("legendary")) {
    return {.installed = true, .source = "path", .path = *on_path, .version = VersionOf(*on_path)};
  }

  return {.installed = false, .source = "none", .path = "", .version = ""};
}

Result<void> InstallLegendaryBinary(const config::Config& config, const runner::ReleaseAsset& asset) {
  const fs::path target = ManagedLegendaryPath(config);
  std::error_code ec;
  fs::create_directories(target.parent_path(), ec);
  if (ec) return Err("install_dir_failed", ec.message());

  Command download;
  download.argv = {"curl", "-sSL", "-o", target.string(), asset.download_url};
  const Result<runner::ExecResult> result = runner::RunAndWait(download);
  if (!result || result->exit_code != 0) {
    fs::remove(target, ec);
    return Err("download_failed", !result ? result.error().message
                                          : std::format("curl exited {}: {}", result->exit_code, result->output));
  }

  // Legendary's releases don't ship a separate checksum asset the way
  // Proton-GE/Wine-GE do (runner::ReleaseAsset::checksum_url is always empty
  // here) — installed unverified, same fallback Downloader.cpp uses when a
  // release happens to have none.
  log::Warn("no checksum available for legendary {} — installing unverified", asset.tag);

  fs::permissions(target,
                  fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                      fs::perms::others_exec,
                  ec);
  if (ec) return Err("chmod_failed", ec.message());
  return {};
}

Result<std::string> RunLegendary(const config::Config& config, std::vector<std::string> args) {
  const LegendaryStatus status = DetectLegendary(config);
  if (!status.installed) {
    return Err("legendary_missing",
               "legendary isn't installed — run \"mira epic setup\" to download it, or install it "
               "yourself and set epic.legendary_bin");
  }

  Command command;
  command.argv = {status.path};
  command.argv.insert(command.argv.end(), args.begin(), args.end());
  const Result<runner::ExecResult> result = runner::RunAndWait(command);
  if (!result) return std::unexpected(result.error());
  if (result->exit_code != 0) {
    return Err("legendary_failed", std::format("legendary exited {}: {}", result->exit_code, result->output));
  }
  return result->output;
}

Result<json> RunLegendaryJson(const config::Config& config, std::vector<std::string> args) {
  args.push_back("--json");
  const Result<std::string> output = RunLegendary(config, args);
  if (!output) return std::unexpected(output.error());

  const json parsed = json::parse(*output, nullptr, false);
  if (parsed.is_discarded()) return Err("legendary_json_error", "legendary's output wasn't valid JSON");
  return parsed;
}

EpicAuthStatus Status(const config::Config& config) {
  EpicAuthStatus status;
  status.legendary = DetectLegendary(config);
  if (!status.legendary.installed) return status;  // authenticated=false, no subprocess needed

  // Not RunLegendaryJson: "not logged in" is an ordinary result of this
  // specific call, not an error to propagate — a failed/unparseable run
  // just leaves authenticated=false rather than failing the whole status
  // call the way every other legendary invocation here does.
  Command command;
  command.argv = {status.legendary.path, "status", "--json"};
  const Result<runner::ExecResult> result = runner::RunAndWait(command);
  if (!result) return status;

  const json parsed = json::parse(result->output, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) return status;

  const std::string account = parsed.value("account", std::string());
  if (!account.empty()) {
    status.authenticated = true;
    status.account = account;
  }
  return status;
}

Result<void> Login(const config::Config& config, const std::string& code) {
  const Result<std::string> output = RunLegendary(config, {"auth", "--code", code});
  if (!output) return std::unexpected(output.error());
  return {};
}

Result<void> Logout(const config::Config& config) {
  const Result<std::string> output = RunLegendary(config, {"auth", "--delete"});
  if (!output) return std::unexpected(output.error());
  return {};
}

}  // namespace mira::epic
