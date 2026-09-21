#include "gog/Gog.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>
#include <fstream>
#include <optional>
#include <ranges>

#include <json.hpp>

#include "core/Json.h"
#include "core/Log.h"
#include "runner/Exec.h"

namespace mira::gog {
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

std::optional<json> ReadAuthConfig(const config::Config& config) {
  std::ifstream file(AuthConfigPath(config));
  if (!file) return std::nullopt;
  const json parsed = json::parse(file, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) return std::nullopt;
  return parsed;
}

}  // namespace

std::filesystem::path ManagedGogPath(const config::Config& config) {
  return config.File().parent_path() / "tools" / "gog" / "gogdl";
}

GogStatus DetectGog(const config::Config& config) {
  const std::string override_path = config.GetString("gog.gogdl_bin");
  if (!override_path.empty() && fs::exists(override_path)) {
    return {.installed = true, .source = "override", .path = override_path, .version = VersionOf(override_path)};
  }

  const fs::path managed = ManagedGogPath(config);
  if (fs::exists(managed)) {
    return {.installed = true, .source = "managed", .path = managed.string(), .version = VersionOf(managed.string())};
  }

  if (const auto on_path = runner::FindOnPath("gogdl")) {
    return {.installed = true, .source = "path", .path = *on_path, .version = VersionOf(*on_path)};
  }

  return {.installed = false, .source = "none", .path = "", .version = ""};
}

Result<void> InstallGogBinary(const config::Config& config, const runner::ReleaseAsset& asset) {
  // gogdl is a Python zipapp ("#!/usr/bin/env python3" shebang, confirmed
  // against a real release), not a self-contained binary — a system
  // python3 has to actually be there for it to run at all, unlike
  // Legendary. Checked here rather than only at first use, so
  // "mira gog setup" fails with a clear, actionable error immediately
  // instead of leaving a binary that can't execute.
  if (!runner::FindOnPath("python3")) {
    return Err("python3_missing",
              "gogdl needs a system python3 to run (it's a Python zipapp, not a standalone binary) — "
              "install python3 first");
  }
  auto installed = runner::InstallToolBinary(config, "gog", asset, "gogdl");
  if (!installed) return std::unexpected(installed.error());
  return {};
}

std::filesystem::path AuthConfigPath(const config::Config& config) {
  return config.File().parent_path() / "gog-auth.json";
}

Result<std::string> RunGogdl(const config::Config& config, std::vector<std::string> args) {
  const GogStatus status = DetectGog(config);
  if (!status.installed) {
    return Err("gogdl_missing",
               "gogdl isn't installed — run \"mira gog setup\" to download it, or install it yourself "
               "and set gog.gogdl_bin");
  }

  Command command;
  command.argv = {status.path, "--auth-config-path", AuthConfigPath(config).string()};
  command.argv.insert(command.argv.end(), args.begin(), args.end());
  const Result<runner::ExecResult> result = runner::RunAndWait(command);
  if (!result) return std::unexpected(result.error());
  if (result->exit_code != 0) {
    return Err("gogdl_failed", std::format("gogdl exited {}: {}", result->exit_code, result->output));
  }
  return result->output;
}

GogAuthStatus Status(const config::Config& config) {
  GogAuthStatus status;
  status.gogdl = DetectGog(config);
  if (!status.gogdl.installed) return status;

  const auto stored = ReadAuthConfig(config);
  if (!stored) return status;
  status.authenticated = stored->contains("access_token") && (*stored)["access_token"].is_string() &&
                        !(*stored)["access_token"].get<std::string>().empty();
  return status;
}

Result<void> Login(const config::Config& config, const std::string& code) {
  // Same posture as epic::Login: verify by re-reading what gogdl actually
  // wrote, not by trusting a nonzero/zero exit code — gogdl's own `auth`
  // handler prints {"error": true} on a rejected code but still exits 0
  // (confirmed live: `gogdl auth --code <bogus>` -> `{"error": true}`,
  // exit 0).
  if (auto output = RunGogdl(config, {"auth", "--code", code}); !output) {
    return std::unexpected(output.error());
  }
  if (const GogAuthStatus status = Status(config); !status.authenticated) {
    return Err("login_failed", "gogdl didn't accept that code — it may be wrong, expired, or already used");
  }
  return {};
}

Result<void> Logout(const config::Config& config) {
  std::error_code ec;
  fs::remove(AuthConfigPath(config), ec);
  return {};
}

Result<std::string> AccessToken(const config::Config& config) {
  auto stored = ReadAuthConfig(config);
  bool expired = true;
  if (stored && stored->contains("loginTime") && stored->contains("expires_in")) {
    const double login_time = stored->value("loginTime", 0.0);
    const double expires_in = stored->value("expires_in", 0.0);
    const double now = static_cast<double>(std::chrono::duration_cast<std::chrono::seconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count());
    expired = now >= login_time + expires_in;
  }

  if (!stored || expired) {
    // gogdl's own load path refreshes an expired token using its stored
    // refresh_token as a side effect of loading --auth-config-path at all
    // (confirmed in heroic-gogdl's auth.py: is_credential_expired/
    // refresh_credentials run before any subcommand does its own work) —
    // triggering that is as simple as invoking gogdl with no real work to
    // do.
    if (auto refreshed = RunGogdl(config, {"auth"}); !refreshed) return std::unexpected(refreshed.error());
    stored = ReadAuthConfig(config);
  }

  if (!stored || !stored->contains("access_token") || !(*stored)["access_token"].is_string()) {
    return Err("not_authenticated", "run \"mira gog login\" first");
  }
  return (*stored)["access_token"].get<std::string>();
}

}  // namespace mira::gog
