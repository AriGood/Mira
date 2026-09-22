#include "gog/GogInstaller.h"

#include "gog/Gog.h"
#include "gog/GogImporter.h"

namespace mira::gog {
namespace {

Result<void> CheckReady(const config::Config& config) {
  const GogAuthStatus auth = Status(config);
  if (!auth.gogdl.installed) return Err("gogdl_missing", "run \"mira gog setup\" first");
  if (!auth.authenticated) return Err("not_authenticated", "run \"mira gog login\" first");
  return {};
}

}  // namespace

GogInstaller::GogInstaller(config::Config& config, store::GameStore& games, api::EventBus& events)
    : config_(config), games_(games), events_(events) {}

Result<void> GogInstaller::Run(const std::string& id) {
  if (auto ready = CheckReady(config_); !ready) return ready;

  const std::filesystem::path install_dir = config_.GetPath("gog.install_root") / id;
  if (auto output = RunGogdl(config_, {"download", id, "--path", install_dir.string(), "--platform", "windows"});
      !output) {
    return std::unexpected(output.error());
  }

  GogImporter importer(config_, games_, events_);
  if (auto imported = importer.ImportPath(id, install_dir); !imported) return std::unexpected(imported.error());
  return {};
}

Result<void> GogInstaller::Install(const std::string& id) { return Run(id); }
Result<void> GogInstaller::Update(const std::string& id) { return Run(id); }

}  // namespace mira::gog
