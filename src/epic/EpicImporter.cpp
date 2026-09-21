#include "epic/EpicImporter.h"

#include <json.hpp>

#include "core/Log.h"
#include "epic/Legendary.h"
#include "runner/RunnerRegistry.h"

namespace mira::epic {
namespace {
using nlohmann::json;

struct InstalledTitle {
  std::string app_name;
  std::string title;
  std::string install_path;
  std::string executable;
};

std::vector<InstalledTitle> ParseInstalled(const json& parsed) {
  std::vector<InstalledTitle> out;
  if (!parsed.is_array()) return out;
  for (const auto& entry : parsed) {
    InstalledTitle title;
    title.app_name = entry.value("app_name", std::string());
    if (title.app_name.empty()) continue;
    title.title = entry.value("title", std::string());
    title.install_path = entry.value("install_path", std::string());
    title.executable = entry.value("executable", std::string());
    out.push_back(std::move(title));
  }
  return out;
}

struct CatalogTitle {
  std::string app_name;
  std::string title;
};

std::vector<CatalogTitle> ParseCatalog(const json& parsed) {
  std::vector<CatalogTitle> out;
  if (!parsed.is_array()) return out;
  for (const auto& entry : parsed) {
    CatalogTitle title;
    title.app_name = entry.value("app_name", std::string());
    if (title.app_name.empty()) continue;
    // Legendary's `list --json` calls this field "app_title"; `list-installed`
    // calls the same concept "title" -- accept either.
    title.title = entry.value("app_title", entry.value("title", std::string()));
    out.push_back(std::move(title));
  }
  return out;
}

}  // namespace

EpicImporter::EpicImporter(config::Config& config, store::GameStore& games, api::EventBus& events)
    : config_(config), games_(games), events_(events) {}

Result<EpicImportSummary> EpicImporter::Import() {
  EpicImportSummary summary;
  if (!config_.GetBool("epic.enabled")) return summary;

  const Result<json> installed_json = RunLegendaryJson(config_, {"list-installed"});
  if (!installed_json) return std::unexpected(installed_json.error());

  const runner::RunnerRegistry provisioner(config_);

  for (const InstalledTitle& title : ParseInstalled(*installed_json)) {
    const std::string id = "epic-" + title.app_name;
    const auto existing = games_.Find(id);

    // Preserve anything the user already configured across a re-import —
    // only the fields Legendary itself owns get overwritten, same contract
    // as SteamScanner::Scan/LutrisImporter::Import.
    model::Game game = existing.value_or(model::Game{});
    game.id = id;
    game.source = "epic";
    game.source_ref = title.app_name;
    game.name = title.title.empty() ? title.app_name : title.title;
    game.install_path = title.install_path;
    game.exe_path = title.executable;
    game.platform = model::Platform::Windows;
    game.last_error.clear();
    game.updated_at = model::NowSeconds();
    if (!existing) game.created_at = game.updated_at;

    // Unlike Steam/Lutris, Legendary never creates a Wine prefix of its own
    // — an already-installed Epic title still needs Mira's own
    // RunnerRegistry to provision one before it's launchable (see this
    // class's header comment). Only on first sight, or if a previous
    // provisioning attempt never actually finished: re-provisioning a
    // working game on every re-import would be wasteful and pointless.
    if (!existing || existing->runner_ref.empty() || existing->data_dir.empty()) {
      const model::Game provisioned = provisioner.ProvisionGame(game);
      game.runner_ref = provisioned.runner_ref;
      game.data_dir = provisioned.data_dir;
      game.status = provisioned.status;
      game.last_error = provisioned.last_error;
    } else {
      game.status = model::GameStatus::Ready;
    }

    auto result = games_.Upsert(game);
    if (!result) {
      log::Error("failed to save epic game {}: {}", id, result.error().message);
      continue;
    }
    if (existing) {
      ++summary.updated;
      events_.Publish("game.updated", model::ToJson(game));
    } else {
      ++summary.added;
      summary.added_games.push_back(game);
      events_.Publish("game.added", model::ToJson(game));
    }
  }

  if (!config_.GetBool("epic.import_uninstalled")) return summary;

  const Result<json> catalog_json = RunLegendaryJson(config_, {"list"});
  if (!catalog_json) {
    log::Warn("epic catalog listing failed, only already-installed titles imported: {}",
             catalog_json.error().message);
    return summary;
  }

  for (const CatalogTitle& title : ParseCatalog(*catalog_json)) {
    const std::string id = "epic-" + title.app_name;
    if (games_.Find(id)) continue;  // already installed (handled above), or already a placeholder

    model::Game game;
    game.id = id;
    game.source = "epic";
    game.source_ref = title.app_name;
    game.name = title.title.empty() ? title.app_name : title.title;
    game.platform = model::Platform::Windows;
    game.status = model::GameStatus::NeedsInstall;
    game.last_error = "Not installed — run \"mira epic install " + id + "\".";
    game.updated_at = model::NowSeconds();
    game.created_at = game.updated_at;

    auto result = games_.Upsert(game);
    if (!result) {
      log::Error("failed to save epic catalog entry {}: {}", id, result.error().message);
      continue;
    }
    ++summary.added;
    summary.added_games.push_back(game);
    events_.Publish("game.added", model::ToJson(game));
  }

  return summary;
}

}  // namespace mira::epic
