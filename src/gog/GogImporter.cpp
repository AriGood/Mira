#include "gog/GogImporter.h"

#include <algorithm>
#include <fstream>

#include <json.hpp>

#include "core/Json.h"
#include "core/Log.h"
#include "gog/Gog.h"
#include "runner/RunnerRegistry.h"

namespace mira::gog {
namespace {
namespace fs = std::filesystem;
using nlohmann::json;

void AddTag(std::vector<std::string>& tags, const std::string& tag) {
  if (std::ranges::find(tags, tag) == tags.end()) tags.push_back(tag);
}

std::filesystem::path InstallRoot(const config::Config& config) { return config.GetPath("gog.install_root"); }

// gogdl's `download --path P` creates one subdirectory under P named for
// the game's title, not P itself (confirmed live: `download --path
// .../1207660413` put the game under `.../1207660413/Shadowrun Returns/`)
// -- `gogdl import` needs pointing at that inner directory or it crashes
// looking for a goggame-*.info file that isn't there. Falls back to `dir`
// itself if it doesn't look like that shape (zero or multiple
// subdirectories), rather than guessing wrong.
fs::path ResolveGameDir(const fs::path& dir) {
  std::error_code ec;
  fs::path only;
  int count = 0;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (!entry.is_directory()) continue;
    only = entry.path();
    if (++count > 1) break;
  }
  return count == 1 ? only : dir;
}

}  // namespace

GogImporter::GogImporter(config::Config& config, store::GameStore& games, api::EventBus& events)
    : config_(config), games_(games), events_(events) {}

Result<model::Game> GogImporter::ImportPath(const std::string& id, const std::filesystem::path& path) {
  const std::string game_id = "gog-" + id;
  const auto existing = games_.Find(game_id);
  const fs::path game_dir = ResolveGameDir(path);

  // gogdl's own `import <path>` output only confirms the install and
  // (best-effort) names/points at it -- everything Mira itself owns (id,
  // source, tags, prefix) is set here regardless of what that call
  // returns.
  // gogdl import's real field names for the title/executable aren't
  // confirmed yet (its own output crashed until game_dir pointed at the
  // right directory) -- title only for now, exe_path stays whatever it
  // already was (empty on a fresh install, needing a manual `mira set
  // --exe`, same as any detected-but-unconfirmed game).
  std::string title;
  if (const Result<std::string> imported = RunGogdl(config_, {"import", game_dir.string()}); imported) {
    const json parsed = core::ParseJsonTail(*imported);
    if (!parsed.is_discarded() && parsed.is_object()) title = parsed.value("title", std::string());
  } else {
    log::Warn("gogdl import at {} failed: {}", game_dir.string(), imported.error().message);
  }

  model::Game game = existing.value_or(model::Game{});
  game.id = game_id;
  game.source = "gog";
  game.source_ref = id;
  game.name = title.empty() ? id : title;
  game.install_path = game_dir.string();
  game.platform = model::Platform::Windows;
  game.last_error.clear();
  game.updated_at = model::NowSeconds();
  if (!existing) game.created_at = game.updated_at;
  AddTag(game.tags, "gog");

  if (!existing || existing->runner_ref.empty() || existing->data_dir.empty()) {
    const runner::RunnerRegistry provisioner(config_);
    game.data_dir = (config_.GetPath("prefix_root") / game.id).string();
    const model::Game provisioned = provisioner.ProvisionGame(game);
    game.runner_ref = provisioned.runner_ref;
    game.data_dir = provisioned.data_dir;
    game.status = provisioned.status;
    game.last_error = provisioned.last_error;
  } else {
    game.status = model::GameStatus::Ready;
  }

  auto result = games_.Upsert(game);
  if (!result) return std::unexpected(result.error());

  events_.Publish(existing ? "game.updated" : "game.added", model::ToJson(game));
  return game;
}

Result<GogImportSummary> GogImporter::Import() {
  GogImportSummary summary;
  if (!config_.GetBool("gog.enabled")) return summary;

  const fs::path root = InstallRoot(config_);
  std::error_code ec;
  if (!fs::is_directory(root, ec)) return summary;  // nothing installed yet is not an error

  for (const auto& entry : fs::directory_iterator(root, ec)) {
    if (!entry.is_directory()) continue;
    const std::string id = entry.path().filename().string();
    const bool existed = games_.Find("gog-" + id).has_value();

    const Result<model::Game> imported = ImportPath(id, entry.path());
    if (!imported) {
      log::Error("failed to import gog game at {}: {}", entry.path().string(), imported.error().message);
      continue;
    }
    if (existed) {
      ++summary.updated;
    } else {
      ++summary.added;
      summary.added_games.push_back(*imported);
    }
  }
  return summary;
}

}  // namespace mira::gog
