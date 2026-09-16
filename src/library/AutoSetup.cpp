#include "library/AutoSetup.h"

#include "core/Log.h"
#include "core/Strings.h"

namespace mira::library {
namespace {
namespace fs = std::filesystem;
}

AutoSetup::AutoSetup(config::Config& config, store::GameStore& games, api::EventBus& events)
    : config_(config), games_(games), events_(events) {}

model::Game AutoSetup::CreateGame(const fs::path& install_path, const Detector::Result& detected) {
  model::Game game;
  game.name = strings::CleanGameName(install_path.filename().string());
  game.id = games_.NextId(game.name);
  game.install_path = install_path.string();
  game.confidence = detected.confidence;
  game.candidates = detected.candidates;
  game.created_at = model::NowSeconds();
  game.updated_at = game.created_at;
  game.data_dir = (config_.GetPath("prefix_root") / game.id).string();

  if (detected.candidates.empty()) {
    game.status = model::GameStatus::Broken;
    game.platform = model::Platform::Unknown;
    game.last_error = "No executable found automatically — set one manually.";
  } else {
    const model::Candidate& top = detected.candidates.front();
    game.platform = top.kind;
    game.exe_path = top.rel_path;
    // Native games need no provisioning; Windows games wait at setting_up
    // until the runner layer (not yet built) creates their prefix.
    game.status = top.kind == model::Platform::Native ? model::GameStatus::Ready
                                                       : model::GameStatus::SettingUp;
  }

  if (auto result = games_.Upsert(game); !result) {
    log::Error("failed to save new game \"{}\": {}", game.id, result.error().message);
  }

  nlohmann::json payload = model::ToJson(game);
  payload["open_config"] = config_.GetBool("open_config_on_add");
  events_.Publish("game.added", std::move(payload));

  return game;
}

}  // namespace mira::library
