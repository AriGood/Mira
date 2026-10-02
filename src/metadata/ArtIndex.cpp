#include "metadata/ArtIndex.h"

#include "core/Json.h"

#include <array>
#include <cstdint>
#include <format>
#include <fstream>
#include <utility>

#include "metadata/MetadataFetcher.h"

namespace mira::metadata {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

// Metadata file key -> the slot's name everywhere else ("artwork" is the cover's legacy key).
constexpr std::array<std::pair<const char*, const char*>, 4> kSlots = {{
    {"artwork", "cover"},
    {"hero", "hero"},
    {"logo", "logo"},
    {"icon", "icon"},
}};

json ReadArt(const config::Config& config, const std::string& game_id, const fs::path& metadata_file) {
  json art = json::object();
  std::ifstream in(metadata_file);
  const json info = json::parse(in, nullptr, false);
  if (!info.is_object()) return art;
  for (const auto& [key, slot] : kSlots) {
    if (!info.contains(key) || !info[key].is_object()) continue;
    const fs::path file = ArtworkDir(config, game_id) / core::JsonString(info[key], "file");
    std::error_code ec;
    const auto written = fs::last_write_time(file, ec);
    if (ec) continue;
    const auto size = fs::file_size(file, ec);
    if (ec) continue;
    // A slot's file keeps its name when replaced, so its time and size stand in for its contents.
    art[slot] = std::format("{:x}-{:x}", static_cast<std::uint64_t>(written.time_since_epoch().count()), size);
  }
  return art;
}

}  // namespace

json ArtIndex::For(const std::string& game_id) {
  const fs::path metadata_file = MetadataFile(config_, game_id);
  std::error_code ec;
  const auto stamp = fs::last_write_time(metadata_file, ec);
  {
    std::lock_guard lock(mutex_);
    if (ec) {
      entries_.erase(game_id);
      return json::object();
    }
    if (const auto found = entries_.find(game_id); found != entries_.end() && found->second.stamp == stamp) {
      return found->second.art;
    }
  }
  json art = ReadArt(config_, game_id, metadata_file);  // file I/O stays outside the lock
  std::lock_guard lock(mutex_);
  entries_[game_id] = {stamp, art};
  return art;
}

}  // namespace mira::metadata
