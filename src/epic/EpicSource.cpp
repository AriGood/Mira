#include "epic/EpicSource.h"

#include <json.hpp>

#include "epic/EpicImporter.h"
#include "epic/EpicInstaller.h"
#include "epic/Legendary.h"

namespace mira::epic {
using nlohmann::json;

Result<std::vector<library::CatalogEntry>> EpicSource::Catalog(const config::Config& config,
                                                              const store::GameStore& games) const {
  std::vector<library::CatalogEntry> entries;
  if (!config.GetBool("epic.enabled")) return entries;

  const Result<json> listed = RunLegendaryJson(config, {"list"});
  if (!listed) return std::unexpected(listed.error());
  if (!listed->is_array()) return entries;

  for (const json& item : *listed) {
    library::CatalogEntry entry;
    entry.source = "epic";
    entry.ref = item.value("app_name", std::string());
    if (entry.ref.empty()) continue;
    // `list --json` calls this "app_title"; `list-installed --json` calls
    // the same thing "title" -- accept either.
    entry.title = item.value("app_title", item.value("title", std::string()));
    if (entry.title.empty()) entry.title = entry.ref;
    library::MarkTracked(games, entry);
    entries.push_back(std::move(entry));
  }
  return entries;
}

namespace {
// Shared precondition for Install/Update: never assume legendary is set up
// and authenticated, same as every direct /v1/epic/* route already checks.
Result<void> CheckReady(const config::Config& config) {
  const EpicAuthStatus auth = Status(config);
  if (!auth.legendary.installed) return Err("legendary_missing", "run \"mira epic setup\" first");
  if (!auth.authenticated) return Err("not_authenticated", "run \"mira epic login\" first");
  return {};
}
}  // namespace

Result<void> EpicSource::Install(config::Config& config, store::GameStore& games, api::EventBus& events,
                                const std::string& ref) {
  if (auto ready = CheckReady(config); !ready) return ready;
  EpicInstaller installer(config, games, events);
  return installer.Install(ref);
}

Result<void> EpicSource::Update(config::Config& config, store::GameStore& games, api::EventBus& events,
                               const std::string& ref) {
  if (auto ready = CheckReady(config); !ready) return ready;
  EpicInstaller installer(config, games, events);
  return installer.Update(ref);
}

}  // namespace mira::epic
