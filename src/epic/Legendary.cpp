#include "epic/Legendary.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <ranges>
#include <string>
#include <string_view>

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

// runner::ExecResult combines stdout+stderr into one string, but legendary
// writes its own log lines (e.g. "[Core] INFO: Trying to re-use existing
// login session...", "[cli] INFO: Getting game list...") to stderr and only
// the actual --json payload to stdout -- so that combined text has log
// noise around the JSON whenever there's something to log about (confirmed
// against a real install: `status --json` is noisy once authenticated,
// `list --json` always logs while it works). A naive "first '{' or '['"
// scan isn't enough either: legendary's own log lines are tagged
// "[Core] ...", "[cli] ..." -- a literal '[' that isn't the JSON's own
// opener. Instead: legendary always emits the actual --json payload as one
// complete line, the last line of output that's valid JSON on its own --
// scanned for from the end so real log lines (which never parse as JSON)
// are skipped over regardless of what stray brackets they contain.
json ParseJsonTail(const std::string& text) {
  size_t line_end = text.size();
  while (line_end > 0) {
    size_t line_start = text.rfind('\n', line_end - 1);
    line_start = (line_start == std::string::npos) ? 0 : line_start + 1;
    const std::string_view line(text.data() + line_start, line_end - line_start);
    const size_t first = line.find_first_not_of(" \t\r");
    if (first != std::string_view::npos && (line[first] == '{' || line[first] == '[')) {
      const json parsed = json::parse(line.substr(first), nullptr, false);
      if (!parsed.is_discarded()) return parsed;
    }
    if (line_start == 0) break;
    line_end = line_start - 1;
  }
  return json();
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

std::filesystem::path LegendaryMetadataFile(const std::string& app_name) {
  const char* xdg_config_home = std::getenv("XDG_CONFIG_HOME");
  fs::path config_dir;
  if (xdg_config_home && *xdg_config_home) {
    config_dir = xdg_config_home;
  } else {
    const char* home = std::getenv("HOME");
    config_dir = (home && *home ? fs::path(home) : fs::path()) / ".config";
  }
  return config_dir / "legendary" / "metadata" / (app_name + ".json");
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

  const json parsed = ParseJsonTail(*output);
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

  const json parsed = ParseJsonTail(result->output);
  if (parsed.is_discarded() || !parsed.is_object()) return status;

  // legendary always includes this key -- logged out isn't its absence, it's
  // this literal placeholder string (confirmed against a real install).
  const std::string account = parsed.value("account", std::string());
  if (!account.empty() && account != "<not logged in>") {
    status.authenticated = true;
    status.account = account;
  }
  return status;
}

Result<void> Login(const config::Config& config, const std::string& code) {
  // legendary's own exit code is not trustworthy here: `auth --code` with an
  // invalid or expired code still exits 0, only reporting the failure as an
  // "[cli] ERROR: Login attempt failed" line in its output (confirmed against
  // a real install) -- so success is verified the same way a caller would,
  // by checking status afterward, not by trusting this call's exit code.
  if (auto output = RunLegendary(config, {"auth", "--code", code}); !output) {
    return std::unexpected(output.error());
  }
  if (const EpicAuthStatus status = Status(config); !status.authenticated) {
    return Err("login_failed", "legendary didn't accept that code — it may be wrong, expired, or already used");
  }
  return {};
}

Result<void> Logout(const config::Config& config) {
  const Result<std::string> output = RunLegendary(config, {"auth", "--delete"});
  if (!output) return std::unexpected(output.error());
  return {};
}

}  // namespace mira::epic
