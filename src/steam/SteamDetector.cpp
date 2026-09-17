#include "steam/SteamDetector.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "core/Paths.h"
#include "core/Strings.h"
#include "steam/Vdf.h"

namespace mira::steam {
namespace {
namespace fs = std::filesystem;

// Fixed across every Steam installation — Valve's own redistributable
// bundler, not a game.
constexpr std::string_view kRedistAppId = "228980";

std::optional<std::string> ReadFile(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return std::nullopt;
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

// True if `install_dir` looks like a compatibility tool (Proton, Steam Linux
// Runtime) rather than a game: it ships its own "proton" launcher script or
// a runtime entry point at its top level. Structural, not name-matched, so
// it doesn't need updating as new Proton/runtime versions ship.
bool LooksLikeSteamTooling(const fs::path& install_dir) {
  std::error_code ec;
  if (fs::exists(install_dir / "proton", ec)) return true;
  if (fs::exists(install_dir / "_v2-entry-point", ec)) return true;
  return false;
}

}  // namespace

std::optional<fs::path> FindSteamRoot(const config::Config& config) {
  std::vector<fs::path> candidates;
  if (const fs::path configured = config.GetPath("steam.root"); !configured.empty()) {
    candidates.push_back(configured);
  }
  candidates.push_back(paths::Expand("~/.steam/steam"));
  candidates.push_back(paths::Expand("~/.local/share/Steam"));

  std::error_code ec;
  for (const fs::path& candidate : candidates) {
    if (fs::is_directory(candidate / "steamapps", ec)) return candidate;
  }
  return std::nullopt;
}

std::vector<fs::path> LibraryFolders(const fs::path& steam_root) {
  // steam_root is very commonly a symlink to one of the paths
  // libraryfolders.vdf itself lists (~/.steam/steam -> ~/.local/share/Steam
  // is the standard layout) — de-duplicated by what they resolve to on
  // disk, not by string equality, or every app in the default library gets
  // listed twice.
  std::error_code ec;
  std::vector<fs::path> folders = {steam_root};
  std::vector<fs::path> resolved = {fs::weakly_canonical(steam_root, ec)};

  const auto text = ReadFile(steam_root / "steamapps" / "libraryfolders.vdf");
  if (!text) return folders;
  const auto parsed = ParseVdf(*text);
  if (!parsed) return folders;

  const VdfValue* root = parsed->Get({"libraryfolders"});
  if (root == nullptr || !root->IsObject()) return folders;

  for (const auto& [key, entry] : root->children) {
    const VdfValue* path = entry.Get({"path"});
    if (path == nullptr || !path->scalar) continue;
    const fs::path candidate(*path->scalar);
    const fs::path canonical = fs::weakly_canonical(candidate, ec);
    if (!ec && std::ranges::find(resolved, canonical) != resolved.end()) continue;
    folders.push_back(candidate);
    resolved.push_back(canonical);
  }
  return folders;
}

std::optional<fs::path> ResolveProtonPath(const fs::path& compat_data_dir) {
  const auto text = ReadFile(compat_data_dir / "config_info");
  if (!text) return std::nullopt;

  // config_info is line-oriented, not VDF. Line 2 (0-indexed 1) is the
  // compat tool's own "files/share/fonts/" directory — three parent_path()
  // calls off of it (fonts -> share -> files -> the tool's own root, where
  // its "proton" script lives) is the same resolution Proton-adjacent tools
  // (protontricks and others) use, since Steam doesn't expose this any other
  // way short of parsing its own C++ source.
  const std::vector<std::string> lines = strings::Split(*text, '\n');
  if (lines.size() < 2) return std::nullopt;

  // A trailing '/' makes std::filesystem::path treat the last component as
  // an empty pseudo-element, so parent_path() needs one extra call to get
  // past it — stripped explicitly instead, so "fonts -> share -> files ->
  // tool root" is exactly three parent_path() calls, not a fragile four.
  std::string trimmed = strings::Trim(lines[1]);
  while (!trimmed.empty() && trimmed.back() == '/') trimmed.pop_back();
  fs::path fonts_dir(trimmed);
  if (fonts_dir.empty()) return std::nullopt;
  const fs::path tool_root = fonts_dir.parent_path().parent_path().parent_path();
  const fs::path proton = tool_root / "proton";

  std::error_code ec;
  if (!fs::exists(proton, ec)) return std::nullopt;
  return proton;
}

std::vector<SteamApp> ListApps(const fs::path& steam_root) {
  std::vector<SteamApp> apps;

  for (const fs::path& library : LibraryFolders(steam_root)) {
    const fs::path steamapps = library / "steamapps";
    std::error_code ec;
    if (!fs::is_directory(steamapps, ec)) continue;

    for (const auto& entry : fs::directory_iterator(steamapps, fs::directory_options::skip_permission_denied, ec)) {
      const std::string filename = entry.path().filename().string();
      if (!filename.starts_with("appmanifest_") || !filename.ends_with(".acf")) continue;

      const auto text = ReadFile(entry.path());
      if (!text) continue;
      const auto parsed = ParseVdf(*text);
      if (!parsed) continue;

      const VdfValue* state = parsed->Get({"appstate"});
      if (state == nullptr) continue;
      const VdfValue* appid = state->Get({"appid"});
      const VdfValue* name = state->Get({"name"});
      const VdfValue* installdir = state->Get({"installdir"});
      if (appid == nullptr || !appid->scalar || installdir == nullptr || !installdir->scalar) continue;
      if (*appid->scalar == kRedistAppId) continue;
      if (name && name->scalar && name->scalar->starts_with("Steam Linux Runtime")) continue;

      SteamApp app;
      app.appid = *appid->scalar;
      app.name = name && name->scalar ? *name->scalar : app.appid;
      app.install_dir = steamapps / "common" / *installdir->scalar;
      app.steam_root = steam_root;
      app.library_root = library;

      if (!fs::is_directory(app.install_dir, ec)) continue;  // manifest with no actual content on disk
      if (LooksLikeSteamTooling(app.install_dir)) continue;

      app.compat_data_dir = steamapps / "compatdata" / app.appid;
      app.is_native = !fs::is_directory(app.compat_data_dir, ec);
      if (!app.is_native) {
        if (auto proton = ResolveProtonPath(app.compat_data_dir)) app.proton_path = *proton;
      }

      apps.push_back(std::move(app));
    }
  }
  return apps;
}

}  // namespace mira::steam
