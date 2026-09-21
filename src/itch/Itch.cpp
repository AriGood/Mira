#include "itch/Itch.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <ranges>

#include <json.hpp>

#include "itch/Butlerd.h"
#include "runner/Exec.h"

namespace mira::itch {
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

}  // namespace

std::filesystem::path ManagedButlerPath(const config::Config& config) {
  return config.File().parent_path() / "tools" / "itch" / "butler";
}

ItchStatus DetectButler(const config::Config& config) {
  const fs::path managed = ManagedButlerPath(config);
  if (fs::exists(managed)) {
    return {.installed = true, .source = "managed", .path = managed.string(), .version = VersionOf(managed.string())};
  }
  if (const auto on_path = runner::FindOnPath("butler")) {
    return {.installed = true, .source = "path", .path = *on_path, .version = VersionOf(*on_path)};
  }
  return {.installed = false, .source = "none", .path = "", .version = ""};
}

Result<void> InstallButlerBinary(const config::Config& config, const runner::ReleaseAsset& asset) {
  auto installed = runner::InstallToolBinary(config, "itch", asset, "butler");
  if (!installed) return std::unexpected(installed.error());
  return {};
}

std::filesystem::path ApiKeyFile(const config::Config& config) {
  return config.File().parent_path() / "itch-api-key";
}

ItchAuthStatus Status(const config::Config& config) {
  ItchAuthStatus status;
  status.butler = DetectButler(config);
  if (!status.butler.installed) return status;

  std::error_code ec;
  status.authenticated = fs::exists(ApiKeyFile(config), ec) && fs::file_size(ApiKeyFile(config), ec) > 0;
  return status;
}

Result<void> Login(const config::Config& config, const std::string& api_key) {
  const fs::path key_file = ApiKeyFile(config);
  std::error_code ec;
  fs::create_directories(key_file.parent_path(), ec);
  {
    std::ofstream file(key_file, std::ios::trunc);
    if (!file) return Err("write_failed", "couldn't write " + key_file.string());
    file << api_key;
  }
  fs::permissions(key_file, fs::perms::owner_read | fs::perms::owner_write, ec);

  const Result<nlohmann::json> result = Call(config, "Profile.LoginWithAPIKey", {{"apiKey", api_key}});
  if (!result) {
    fs::remove(key_file, ec);
    return std::unexpected(result.error());
  }
  return {};
}

Result<void> Logout(const config::Config& config) {
  std::error_code ec;
  fs::remove(ApiKeyFile(config), ec);
  return {};
}

}  // namespace mira::itch
