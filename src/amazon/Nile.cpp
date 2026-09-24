#include "amazon/Nile.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <ranges>

#include "core/Json.h"
#include "runner/Exec.h"

namespace mira::amazon {
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

fs::path ManagedNilePath(const config::Config& config) {
  return config.File().parent_path() / "tools" / "amazon" / "nile";
}

std::mutex pending_mutex;
std::optional<json> pending_login;  // client_id, code_verifier, serial

}  // namespace

NileStatus DetectNile(const config::Config& config) {
  const std::string override_path = config.GetString("amazon.nile_bin");
  if (!override_path.empty() && fs::exists(override_path)) {
    return {.installed = true, .source = "override", .path = override_path, .version = VersionOf(override_path)};
  }
  const fs::path managed = ManagedNilePath(config);
  if (fs::exists(managed)) {
    return {.installed = true, .source = "managed", .path = managed.string(), .version = VersionOf(managed.string())};
  }
  if (const auto on_path = runner::FindOnPath("nile")) {
    return {.installed = true, .source = "path", .path = *on_path, .version = VersionOf(*on_path)};
  }
  return {.installed = false, .source = "none", .path = "", .version = ""};
}

Result<void> InstallNileBinary(const config::Config& config, const runner::ReleaseAsset& asset) {
  auto installed = runner::InstallToolBinary(config, "amazon", asset, "nile");
  if (!installed) return std::unexpected(installed.error());
  return {};
}

fs::path NileConfigDir() {
  for (const char* var : {"NILE_CONFIG_PATH", "XDG_CONFIG_HOME"}) {
    if (const char* value = std::getenv(var); value && *value) return fs::path(value) / "nile";
  }
  const char* home = std::getenv("HOME");
  return fs::path(home ? home : "") / ".config" / "nile";
}

json ReadNileFile(const std::string& name) {
  std::ifstream file(NileConfigDir() / name);
  if (!file) return nullptr;
  const json parsed = json::parse(file, nullptr, false);
  return parsed.is_discarded() ? json(nullptr) : parsed;
}

Result<std::string> RunNile(const config::Config& config, std::vector<std::string> args) {
  const NileStatus status = DetectNile(config);
  if (!status.installed) {
    return Err("nile_missing",
               "nile isn't installed — run \"mira amazon setup\" to download it, or install it yourself "
               "and set amazon.nile_bin");
  }
  Command command;
  command.argv = {status.path};
  command.argv.insert(command.argv.end(), args.begin(), args.end());
  const Result<runner::ExecResult> result = runner::RunAndWait(command);
  if (!result) return std::unexpected(result.error());
  if (result->exit_code != 0) {
    return Err("nile_failed", std::format("nile exited {}: {}", result->exit_code, result->output));
  }
  return result->output;
}

AmazonAuthStatus Status(const config::Config& config) {
  AmazonAuthStatus status;
  status.nile = DetectNile(config);
  if (!status.nile.installed) return status;
  const Result<std::string> output = RunNile(config, {"auth", "--status"});
  if (!output) return status;
  const json parsed = core::ParseJsonTail(*output);
  status.authenticated = parsed.is_object() && parsed.value("LoggedIn", false);
  return status;
}

Result<std::string> BeginLogin(const config::Config& config) {
  const Result<std::string> output = RunNile(config, {"auth", "--login", "--non-interactive"});
  if (!output) return std::unexpected(output.error());
  const json parsed = core::ParseJsonTail(*output);
  if (!parsed.is_object() || !parsed.contains("url")) {
    return Status(config).authenticated ? Err("already_authenticated", "already logged in to Amazon")
                                        : Err("nile_json_error", "nile didn't return a login URL");
  }
  const std::lock_guard lock(pending_mutex);
  pending_login = parsed;
  return parsed.value("url", std::string());
}

Result<void> FinishLogin(const config::Config& config, const std::string& redirect) {
  json pending;
  {
    const std::lock_guard lock(pending_mutex);
    if (!pending_login) return Err("no_login_pending", "start a login first (mira amazon login)");
    pending = *pending_login;
  }
  constexpr std::string_view kMarker = "openid.oa2.authorization_code=";
  std::string code = redirect;
  if (const std::size_t at = redirect.find(kMarker); at != std::string::npos) {
    const std::size_t start = at + kMarker.size();
    code = redirect.substr(start, redirect.find('&', start) - start);
  }
  if (auto registered = RunNile(config, {"register", "--code", code, "--client-id", pending.value("client_id", ""),
                                         "--code-verifier", pending.value("code_verifier", ""), "--serial",
                                         pending.value("serial", "")});
      !registered) {
    return Err("login_failed", "nile couldn't register this device — the code may be wrong or expired");
  }
  {
    const std::lock_guard lock(pending_mutex);
    pending_login.reset();
  }
  if (!Status(config).authenticated) {
    return Err("login_failed", "nile didn't accept that code — it may be wrong, expired, or already used");
  }
  return {};
}

Result<void> Logout(const config::Config& config) {
  if (auto output = RunNile(config, {"auth", "--logout"}); !output) return std::unexpected(output.error());
  return {};
}

}  // namespace mira::amazon
