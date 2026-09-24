#include "itch/ItchInstaller.h"

#include <chrono>

#include <filesystem>

#include <json.hpp>

#include "itch/Butlerd.h"
#include "itch/Itch.h"
#include "itch/ItchImporter.h"

namespace mira::itch {
namespace {
using nlohmann::json;

Result<void> CheckReady(const config::Config& config) {
  const ItchAuthStatus auth = Status(config);
  if (!auth.butler.installed) return Err("butler_missing", "run \"mira itch setup\" first");
  if (!auth.authenticated) return Err("not_authenticated", "run \"mira itch login\" first");
  return {};
}

}  // namespace

ItchInstaller::ItchInstaller(config::Config& config, store::GameStore& games, api::EventBus& events)
    : config_(config), games_(games), events_(events) {}

Result<void> ItchInstaller::Run(const std::string& game_id) {
  if (auto ready = CheckReady(config_); !ready) return ready;

  const Result<std::int64_t> profile_id = CurrentProfileId(config_);
  if (!profile_id) return std::unexpected(profile_id.error());
  if (auto location = EnsureInstallLocation(config_); !location) return std::unexpected(location.error());

  // Two-call sequence confirmed against butlerd's own spec: Install.Queue
  // picks an upload and returns {id, stagingFolder}; Install.Perform
  // fetches it using exactly those two values back.
  const Result<json> queued = Call(config_, "Install.Queue",
                                  {{"game", {{"id", std::stoll(game_id)}}},
                                   {"profileId", *profile_id},
                                   {"installLocationId", "mira"}});
  if (!queued) return std::unexpected(queued.error());

  const std::string install_id = queued->value("id", std::string());
  const std::string staging_folder = queued->value("stagingFolder", std::string());
  if (install_id.empty() || staging_folder.empty()) {
    return Err("itch_queue_failed", "Install.Queue didn't return an id/stagingFolder");
  }

  // Progress arrives as notifications; passed on about once a second.
  auto last_sent = std::chrono::steady_clock::time_point{};
  const auto on_notification = [&](const std::string& method, const json& params) {
    if (method != "Progress") return;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_sent < std::chrono::seconds(1)) return;
    last_sent = now;
    events_.Publish("library.install.progress", {{"source", "itch"},
                                                 {"ref", game_id},
                                                 {"progress", params.value("progress", 0.0)},
                                                 {"eta", params.value("eta", 0.0)},
                                                 {"bps", params.value("bps", 0.0)}});
  };
  if (auto performed = CallLong(config_, "Install.Perform", {{"id", install_id}, {"stagingFolder", staging_folder}},
                                on_notification);
      !performed) {
    return std::unexpected(performed.error());
  }

  ItchImporter importer(config_, games_, events_);
  if (auto imported = importer.Import(); !imported) return std::unexpected(imported.error());
  return {};
}

Result<void> ItchInstaller::Install(const std::string& game_id) { return Run(game_id); }
Result<void> ItchInstaller::Update(const std::string& game_id) { return Run(game_id); }

}  // namespace mira::itch
