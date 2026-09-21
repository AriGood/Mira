#pragma once

#include <filesystem>
#include <string>

#include "config/Config.h"
#include "core/Result.h"
#include "runner/Downloader.h"

namespace mira::itch {

struct ItchStatus {
  bool installed = false;
  std::string source;  // "override" | "managed" | "path" | "none"
  std::string path;
  std::string version;
};

std::filesystem::path ManagedButlerPath(const config::Config& config);

ItchStatus DetectButler(const config::Config& config);

Result<void> InstallButlerBinary(const config::Config& config, const runner::ReleaseAsset& asset);

// Mira-managed storage for the itch.io API key butlerd's Profile.
// LoginWithAPIKey needs -- butlerd itself is handed this key fresh at
// connect time (Butlerd.cpp), it isn't asked to remember it the way
// Legendary/gogdl persist their own tokens on disk, so Mira keeps it
// instead.
std::filesystem::path ApiKeyFile(const config::Config& config);

struct ItchAuthStatus {
  ItchStatus butler;
  bool authenticated = false;
};

// installed-only if butler itself isn't present; otherwise just whether a
// key is stored -- never connects to butlerd just to check this, matching
// gog::Status's "no cheap status call" posture (here doubly true: starting
// butlerd is a real subprocess spawn, not a quick one-shot command).
ItchAuthStatus Status(const config::Config& config);

// Stores `api_key` and verifies it immediately via Profile.LoginWithAPIKey
// (so a bad key is caught right here, not on the first real catalog
// call).
Result<void> Login(const config::Config& config, const std::string& api_key);

Result<void> Logout(const config::Config& config);

}  // namespace mira::itch
