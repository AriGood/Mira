#include "amazon/AmazonImporter.h"

#include <algorithm>
#include <format>
#include <fstream>

#include <json.hpp>

#include "amazon/Nile.h"
#include "core/Log.h"
#include "library/PrefixNaming.h"
#include "runner/RunnerRegistry.h"

namespace mira::amazon {
namespace {
namespace fs = std::filesystem;
using nlohmann::json;

std::string ForwardSlashes(std::string path) {
  std::ranges::replace(path, '\\', '/');
  return path;
}

}  // namespace

AmazonImporter::AmazonImporter(config::Config& config, store::GameStore& games, api::EventBus& events)
    : config_(config), games_(games), events_(events) {}

Result<AmazonImportSummary> AmazonImporter::Import() {
  AmazonImportSummary summary;
  if (!config_.GetBool("amazon.enabled")) return summary;
  const json installed = ReadNileFile("installed.json");
  if (!installed.is_array()) return summary;  // nothing installed yet
  const json library = ReadNileFile("library.json");

  for (const json& entry : installed) {
    const std::string product_id = entry.value("id", std::string());
    const fs::path path = entry.value("path", std::string());
    std::error_code ec;
    if (product_id.empty() || !fs::is_directory(path, ec)) continue;

    json owned = json::object();
    if (library.is_array()) {
      const auto it = std::ranges::find_if(library, [&](const json& item) {
        return item.contains("product") && item["product"].value("id", std::string()) == product_id;
      });
      if (it != library.end()) owned = *it;
    }
    const json product = owned.value("product", json::object());

    // fuel.json is what `nile launch` runs: Main.Command relative to the
    // install, plus Args and an optional working directory.
    std::ifstream fuel_file(path / "fuel.json");
    const json fuel = json::parse(fuel_file, nullptr, false, true);
    const json main = fuel.is_object() ? fuel.value("Main", json::object()) : json::object();
    const std::string command = ForwardSlashes(main.value("Command", std::string()));

    const std::string id = "amazon-" + product_id;
    const auto existing = games_.Find(id);
    model::Game game = existing.value_or(model::Game{});
    game.id = id;
    game.source = "amazon";
    game.source_ref = product_id;
    if (product.contains("title") && product["title"].is_string()) {
      game.name = product["title"].get<std::string>();
    } else if (game.name.empty()) {
      game.name = path.filename().string();
    }
    game.install_path = path.string();
    if (!command.empty()) game.exe_path = command;
    if (const json args = main.value("Args", json::array()); args.is_array() && game.args.empty()) {
      for (const json& arg : args) {
        if (arg.is_string()) game.args += (game.args.empty() ? "" : " ") + arg.get<std::string>();
      }
    }
    if (main.contains("WorkingSubdirOverride") && main["WorkingSubdirOverride"].is_string()) {
      game.working_dir = ForwardSlashes(main["WorkingSubdirOverride"].get<std::string>());
    }
    game.platform = model::Platform::Windows;
    const fs::path sdk = NileConfigDir() / "SDK" / "Amazon Games Services";
    game.env["FUEL_DIR"] = (sdk / "Legacy").string();
    game.env["AMAZON_GAMES_SDK_PATH"] = (sdk / "AmazonGamesSDK").string();
    game.env["AMAZON_GAMES_FUEL_ENTITLEMENT_ID"] = owned.value("id", std::string());
    game.env["AMAZON_GAMES_FUEL_PRODUCT_SKU"] = product.value("sku", std::string());
    game.runner_config["store"] = "amazon";
    if (std::ranges::find(game.tags, "amazon") == game.tags.end()) game.tags.push_back("amazon");
    game.last_error.clear();
    game.updated_at = model::NowSeconds();
    if (!existing) game.created_at = game.updated_at;

    if (game.exe_path.empty()) {
      game.status = model::GameStatus::Broken;
      game.last_error = "no fuel.json launch command in this install";
    } else if (!existing || existing->runner_ref.empty() || existing->data_dir.empty()) {
      if (game.data_dir.empty()) game.data_dir = library::PrefixDir(config_, game).string();
      const model::Game provisioned = runner::RunnerRegistry(config_).ProvisionGame(game);
      game.runner_ref = provisioned.runner_ref;
      game.data_dir = provisioned.data_dir;
      game.status = provisioned.status;
      game.last_error = provisioned.last_error;
    } else {
      game.status = model::GameStatus::Ready;
    }

    if (auto saved = games_.Upsert(game); !saved) {
      log::Error("failed to import amazon game {}: {}", product_id, saved.error().message);
      continue;
    }
    events_.Publish(existing ? "game.updated" : "game.added", model::ToJson(game));
    if (existing) {
      ++summary.updated;
    } else {
      ++summary.added;
      summary.added_games.push_back(game);
    }
  }
  return summary;
}

}  // namespace mira::amazon
