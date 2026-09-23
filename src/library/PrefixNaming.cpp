#include "library/PrefixNaming.h"

#include <format>
#include <system_error>

#include "core/Strings.h"

namespace mira::library {

std::filesystem::path NamedDir(const config::Config& config, const model::Game& game,
                               const std::filesystem::path& root) {
  namespace fs = std::filesystem;
  if (config.GetString("prefix_naming") != "name") return root / game.id;

  const std::string base = strings::Slugify(game.name);  // never empty -- Slugify's own "game" fallback

  std::error_code ec;
  fs::path candidate = root / base;
  for (int suffix = 2; fs::exists(candidate, ec); ++suffix) {
    candidate = root / std::format("{}-{}", base, suffix);
  }
  return candidate;
}

std::filesystem::path PrefixDir(const config::Config& config, const model::Game& game) {
  return NamedDir(config, game, config.GetPath("prefix_root"));
}

}  // namespace mira::library
