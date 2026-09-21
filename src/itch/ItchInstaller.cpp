#include "itch/ItchInstaller.h"

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

  // Two-call sequence: Install.Queue (asks butlerd to pick an upload for
  // this game and stage it) then Install.Perform (actually fetches it
  // into stagingFolder). This is a simplification of the three-call
  // Queue/PlanUpload/Perform sequence itch's own docs describe -- the
  // exact parameters PlanUpload needs weren't confirmed against a real
  // account (see Butlerd.h's class comment), so it's left out rather than
  // called with guessed arguments; if it turns out to be required for a
  // given title, Perform below fails with a clear butlerd_error instead
  // of silently doing the wrong thing.
  const Result<json> queued = Call(config_, "Install.Queue", {{"game", {{"id", std::stoll(game_id)}}}});
  if (!queued) return std::unexpected(queued.error());

  const std::string install_id = queued->value("id", std::string());
  if (install_id.empty()) return Err("itch_queue_failed", "Install.Queue didn't return an install id");

  const std::filesystem::path staging = config_.File().parent_path() / "tools" / "itch" / "staging" / install_id;
  std::error_code ec;
  std::filesystem::create_directories(staging, ec);

  if (auto performed = Call(config_, "Install.Perform", {{"id", install_id}, {"stagingFolder", staging.string()}});
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
