#include "itch/ItchSource.h"

#include <json.hpp>

#include "itch/Butlerd.h"
#include "itch/ItchInstaller.h"

namespace mira::itch {
using nlohmann::json;

Result<std::vector<library::CatalogEntry>> ItchSource::Catalog(const config::Config& config,
                                                               const store::GameStore& games) const {
  std::vector<library::CatalogEntry> entries;
  if (!config.GetBool("itch.enabled")) return entries;

  const Result<json> keys = Call(config, "Fetch.ProfileOwnedKeys", {{"fresh", true}});
  if (!keys) return std::unexpected(keys.error());

  const json items = keys->value("items", json::array());
  if (!items.is_array()) return entries;

  for (const json& key : items) {
    const json& game = key.value("game", json::object());
    const std::int64_t game_id = game.value("id", std::int64_t{0});
    if (game_id == 0) continue;

    library::CatalogEntry entry;
    entry.source = "itch";
    entry.ref = std::to_string(game_id);
    entry.title = game.value("title", std::string());
    if (entry.title.empty()) entry.title = entry.ref;
    library::MarkTracked(games, entry);
    entries.push_back(std::move(entry));
  }
  return entries;
}

Result<void> ItchSource::Install(config::Config& config, store::GameStore& games, api::EventBus& events,
                                const std::string& ref) {
  ItchInstaller installer(config, games, events);
  return installer.Install(ref);
}

Result<void> ItchSource::Update(config::Config& config, store::GameStore& games, api::EventBus& events,
                               const std::string& ref) {
  ItchInstaller installer(config, games, events);
  return installer.Update(ref);
}

}  // namespace mira::itch
