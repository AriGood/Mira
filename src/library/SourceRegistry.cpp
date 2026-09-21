#include "library/SourceRegistry.h"

#include "epic/EpicSource.h"
#include "steam/SteamSource.h"

namespace mira::library {
namespace {

std::vector<std::unique_ptr<ILibrarySource>> BuildSources() {
  std::vector<std::unique_ptr<ILibrarySource>> sources;
  sources.push_back(std::make_unique<epic::EpicSource>());
  sources.push_back(std::make_unique<steam::SteamSource>());
  return sources;
}

}  // namespace

const std::vector<std::unique_ptr<ILibrarySource>>& AllSources() {
  static const std::vector<std::unique_ptr<ILibrarySource>> sources = BuildSources();
  return sources;
}

ILibrarySource* FindSource(const std::string& name) {
  for (const auto& source : AllSources()) {
    if (source->Name() == name) return source.get();
  }
  return nullptr;
}

}  // namespace mira::library
