#include "library/Relocate.h"

#include <algorithm>
#include <format>
#include <system_error>

#include "library/PrefixNaming.h"

namespace mira::library {
namespace {
namespace fs = std::filesystem;

// Same containment idiom api::DeleteUnderRoot uses for a delete target:
// weakly_canonical both sides, then a prefix match, so a symlink or a
// relative "../.." in a request body can't move a game's files outside a
// configured root.
bool Contained(const fs::path& target, const fs::path& root) {
  std::error_code ec;
  const fs::path resolved = fs::weakly_canonical(target, ec);
  if (ec) return false;
  const fs::path canon_root = fs::weakly_canonical(root, ec);
  if (ec) return false;
  const auto [root_end, nothing] = std::mismatch(canon_root.begin(), canon_root.end(), resolved.begin());
  return root_end == canon_root.end();
}

bool ContainedInAny(const fs::path& target, const std::vector<fs::path>& roots) {
  return std::ranges::any_of(roots, [&](const fs::path& root) { return Contained(target, root); });
}

Result<void> Move(const fs::path& from, const fs::path& to, bool allow_copy) {
  std::error_code ec;
  if (!fs::exists(from, ec)) return Err("source_missing", std::format("{} doesn't exist", from.string()));

  fs::create_directories(to.parent_path(), ec);
  fs::rename(from, to, ec);
  if (!ec) return {};
  if (ec != std::errc::cross_device_link) return Err("move_failed", ec.message());
  if (!allow_copy) return Err("cross_device", "target is on another filesystem and relocate.allow_copy is off");

  // rename() can't cross filesystems -- prefix_root and a game's own
  // install_path may be on different drives. Copy the whole tree, then
  // remove the source, same fallback any "move a directory" tool needs
  // once that's possible.
  fs::copy(from, to, fs::copy_options::recursive, ec);
  if (ec) return Err("move_failed", ec.message());
  fs::remove_all(from, ec);
  if (ec) return Err("cleanup_failed", ec.message());
  return {};
}

}  // namespace

Result<model::Game> Relocate(const config::Config& config, model::Game game, const RelocateRequest& request) {
  // Store installs stay where their tool expects them unless a path is given.
  const bool store_managed = game.source == "steam" || game.source == "epic" || game.source == "gog" ||
                             game.source == "itch";
  if (!game.install_path.empty() && (request.install_path || !store_managed)) {
    const std::vector<fs::path> library_roots = config.GetPathArray("library_roots");
    fs::path target;
    if (request.install_path) {
      target = *request.install_path;
    } else if (!config.GetString("relocate.install_root").empty()) {
      target = NamedDir(config, game, config.GetPath("relocate.install_root"));
    } else if (!library_roots.empty()) {
      target = NamedDir(config, game, library_roots.front());
    }
    if (target.empty()) return Err("no_library_roots", "no library_roots configured to relocate into");
    if (!ContainedInAny(target, library_roots)) {
      return Err("path_outside_root",
                std::format("\"{}\" is not inside a configured library root", target.string()));
    }
    if (fs::path(game.install_path) != target) {
      if (auto moved = Move(game.install_path, target, config.GetBool("relocate.allow_copy")); !moved) return std::unexpected(moved.error());
      game.install_path = target.string();
    }
  }

  if (!game.data_dir.empty()) {
    const fs::path prefix_root = config.GetPath("prefix_root");
    const fs::path target = request.data_dir ? *request.data_dir : PrefixDir(config, game);
    if (!Contained(target, prefix_root)) {
      return Err("path_outside_root", std::format("\"{}\" is not inside prefix_root", target.string()));
    }
    if (fs::path(game.data_dir) != target) {
      if (auto moved = Move(game.data_dir, target, config.GetBool("relocate.allow_copy")); !moved) return std::unexpected(moved.error());
      game.data_dir = target.string();
    }
  }

  return game;
}

}  // namespace mira::library
