#include "epic/EpicInstaller.h"

#include "epic/EpicImporter.h"
#include "epic/Legendary.h"

namespace mira::epic {

EpicInstaller::EpicInstaller(config::Config& config, store::GameStore& games, api::EventBus& events)
    : config_(config), games_(games), events_(events) {}

Result<void> EpicInstaller::Run(const std::string& verb, const std::string& app_name) {
  if (auto output = RunLegendary(config_, {verb, app_name, "-y"}); !output) {
    return std::unexpected(output.error());
  }
  // Picks up the new/updated install and provisions a Wine/Proton prefix
  // for it. Also re-syncs the whole catalog as a side effect; harmless, and
  // not worth a narrower "import just this one title" path for what's
  // already a slow, human-triggered operation.
  EpicImporter importer(config_, games_, events_);
  if (auto imported = importer.Import(); !imported) return std::unexpected(imported.error());
  return {};
}

Result<void> EpicInstaller::Install(const std::string& app_name) { return Run("install", app_name); }
Result<void> EpicInstaller::Update(const std::string& app_name) { return Run("update", app_name); }

}  // namespace mira::epic
