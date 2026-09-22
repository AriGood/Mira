#include "library/Catalog.h"

#include "core/Log.h"
#include "library/SourceRegistry.h"

namespace mira::library {

void MarkTracked(const store::GameStore& games, CatalogEntry& entry) {
  const std::string id = entry.source + "-" + entry.ref;
  if (const auto tracked = games.Find(id)) {
    entry.installed = true;
    entry.game_id = id;
  }
}

Result<std::vector<CatalogEntry>> ListCatalog(const config::Config& config, const store::GameStore& games,
                                             const std::string& source) {
  if (!source.empty() && FindSource(source) == nullptr) {
    return Err("unknown_source", "no catalog source named \"" + source + "\"");
  }

  std::vector<CatalogEntry> entries;
  for (const auto& src : AllSources()) {
    if (!source.empty() && src->Name() != source) continue;
    Result<std::vector<CatalogEntry>> source_entries = src->Catalog(config, games);
    if (!source_entries) {
      // Unconfigured/unauthenticated is the normal case for most sources,
      // not an error worth failing the whole listing over -- one broken
      // storefront shouldn't hide the others.
      log::Warn("{} catalog unavailable: {}", src->Name(), source_entries.error().message);
      continue;
    }
    entries.insert(entries.end(), source_entries->begin(), source_entries->end());
  }
  return entries;
}

}  // namespace mira::library
