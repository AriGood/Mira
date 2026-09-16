#include "store/GameStore.h"

#include <algorithm>
#include <format>
#include <fstream>

#include <toml.hpp>

#include "core/Log.h"
#include "core/Strings.h"
#include "core/TomlJson.h"

namespace mira::store {
namespace {
using nlohmann::json;
}

GameStore::GameStore(std::filesystem::path file) : file_(std::move(file)) {}

void GameStore::Load() {
  std::lock_guard lock(mutex_);
  games_.clear();

  std::error_code ec;
  if (!std::filesystem::exists(file_, ec)) return;  // no games yet; not an error

  toml::parse_result parsed = toml::parse_file(file_.string());
  if (!parsed) {
    const auto broken = file_.string() + ".bad";
    std::filesystem::rename(file_, broken, ec);
    log::Error("games at {} could not be parsed ({}); kept it as {} and starting with an "
              "empty library",
              file_.string(), parsed.error().description(), broken);
    return;
  }

  const json whole = tomljson::ToJson(parsed.table());
  if (!whole.contains("game") || !whole["game"].is_array()) return;

  for (const json& entry : whole["game"]) {
    model::Game game = model::GameFromJson(entry);
    if (game.id.empty()) {
      log::Warn("skipping a game entry in {} with no id", file_.string());
      continue;
    }
    games_.push_back(std::move(game));
  }
  log::Info("loaded {} game(s) from {}", games_.size(), file_.string());
}

Result<void> GameStore::Save() {
  json whole = json::object();
  whole["game"] = json::array();
  for (const model::Game& game : games_) whole["game"].push_back(model::ToJson(game));

  std::error_code ec;
  std::filesystem::create_directories(file_.parent_path(), ec);

  const auto temp = file_.string() + ".tmp";
  {
    std::ofstream out(temp);
    if (!out) return Err("games_write_failed", std::format("cannot write {}", temp));
    out << tomljson::ToToml(whole);
  }
  std::filesystem::rename(temp, file_, ec);
  if (ec) return Err("games_write_failed", ec.message());
  return {};
}

std::vector<model::Game> GameStore::All() const {
  std::lock_guard lock(mutex_);
  return games_;
}

std::optional<model::Game> GameStore::Find(const std::string& id) const {
  std::lock_guard lock(mutex_);
  const auto it = std::ranges::find(games_, id, &model::Game::id);
  if (it == games_.end()) return std::nullopt;
  return *it;
}

std::optional<model::Game> GameStore::FindByInstallPath(const std::string& install_path) const {
  std::lock_guard lock(mutex_);
  const auto it = std::ranges::find(games_, install_path, &model::Game::install_path);
  if (it == games_.end()) return std::nullopt;
  return *it;
}

std::string GameStore::NextId(const std::string& name) const {
  const std::string base = strings::Slugify(name);
  std::lock_guard lock(mutex_);
  if (std::ranges::none_of(games_, [&](const model::Game& g) { return g.id == base; })) {
    return base;
  }
  for (int suffix = 2;; ++suffix) {
    const std::string candidate = std::format("{}-{}", base, suffix);
    if (std::ranges::none_of(games_, [&](const model::Game& g) { return g.id == candidate; })) {
      return candidate;
    }
  }
}

Result<void> GameStore::Upsert(model::Game game) {
  {
    std::lock_guard lock(mutex_);
    auto it = std::ranges::find(games_, game.id, &model::Game::id);
    if (it == games_.end()) {
      games_.push_back(std::move(game));
    } else {
      *it = std::move(game);
    }
  }
  return Save();
}

Result<model::Game> GameStore::Update(const std::string& id,
                                       std::function<void(model::Game&)> mutator) {
  model::Game updated;
  {
    std::lock_guard lock(mutex_);
    auto it = std::ranges::find(games_, id, &model::Game::id);
    if (it == games_.end()) return Err("game_not_found", std::format("no game with id \"{}\"", id));
    mutator(*it);
    it->updated_at = model::NowSeconds();
    updated = *it;
  }
  if (auto result = Save(); !result) return std::unexpected(result.error());
  return updated;
}

Result<void> GameStore::Remove(const std::string& id) {
  {
    std::lock_guard lock(mutex_);
    const auto [first, last] = std::ranges::remove(games_, id, &model::Game::id);
    if (first == games_.end()) {
      return Err("game_not_found", std::format("no game with id \"{}\"", id));
    }
    games_.erase(first, last);
  }
  return Save();
}

}  // namespace mira::store
