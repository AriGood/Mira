#include "desktop/DesktopEntryScanner.h"

#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "core/Log.h"
#include "core/Paths.h"

namespace mira::desktop {
namespace {
namespace fs = std::filesystem;

fs::path EnvOr(const char* name, const fs::path& fallback) {
  const char* value = std::getenv(name);
  return (value && *value) ? fs::path(value) : fallback;
}

std::vector<fs::path> SplitPathList(const std::string& value) {
  std::vector<fs::path> out;
  std::stringstream ss(value);
  std::string part;
  while (std::getline(ss, part, ':')) {
    if (!part.empty()) out.push_back(fs::path(part));
  }
  return out;
}

// Every applications/ dir a real .desktop file could live under, per the
// freedesktop base-dir spec, plus the two Flatpak export dirs -- Flatpak's
// own exports aren't reliably already inside XDG_DATA_DIRS depending on how
// the session was started (confirmed live: real Flatpak apps on this
// machine live under /var/lib/flatpak/exports/share/applications).
std::vector<fs::path> SearchDirs(const config::Config& config) {
  std::vector<fs::path> dirs;
  dirs.push_back(EnvOr("XDG_DATA_HOME", paths::Home() / ".local" / "share") / "applications");

  const char* data_dirs = std::getenv("XDG_DATA_DIRS");
  const std::vector<fs::path> base_dirs =
      (data_dirs && *data_dirs) ? SplitPathList(data_dirs)
                                : std::vector<fs::path>{"/usr/local/share", "/usr/share"};
  for (const fs::path& base : base_dirs) dirs.push_back(base / "applications");

  dirs.push_back("/var/lib/flatpak/exports/share/applications");
  dirs.push_back(EnvOr("XDG_DATA_HOME", paths::Home() / ".local" / "share") /
                  "flatpak" / "exports" / "share" / "applications");

  for (const std::string& extra : config.GetStringArray("desktop_import.extra_dirs")) {
    dirs.push_back(paths::Expand(extra));
  }

  // The same real directory can be reachable through more than one of the
  // entries above -- e.g. a user's own $XDG_DATA_DIRS already lists
  // /var/lib/flatpak/exports/share, which this function also adds by hand
  // as a fallback. Scanning it twice would offer every entry in it as two
  // identical candidates.
  std::vector<fs::path> unique;
  std::set<std::string> seen;
  std::error_code ec;
  for (const fs::path& dir : dirs) {
    const fs::path canonical = fs::weakly_canonical(dir, ec);
    const std::string key = ec ? dir.string() : canonical.string();
    if (seen.insert(key).second) unique.push_back(dir);
  }
  return unique;
}

// A small from-scratch [Desktop Entry] reader -- key=value lines, '#'
// comments, section headers, first key wins. Localized variants
// (key[locale]=) are read as their bare key would be if no bare key exists,
// but never overwrite an already-seen bare key.
struct DesktopFile {
  std::map<std::string, std::string> entries;
  bool has_desktop_entry_section = false;

  std::string Get(const std::string& key) const {
    const auto it = entries.find(key);
    return it == entries.end() ? std::string() : it->second;
  }
  bool Has(const std::string& key) const { return entries.contains(key); }
};

std::string Trim(std::string_view s) {
  size_t begin = s.find_first_not_of(" \t");
  if (begin == std::string_view::npos) return "";
  size_t end = s.find_last_not_of(" \t\r");
  return std::string(s.substr(begin, end - begin + 1));
}

std::optional<DesktopFile> ParseDesktopFile(const fs::path& path) {
  std::ifstream in(path);
  if (!in) return std::nullopt;

  DesktopFile file;
  std::string current_section;
  std::string line;
  while (std::getline(in, line)) {
    const std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#') continue;
    if (trimmed.front() == '[' && trimmed.back() == ']') {
      current_section = trimmed.substr(1, trimmed.size() - 2);
      if (current_section == "Desktop Entry") file.has_desktop_entry_section = true;
      continue;
    }
    if (current_section != "Desktop Entry") continue;

    const size_t eq = trimmed.find('=');
    if (eq == std::string::npos) continue;
    std::string key = Trim(trimmed.substr(0, eq));
    const std::string value = Trim(trimmed.substr(eq + 1));

    // Drop a localized variant's [locale] suffix -- keep only the bare key,
    // and never let a localized line overwrite an already-seen bare one.
    const size_t bracket = key.find('[');
    if (bracket != std::string::npos) key = key.substr(0, bracket);
    if (!file.entries.contains(key)) file.entries[key] = value;
  }
  return file;
}

bool IsFieldCodeOrForwardBracket(const std::string& token) {
  if (token.starts_with("@@")) return true;
  return token.size() == 2 && token[0] == '%';
}

// Minimal desktop-entry-spec-aware tokenizer: splits on whitespace, honoring
// one level of double-quoting (the one real quoted case confirmed live: a
// leading "/path/to/binary" token). Drops field codes and Flatpak-style
// @@...@@ forwarding brackets.
std::vector<std::string> TokenizeExec(const std::string& exec) {
  std::vector<std::string> tokens;
  std::string current;
  bool in_quotes = false;
  bool have_token = false;

  auto flush = [&] {
    if (have_token && !IsFieldCodeOrForwardBracket(current)) tokens.push_back(current);
    current.clear();
    have_token = false;
  };

  for (const char c : exec) {
    if (c == '"') {
      in_quotes = !in_quotes;
      have_token = true;
      continue;
    }
    if (c == ' ' && !in_quotes) {
      flush();
      continue;
    }
    current += c;
    have_token = true;
  }
  flush();
  return tokens;
}

// freedesktop "desktop file ID": path relative to its applications/ dir,
// '/' replaced with '-', .desktop stripped. Stable across calls with no
// stored state needed to connect a ListCandidates() id back to a file.
std::string DesktopFileId(const fs::path& dir, const fs::path& file) {
  // lexically_relative, not relative() -- relative() canonicalizes both
  // sides first, which resolves symlinks. Every real Flatpak-exported entry
  // is itself a symlink into a hashed deploy path, so relative() would
  // mangle the id into that path instead of the stable, readable name the
  // export directory presents. entry.path() is always dir-prefixed exactly
  // as walked, so the lexical form is already correct with no filesystem
  // access needed.
  const std::string id_path = file.lexically_relative(dir).string();
  std::string id = id_path;
  for (char& c : id) {
    if (c == '/') c = '-';
  }
  if (id.ends_with(".desktop")) id.resize(id.size() - std::string(".desktop").size());
  return id;
}

struct Resolved {
  std::string install_path;
  std::string exe_path;
  std::string args;
  model::Platform platform = model::Platform::Native;
};

std::optional<Resolved> ResolveCandidate(const DesktopFile& file) {
  if (const std::string app_id = file.Get("X-Flatpak"); !app_id.empty()) {
    // Ignore Exec= entirely -- Flatpak's own Exec line is a mess of
    // "flatpak run --branch=... --command=... <app-id> @@u %u @@" that isn't
    // worth parsing when the app id alone is enough to relaunch it exactly.
    Resolved resolved;
    resolved.install_path = (paths::Home() / ".var" / "app" / app_id).string();
    resolved.exe_path = "flatpak";
    resolved.args = "run " + app_id;
    resolved.platform = model::Platform::Native;
    return resolved;
  }

  const std::string exec = file.Get("Exec");
  if (exec.empty()) return std::nullopt;

  const std::vector<std::string> tokens = TokenizeExec(exec);
  if (tokens.empty()) return std::nullopt;

  const fs::path exe = tokens.front();
  Resolved resolved;
  if (exe.is_absolute()) {
    resolved.install_path = exe.parent_path().string();
    resolved.exe_path = exe.filename().string();
  } else {
    // A bare name with no '/' -- install_path stays empty, exe_path is the
    // name itself. NativeRunner's install_path / exe_path join and
    // execvpe's own PATH search already resolve this correctly with zero
    // new runner code.
    resolved.install_path.clear();
    resolved.exe_path = exe.string();
  }
  for (size_t i = 1; i < tokens.size(); ++i) {
    if (i > 1) resolved.args += ' ';
    resolved.args += tokens[i];
  }
  resolved.platform = model::Platform::Native;
  return resolved;
}

bool IsSteamStyle(const std::string& exec) {
  return exec.starts_with("steam ") || exec.find("steam://rungameid") != std::string::npos;
}

struct ScannedEntry {
  std::string id;
  DesktopFile file;
  Resolved resolved;
};

std::vector<ScannedEntry> ScanAll(const config::Config& config) {
  std::vector<ScannedEntry> out;
  std::error_code ec;

  for (const fs::path& dir : SearchDirs(config)) {
    if (!fs::is_directory(dir, ec)) continue;
    for (const auto& entry :
         fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
      if (ec || !entry.is_regular_file(ec) || entry.path().extension() != ".desktop") continue;

      const auto file = ParseDesktopFile(entry.path());
      if (!file || !file->has_desktop_entry_section) continue;
      if (const std::string type = file->Get("Type"); !type.empty() && type != "Application") continue;
      if (file->Get("NoDisplay") == "true" || file->Get("Hidden") == "true") continue;
      if (file->Has("X-Mira-Game-Id")) continue;  // our own entry -- never re-import it
      if (IsSteamStyle(file->Get("Exec"))) continue;  // SteamScanner already covers this, better

      const auto resolved = ResolveCandidate(*file);
      if (!resolved) continue;

      out.push_back(ScannedEntry{.id = DesktopFileId(dir, entry.path()), .file = *file, .resolved = *resolved});
    }
  }
  return out;
}

}  // namespace

DesktopEntryScanner::DesktopEntryScanner(config::Config& config, store::GameStore& games, api::EventBus& events)
    : config_(config), games_(games), events_(events) {}

Result<std::vector<DesktopEntryCandidate>> DesktopEntryScanner::ListCandidates() const {
  if (!config_.GetBool("desktop_import.enabled")) return std::vector<DesktopEntryCandidate>{};

  std::vector<DesktopEntryCandidate> candidates;
  for (const ScannedEntry& entry : ScanAll(config_)) {
    // No reason to list something already in the library as a candidate to
    // add again.
    if (games_.FindByInstallPath(entry.resolved.install_path)) continue;
    candidates.push_back(DesktopEntryCandidate{
        .id = entry.id,
        .name = entry.file.Get("Name"),
        .icon = entry.file.Get("Icon"),
    });
  }
  return candidates;
}

Result<DesktopEntryImportSummary> DesktopEntryScanner::Import(const std::vector<std::string>& ids) {
  if (!config_.GetBool("desktop_import.enabled")) {
    return Err("desktop_import_disabled", "desktop_import.enabled is off");
  }

  const std::set<std::string> wanted(ids.begin(), ids.end());
  DesktopEntryImportSummary summary;

  for (const ScannedEntry& entry : ScanAll(config_)) {
    if (!wanted.contains(entry.id)) continue;

    const auto existing = games_.FindByInstallPath(entry.resolved.install_path);
    model::Game game = existing.value_or(model::Game{});
    game.id = existing ? game.id : games_.NextId(entry.file.Get("Name"));
    game.source = "desktop-entry";
    game.name = entry.file.Get("Name");
    game.install_path = entry.resolved.install_path;
    game.exe_path = entry.resolved.exe_path;
    game.args = entry.resolved.args;
    game.platform = entry.resolved.platform;
    game.status = model::GameStatus::Ready;
    game.last_error.clear();
    game.updated_at = model::NowSeconds();
    if (!existing) game.created_at = game.updated_at;

    auto result = games_.Upsert(game);
    if (!result) {
      log::Error("failed to save desktop-entry game {}: {}", game.id, result.error().message);
      continue;
    }
    if (existing) {
      ++summary.updated;
      events_.Publish("game.updated", model::ToJson(game));
    } else {
      ++summary.added;
      summary.added_games.push_back(game);
      events_.Publish("game.added", model::ToJson(game));
    }
  }
  return summary;
}

}  // namespace mira::desktop
