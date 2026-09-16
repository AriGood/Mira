#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/Result.h"
#include "model/Types.h"

namespace mira::store {

// The games.toml-backed source of truth for the library. One in-memory
// vector guarded by one mutex, matching Config's approach: at the scale this
// targets (hundreds of games, a single user) that is simpler than anything
// else and no slower in practice. Every mutation is saved to disk before it
// returns, so games.toml is never more than one write behind memory.
class GameStore {
public:
  explicit GameStore(std::filesystem::path file);

  // Same never-fails contract as Config::Load: an unparseable file is kept as
  // <file>.bad and the library starts empty rather than the daemon refusing
  // to start.
  void Load();
  Result<void> Save();

  std::vector<model::Game> All() const;
  std::optional<model::Game> Find(const std::string& id) const;
  std::optional<model::Game> FindByInstallPath(const std::string& install_path) const;

  // Derives an id from the game's name, disambiguating against existing ids
  // ("celeste", "celeste-2", ...) so games.toml stays readable.
  std::string NextId(const std::string& name) const;

  // Inserts or replaces a game wholesale, then saves.
  Result<void> Upsert(model::Game game);

  // Applies `mutator` to the stored game under lock, saves, and returns the
  // updated copy. The common path for partial updates (PATCH, status
  // transitions) so read-modify-write is never split across two lock
  // acquisitions.
  Result<model::Game> Update(const std::string& id, std::function<void(model::Game&)> mutator);

  Result<void> Remove(const std::string& id);

private:
  mutable std::mutex mutex_;
  std::filesystem::path file_;
  std::vector<model::Game> games_;
};

}  // namespace mira::store
