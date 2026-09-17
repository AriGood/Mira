#include "desktop/DesktopEntries.h"

#include <format>
#include <set>
#include <fstream>

#include "core/Log.h"
#include "core/Paths.h"

namespace mira::desktop {
namespace {
namespace fs = std::filesystem;

constexpr std::string_view kPrefix = "mira-";
constexpr std::string_view kSuffix = ".desktop";

// Freedesktop values are newline-delimited, so a name containing one would
// inject arbitrary keys into the entry.
std::string Sanitize(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    if (c != '\n' && c != '\r') out += c;
  }
  return out;
}

bool IsLaunchable(const model::Game& game) {
  if (game.status != model::GameStatus::Ready) return false;
  // Steam already puts every game in your Steam library into the desktop
  // menu itself (via its own Linux integration) — a second, Mira-owned
  // entry for the same game is redundant clutter, not a missing feature,
  // regardless of steam.launch_mode. Sync() below removes any mira-<id>
  // entry that stops being "wanted", so this also cleans up an entry a
  // Steam game already had from before this exclusion existed.
  if (game.runner_ref.starts_with("steam:")) return false;
  return !game.exe_path.empty();
}

}  // namespace

DesktopEntries::DesktopEntries(config::Config& config) : config_(config) {}

fs::path DesktopEntries::EntryPath(const std::string& game_id) const {
  return config_.GetPath("desktop_entries.directory") /
         std::format("{}{}{}", kPrefix, game_id, kSuffix);
}

std::string DesktopEntries::Render(const model::Game& game) const {
  const std::string exec = config_.GetString("desktop_entries.exec_mode") == "frontend"
                               ? std::format("mira-gui --launch {}", game.id)
                               : std::format("mira launch {}", game.id);

  return std::format(
      "[Desktop Entry]\n"
      "Type=Application\n"
      "Name={}\n"
      "Comment=Launch {} with Mira\n"
      "Exec={}\n"
      "Icon=applications-games\n"
      "Categories={}\n"
      "Terminal=false\n"
      "X-Mira-Game-Id={}\n",
      Sanitize(game.name), Sanitize(game.name), exec,
      Sanitize(config_.GetString("desktop_entries.categories")), game.id);
}

Result<void> DesktopEntries::Sync(const std::vector<model::Game>& games) {
  const bool enabled = config_.GetBool("desktop_entries.enabled");
  const fs::path dir = config_.GetPath("desktop_entries.directory");

  std::error_code ec;
  if (enabled) {
    fs::create_directories(dir, ec);
    if (ec) return Err("desktop_dir_failed", ec.message());
  } else if (!fs::is_directory(dir, ec)) {
    return {};  // disabled and nothing was ever written
  }

  // Which entries should exist now. Disabled means none, which also cleans
  // up anything written while it was on.
  std::set<std::string> wanted;
  if (enabled) {
    for (const model::Game& game : games) {
      if (IsLaunchable(game)) wanted.insert(game.id);
    }
  }

  for (const model::Game& game : games) {
    if (!wanted.contains(game.id)) continue;
    const fs::path path = EntryPath(game.id);
    std::ofstream out(path);
    if (!out) {
      log::Warn("could not write desktop entry {}", path.string());
      continue;
    }
    out << Render(game);
  }

  // Remove ours that are no longer wanted — a game deleted, gone missing, or
  // now needing an install. Anything not named mira-<id>.desktop is left
  // strictly alone.
  for (const auto& entry : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
    const std::string name = entry.path().filename().string();
    if (!name.starts_with(kPrefix) || !name.ends_with(kSuffix)) continue;
    const std::string id = name.substr(kPrefix.size(), name.size() - kPrefix.size() - kSuffix.size());
    if (wanted.contains(id)) continue;
    fs::remove(entry.path(), ec);
  }
  return {};
}

}  // namespace mira::desktop
