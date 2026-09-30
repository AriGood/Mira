#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

#include <json.hpp>

#include "config/Config.h"

namespace mira::metadata {

// Which art slots each game has cached, as a game record's `art` field:
// {"cover": "<version>", "hero": ..., "logo": ..., "icon": ...}, holding only
// the slots with an image. A version changes whenever that slot's image does,
// so a client can keep its copy until then and skip asking for a missing one.
//
// Built from each game's metadata file, and re-read only when that file
// changes, so listing a library costs one stat per game.
class ArtIndex {
public:
  explicit ArtIndex(const config::Config& config) : config_(config) {}

  nlohmann::json For(const std::string& game_id);

private:
  struct Entry {
    std::filesystem::file_time_type stamp;
    nlohmann::json art;
  };

  const config::Config& config_;
  std::mutex mutex_;
  std::unordered_map<std::string, Entry> entries_;  // only games with a metadata file
};

}  // namespace mira::metadata
