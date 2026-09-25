#include "epic/Legendary.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <ranges>
#include <string>
#include <string_view>

#include "core/Json.h"
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

  // Legendary's releases ship no checksum, so it's installed unverified.
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

  const json parsed = core::ParseJsonTail(*output);
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

  const json parsed = core::ParseJsonTail(result->output);
  if (parsed.is_discarded() || !parsed.is_object()) return status;

  // legendary always includes this key -- logged out isn't its absence, it's
  // this literal placeholder string.
  const std::string account = parsed.value("account", std::string());
  if (!account.empty() && account != "<not logged in>") {
    status.authenticated = true;
    status.account = account;
  }
  return status;
}

Result<void> Login(const config::Config& config, const std::string& pasted) {
  std::string code = Trim(pasted);
  if (code.starts_with('{')) {
    const json page = json::parse(code, nullptr, false);
    if (page.is_discarded() || !page.contains("authorizationCode") || !page["authorizationCode"].is_string()) {
      return Err("invalid_code", "that looks like JSON but has no \"authorizationCode\" field");
    }
    code = page["authorizationCode"].get<std::string>();
  }
  if (code.empty()) return Err("invalid_code", "no code entered");
  // legendary's own exit code is not trustworthy here: `auth --code` with an
  // invalid or expired code still exits 0, only reporting the failure as an
  // "[cli] ERROR: Login attempt failed" line in its output, so success is
  // checked through status afterwards.
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
