#include "library/GamesFolder.h"

#include <filesystem>
#include <format>

#include <json.hpp>

#include "core/Paths.h"

namespace mira::library {
namespace fs = std::filesystem;
using nlohmann::json;

Result<void> SetGamesFolder(config::Config& config, const std::string& folder) {
  std::string base = folder;
  while (base.size() > 1 && base.back() == '/') base.pop_back();
  if (base.empty() || (base.front() != '/' && !base.starts_with('~'))) {
    return Err("invalid_path", std::format("\"{}\" is not an absolute path", folder));
  }

  std::error_code ec;
  fs::create_directories(paths::Expand(base), ec);
  if (ec) return Err("create_failed", std::format("could not create {}: {}", base, ec.message()));

  // The old first root is the one being replaced; any others stay.
  json roots = json::array({base});
  const json old_roots = config.Get("library_roots");
  if (old_roots.is_array()) {
    for (size_t i = 1; i < old_roots.size(); ++i) {
      if (old_roots[i] != base) roots.push_back(old_roots[i]);
    }
  }

  return config.Patch({{"library_roots", roots},
                       {"prefix_root", base + "/prefixes"},
                       {"gog", {{"install_root", base + "/GOG"}}},
                       {"itch", {{"install_root", base + "/itch"}}},
                       {"humble", {{"download_root", base + "/Humble Bundle"}}}});
}

}  // namespace mira::library
