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
  // InstallButlerBinary extracts butler's own zip as-is (it ships nested
  // under "linux-amd64/", alongside .so files it needs there) rather than
  // flattening it -- so this has to search for the binary, not assume a
  // flat "tools/itch/butler" layout.
  const fs::path tools_dir = config.File().parent_path() / "tools" / "itch";
  std::error_code ec;
  for (const auto& entry : fs::recursive_directory_iterator(tools_dir, ec)) {
    if (entry.is_regular_file(ec) && entry.path().filename() == "butler") return entry.path();
  }
  return tools_dir / "butler";  // sensible default even if not installed yet
}

ItchStatus DetectButler(const config::Config& config) {
  const std::string override_path = config.GetString("itch.butler_bin");
  if (!override_path.empty() && fs::exists(override_path)) {
    return {.installed = true, .source = "override", .path = override_path, .version = VersionOf(override_path)};
  }

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

Result<std::int64_t> CurrentProfileId(const config::Config& config) {
  std::ifstream file(ApiKeyFile(config));
  if (!file) return Err("not_authenticated", "run \"mira itch login\" first");
  const std::string api_key((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

  // No separate persisted profile id (see this function's header comment)
  // -- re-authenticating with the stored key is how butlerd hands it back.
  const Result<nlohmann::json> result = Call(config, "Profile.LoginWithAPIKey", {{"apiKey", api_key}});
  if (!result) return std::unexpected(result.error());
  if (!result->contains("profile") || !(*result)["profile"].contains("id")) {
    return Err("itch_profile_missing", "butlerd didn't return a profile id");
  }
  return (*result)["profile"]["id"].get<std::int64_t>();
}

Result<void> EnsureInstallLocation(const config::Config& config) {
  const fs::path path = config.GetPath("itch.install_root");
  std::error_code ec;
  fs::create_directories(path, ec);  // butlerd expects it to already exist
  if (ec) return Err("install_dir_failed", ec.message());

  const Result<nlohmann::json> result =
    Call(config, "Install.Locations.Add", {{"id", "mira"}, {"path", path.string()}});
  if (!result) return std::unexpected(result.error());
  return {};
}

}  // namespace mira::itch
