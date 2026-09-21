#include "config/Schema.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <map>
#include <string_view>

#include "config/KnownExePatterns.h"
#include "config/RunnerSources.h"
#include "core/Strings.h"

namespace mira::config {
namespace {

using nlohmann::json;

Constraint Range(double min, double max) {
  Constraint constraint;
  constraint.minimum = min;
  constraint.maximum = max;
  constraint.validate = [min, max](const json& value) -> std::optional<std::string> {
    if (!value.is_number()) return "expected a number";
    const double number = value.get<double>();
    if (number < min || number > max) {
      return std::format("must be between {} and {}", min, max);
    }
    return std::nullopt;
  };
  return constraint;
}

Constraint OneOf(std::vector<std::string> allowed) {
  Constraint constraint;
  constraint.one_of = allowed;
  constraint.validate = [allowed = std::move(allowed)](const json& value) -> std::optional<std::string> {
    if (!value.is_string()) return "expected a string";
    const std::string text = value.get<std::string>();
    if (std::ranges::find(allowed, text) == allowed.end()) {
      std::string list;
      for (const std::string& option : allowed) {
        if (!list.empty()) list += ", ";
        list += option;
      }
      return std::format("must be one of: {}", list);
    }
    return std::nullopt;
  };
  return constraint;
}

// A runner reference is "kind:name", where name may be "latest".
Validator RunnerRef() {
  return [](const json& value) -> std::optional<std::string> {
    if (!value.is_string()) return "expected a string";
    const std::string text = value.get<std::string>();
    if (text.empty()) return "must not be empty";
    if (text.find(':') == std::string::npos) return "must be \"kind:name\", e.g. \"wine:system\"";
    return std::nullopt;
  };
}

Validator NonEmptyString() {
  return [](const json& value) -> std::optional<std::string> {
    if (!value.is_string()) return "expected a string";
    if (value.get<std::string>().empty()) return "must not be empty";
    return std::nullopt;
  };
}

// UI grouping for a settings screen. A handful of keys are named explicitly
// because their dotted prefix alone would land them somewhere confusing (a
// runner reference split from "Runners", the artwork/store-info feature
// split across a "Metadata" and a "Steamgriddb" group); everything else
// falls back to a guess from its own dotted prefix, or "General" with none.
// This used to live in the frontend (SettingsCategories.cpp) with no way for
// the schema itself to say which group a key belonged in — moved here so
// GET /v1/config/schema can just say it.
std::string CategoryFor(std::string_view key) {
  static const std::map<std::string_view, std::string_view> kOverrides = {
      {"library_roots", "Library"},        {"prefix_root", "Library"},
      {"prefix_provider", "Library"},      {"prefix_template", "Library"},
      {"auto_setup", "Library"},           {"open_config_on_add", "Library"},
      {"command_wrappers", "Library"},     {"default_runner.windows", "Runners"},
      {"default_runner.native", "Runners"}, {"runner_search_paths", "Runners"},
      {"wine_search_paths", "Runners"},    {"socket_path", "Advanced"},
      {"log.level", "Advanced"},           {"events.sse_keepalive_s", "Advanced"},
      {"library.remove_missing", "Library"},
      {"metadata.enabled", "Metadata"},
      {"steamgriddb.api_key", "Metadata"},
  };
  if (const auto it = kOverrides.find(key); it != kOverrides.end()) return std::string(it->second);

  const size_t dot = key.find('.');
  if (dot == std::string_view::npos) return "General";
  const std::string_view prefix = key.substr(0, dot);
  if (prefix == "detect") return "Detection";
  if (prefix == "scan") return "Scanning";
  if (prefix == "default_runner") return "Runners";
  if (prefix == "log" || prefix == "events") return "Advanced";
  if (prefix == "launch") return "Launching";
  if (prefix == "desktop_entries") return "Desktop Entries";
  if (prefix == "library") return "Library";

  std::string label(prefix);
  std::ranges::replace(label, '_', ' ');
  if (!label.empty()) label.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(label.front())));
  return label;
}

}  // namespace

Schema::Schema() {
  entries_ = {
      {"socket_path", Type::String, "$XDG_RUNTIME_DIR/mira/mirad.sock", Tier::Expert,
       "Unix socket the daemon listens on. The frontend and CLI must agree with this.",
       NonEmptyString()},

      {"log.level", Type::String, "info", Tier::Advanced,
       "Logging verbosity: debug, info, warn or error.",
       OneOf({"debug", "info", "warn", "error"})},

      {"library_roots", Type::StringArray, json::array({"~/Games"}), Tier::Basic,
       "Folders watched for new games. Dropping a game folder into one of these is all "
       "that is required to add it."},

      {"prefix_root", Type::String, "~/Games/prefixes", Tier::Basic,
       "Where per-game data directories (Wine/Proton prefixes) are created. Always "
       "excluded from scanning, wherever it points."},

      {"auto_setup", Type::Bool, true, Tier::Basic,
       "Configure and provision newly detected games automatically. With this off, games "
       "are detected but wait for the frontend to configure them."},

      {"open_config_on_add", Type::Bool, true, Tier::Basic,
       "Ask the frontend to open its configuration menu when a game is added, so the "
       "auto-detected settings can be reviewed."},

      {"scan.tag_by_root", Type::Bool, true, Tier::Basic,
       "Automatically tag each newly-detected game with the name of the library root folder "
       "it was found in (e.g. a game under ~/Games Mira gets tagged \"Games Mira\") -- useful "
       "for filtering multiple game folders (GET /v1/games?tag=...) with several "
       "library_roots configured. Only applied at detection time, not retroactively."},

      {"library.remove_missing", Type::Bool, false, Tier::Basic,
       "When a previously-detected game's folder disappears, forget it entirely instead of "
       "just marking it missing. Off by default: missing keeps the game's configuration, "
       "overrides, and playtime around in case a drive or network share is just temporarily "
       "offline. Never touches the game's files themselves either way — same as "
       "DELETE /v1/games/{id}."},

      {"default_runner.windows", Type::String, "auto", Tier::Basic,
       "Runner for Windows games as \"kind:name\" (e.g. \"proton:GE-Proton11-7\", "
       "\"wine:system\"; \"latest\" as the name picks the newest installed build), or "
       "\"auto\" to pick the best installed runner — Proton if any build is present, "
       "otherwise Wine."},

      {"default_runner.native", Type::String, "native:native", Tier::Expert,
       "Runner for native Linux games.", RunnerRef()},

      {"runner_search_paths", Type::StringArray,
       json::array({"~/.steam/steam/compatibilitytools.d",
                    "~/.local/share/Steam/compatibilitytools.d",
                    "~/.local/share/mira/runners"}),
       Tier::Advanced, "Directories scanned for installed Proton builds."},

      {"wine_search_paths", Type::StringArray,
       json::array({"~/.local/share/lutris/runners/wine"}),
       Tier::Advanced,
       "Directories scanned for extra Wine builds (each a directory containing bin/wine), "
       "alongside the system wine on PATH."},

      {"prefix_provider", Type::String, "plain", Tier::Advanced,
       "How a new prefix directory is created. \"plain\" lets the runner initialise it; "
       "\"template\" clones prefix_template, which is far faster on btrfs/xfs.",
       OneOf({"plain", "template"})},

      {"prefix_template", Type::String, "", Tier::Advanced,
       "An already-initialised prefix to clone when prefix_provider is \"template\". "
       "Cloned with reflinks where the filesystem supports them."},

      {"command_wrappers", Type::StringArray, json::array(), Tier::Basic,
       "Wrappers applied to the launch command in order, e.g. [\"gamemoderun\", \"gamescope -W "
       "1920 -H 1080\"]. The first entry ends up outermost. Each entry is split on spaces (like "
       "a game's own args -- no shell quoting support), and receives the game's command line as "
       "its arguments. A wrapper whose own binary isn't on PATH fails the launch with a clear "
       "error instead of a mysterious \"exited with code 127\"."},

      {"launch.stop_timeout_s", Type::Int, 10, Tier::Advanced,
       "How long to give a game to quit after \"stop\" before it's killed outright. The "
       "whole process group is signalled, since a real launch is umu -> proton -> wine -> "
       "the game.",
       Range(0, 600)},

      {"launch.log_max_mb", Type::Int, 64, Tier::Advanced,
       "Cap on a game's own log file (see GET /v1/games/{id}/log), applied when a new session "
       "rotates the previous one out -- an oversized previous log is dropped instead of kept, "
       "so this bounds disk use to roughly 2x this value per game.",
       Range(1, 1024)},

      {"launch.gamemode", Type::Bool, false, Tier::Basic,
       "Register this game with Feral Interactive's GameMode daemon automatically -- no "
       "command_wrappers entry needed. A no-op if the daemon isn't installed or isn't running; "
       "see GET /v1/gamemode/status."},

      {"launch.env", Type::StringArray, json::array(), Tier::Basic,
       "KEY=VALUE environment variables set for every launch, e.g. [\"MANGOHUD=1\", "
       "\"DXVK_ASYNC=1\"]. A game's own env (per-game overrides) always wins over these. Not "
       "applied to a Steam game launched via steam.launch_mode \"steam\" -- Mira only hands "
       "off a steam:// URL there, it never builds the game's own command line."},

      {"launch.pre_script", Type::String, "", Tier::Advanced,
       "Shell command run (via sh -c) before POST /v1/games/{id}/launch actually starts the "
       "game -- e.g. mounting a network drive a game needs, setting a CPU governor. Runs "
       "synchronously; a non-zero exit aborts the launch with the script's own output as the "
       "error. Empty disables it. Overridable per game (PATCH .../config)."},

      {"launch.pre_timeout_s", Type::Int, 30, Tier::Advanced,
       "How long launch.pre_script is given to finish before the launch is aborted outright, "
       "to keep a hung script from wedging a launch forever.",
       Range(1, 600)},

      {"launch.post_script", Type::String, "", Tier::Advanced,
       "Shell command run once the game process exits (any reason: clean exit, crash, or "
       "stop), the mirror of launch.pre_script -- e.g. reverting a CPU governor change. Runs "
       "in the background; its own exit code is only logged, never affects the recorded "
       "playtime/crash state. Not run for a Steam game launched via steam.launch_mode "
       "\"steam\" unless steam.track_process is also on -- Mira otherwise never owns that "
       "process at all (see docs/api.md's Steam section)."},

      {"launch.post_timeout_s", Type::Int, 30, Tier::Advanced,
       "How long launch.post_script is given to finish before it's killed outright, the "
       "mirror of launch.pre_timeout_s.",
       Range(1, 600)},

      {"desktop_entries.enabled", Type::Bool, true, Tier::Basic,
       "Add each ready game to your application menu as a .desktop entry, so it can be "
       "launched from the desktop like any other app. Turn this off to keep Mira's games "
       "out of your menu entirely."},

      {"desktop_entries.directory", Type::String, "~/.local/share/applications", Tier::Expert,
       "Where .desktop entries are written. Only files Mira created (mira-<id>.desktop) are "
       "ever touched."},

      {"desktop_entries.categories", Type::String, "Game;", Tier::Advanced,
       "Freedesktop Categories= value for generated entries, deciding where they appear in "
       "the menu."},

      {"desktop_entries.exec_mode", Type::String, "cli", Tier::Advanced,
       "What a menu entry runs. \"cli\" (`mira launch <id>`) requires mirad to already be "
       "running and fails clearly if it isn't; \"frontend\" starts the frontend, which "
       "brings the daemon up itself. Either way the launch goes through Mira, which is "
       "what makes playtime get recorded — a menu entry that ran the game directly would "
       "launch fine and log nothing.",
       OneOf({"cli", "frontend"})},

      {"scan.debounce_ms", Type::Int, 3000, Tier::Advanced,
       "How long a new folder must stop changing before it is scanned. Raise it if games "
       "arrive over a slow network share.",
       Range(0, 600000)},

      {"scan.max_depth", Type::Int, 4, Tier::Advanced,
       "How deep to search inside a game folder for executables.", Range(1, 16)},

      {"scan.auto_extract_archives", Type::Bool, false, Tier::Basic,
       "Extract a .zip/.rar/.tar(.gz/.xz/.bz2)/.7z dropped directly into a library root, "
       "into a same-named folder, then delete the archive — so an archived game drop "
       "behaves like an already-extracted one. Off by default: silently deleting an "
       "archive is a real action to opt into, not assume. Extracting a .rar or .7z needs "
       "unrar/p7zip installed; a missing tool is reported, not silently skipped."},

      {"scan.periodic_interval_s", Type::Int, 0, Tier::Advanced,
       "Seconds between full rescans. 0 disables them, which is the default: inotify is "
       "authoritative and a timer would cost idle wakeups for nothing.",
       Range(0, 86400)},

      {"scan.ignore_globs", Type::StringArray,
       json::array({".*", "*/Redist*", "*/DirectX*", "*/_CommonRedist*", "*/DotNet*"}),
       Tier::Advanced, "Paths matching these globs are never treated as games."},

      {"detect.rules", Type::StringArray,
       json::array({"deny_patterns", "name_similarity", "depth", "shallowest"}),
       Tier::Expert,
       "Scoring rules applied to candidate executables, in order. Removing a rule "
       "disables it."},

      {"detect.name_match_bonus", Type::Double, 3.0, Tier::Expert,
       "Score added when an executable's name resembles its folder's name.",
       Range(0.0, 100.0)},

      {"detect.depth_penalty", Type::Double, 0.5, Tier::Expert,
       "Score subtracted per directory level, favouring executables near the top.",
       Range(0.0, 100.0)},

      {"detect.low_confidence_threshold", Type::Double, 0.5, Tier::Advanced,
       "Below this confidence a game is flagged for review. It is still configured and "
       "still launchable; the frontend just highlights it.",
       Range(0.0, 1.0)},

      {"detect.deny_name_patterns", Type::StringArray, json(known_exe_patterns::kDeny),
       Tier::Advanced,
       "Executables matching these globs are heavily penalised: they are installers and "
       "helpers rather than games. Defaults are curated in "
       "src/config/KnownExePatterns.h — edit that file to add one, no need to touch this "
       "schema or recompile just to override it for yourself here."},

      {"detect.installer_name_patterns", Type::StringArray, json(known_exe_patterns::kInstaller),
       Tier::Advanced,
       "A candidate matching one of these globs, and meeting detect.installer_min_size_mb "
       "(itself, or sharing a folder with a file that does — installers are often a small "
       "stub exe next to a much larger separate payload), is flagged as an installer "
       "rather than the game itself: stored with status needs_install instead of being "
       "auto-provisioned as launchable."},

      {"detect.installer_min_size_mb", Type::Int, 50, Tier::Advanced,
       "Minimum size — of the candidate itself, or of any file alongside it — for a "
       "name-matched candidate to actually count as an installer, so a small stub or "
       "helper named like one doesn't get misflagged.", Range(0, 1'000'000)},

      {"steam.enabled", Type::Bool, true, Tier::Basic,
       "Detect installed Steam games and let Mira launch them alongside its own library."},

      {"steam.root", Type::String, "", Tier::Advanced,
       "Override for Steam's install directory. Empty auto-detects "
       "~/.steam/steam, then ~/.local/share/Steam."},

      {"steam.launch_mode", Type::String, "steam", Tier::Basic,
       "How launching a Steam game works. \"steam\" fires "
       "steam://rungameid/<appid> and lets the Steam client launch it — full "
       "achievements/overlay support; Mira didn't spawn the process, so it "
       "falls back to steam.track_process (below) rather than normal exit-"
       "code/signal tracking. \"direct\" has Mira exec the game itself, "
       "through the same Proton build and prefix Steam already set up, with "
       "normal Mira process tracking (stop/crash/playtime) — override per "
       "game via games.toml overrides if one game needs the other mode.",
       OneOf({"steam", "direct"})},

      {"steam.track_process", Type::Bool, true, Tier::Advanced,
       "For steam.launch_mode \"steam\": poll /proc for the actual game "
       "process (matched by the SteamAppId/SteamGameId environment variable "
       "Steam itself sets — the same one the Steamworks API reads) so Mira "
       "still shows the game as running and records playtime, even though "
       "it didn't spawn the process. No crash/exit-code detection either "
       "way — that's Steam's own client's job. Off disables the /proc scan "
       "entirely; launching still works, Mira just won't show it running."},

      {"steam.web_api_key", Type::String, "", Tier::Basic,
       "Free API key from steamcommunity.com/dev/apikey. Steam's on-disk files only "
       "describe games that are actually installed, so listing everything the account "
       "owns (GET /v1/library) and importing Steam's own playtime totals both need this "
       "plus steam.steamid64. Left empty, Steam simply contributes nothing to the "
       "library listing and playtime stays whatever Mira itself measured."},

      {"steam.steamid64", Type::String, "", Tier::Basic,
       "The account's 64-bit Steam ID (steamcommunity.com profile URL, or a lookup "
       "site). Needed alongside steam.web_api_key — Steam's Web API identifies the "
       "account by this, not by the key."},

      {"steam.import_playtime", Type::Bool, true, Tier::Advanced,
       "On a Steam scan, adopt Steam's own playtime_forever total for a game when it's "
       "higher than what Mira recorded itself. Steam counts time played on any machine "
       "and long before Mira existed, so it's usually the larger and more complete "
       "number; the max of the two is kept so a Mira-tracked session is never lost to a "
       "stale Steam total. Needs steam.web_api_key + steam.steamid64."},

      {"lutris.enabled", Type::Bool, true, Tier::Basic,
       "Let \"mira lutris import\" / POST /v1/lutris/import read Lutris's own "
       "game database (pga.db) and per-game configs and add them alongside "
       "Mira's own library."},

      {"lutris.data_dir", Type::String, "", Tier::Advanced,
       "Override for where Lutris keeps pga.db and its per-game configs. "
       "Empty auto-detects $XDG_DATA_HOME/lutris, then ~/.local/share/lutris."},

      {"epic.enabled", Type::Bool, true, Tier::Basic,
       "Use Legendary (a native Epic Games Store CLI client) to authenticate, "
       "import already-installed Epic titles, and install/update new ones. The "
       "installed game itself still runs through Mira's own Wine/Proton runner, "
       "never through Legendary or Epic's own client. See \"mira epic setup\" "
       "if Legendary isn't already installed."},

      {"epic.legendary_bin", Type::String, "", Tier::Advanced,
       "Path to the legendary binary. Empty tries Mira's own managed download "
       "(see \"mira epic setup\"), then $PATH."},

      {"gog.enabled", Type::Bool, true, Tier::Basic,
       "Use gogdl (Heroic's GOG downloader) to authenticate, import "
       "already-installed GOG titles, and install/update new ones. The "
       "installed game itself still runs through Mira's own Wine/Proton "
       "runner (or natively, for the titles GOG ships a Linux build of), "
       "never through gogdl or GOG Galaxy. See \"mira gog setup\" if gogdl "
       "isn't already installed."},

      {"gog.gogdl_bin", Type::String, "", Tier::Advanced,
       "Path to the gogdl binary. Empty tries Mira's own managed download "
       "(see \"mira gog setup\"), then $PATH."},

      {"gog.install_root", Type::String, "~/Games/GOG", Tier::Advanced,
       "Where GOG titles are installed to, one subdirectory per game id — "
       "unlike Legendary/butler, gogdl doesn't choose or remember an "
       "install location on its own, so Mira has to."},

      {"itch.enabled", Type::Bool, true, Tier::Basic,
       "Use butler (itch.io's own launcher-integration daemon) to "
       "authenticate, import already-installed itch.io titles, and "
       "install/update new ones. The installed game itself still runs "
       "through Mira's own Wine/Proton runner (or natively, for the many "
       "itch.io titles that ship a Linux build), never through butler or "
       "the itch app. See \"mira itch setup\" if butler isn't already "
       "installed."},

      {"itch.butler_bin", Type::String, "", Tier::Advanced,
       "Path to the butler binary. Empty tries Mira's own managed download "
       "(see \"mira itch setup\"), then $PATH."},

      {"humble.enabled", Type::Bool, true, Tier::Basic,
       "Use humble-cli (an unofficial Humble Bundle CLI) to list purchased "
       "bundles and download items from them. Humble Bundle has no "
       "\"installed game\" concept of its own — a downloaded item is a "
       "plain file (installer, archive, or DRM-free build), added as a "
       "game manually afterward like any other manually-acquired title."},

      {"humble.humble_cli_bin", Type::String, "", Tier::Advanced,
       "Path to the humble-cli binary. Empty tries Mira's own managed "
       "download (see \"mira humble setup\"), then $PATH."},

      {"humble.download_root", Type::String, "~/Downloads/HumbleBundle", Tier::Advanced,
       "Where downloaded bundle items land, one subdirectory per bundle key."},

      {"desktop_import.enabled", Type::Bool, true, Tier::Basic,
       "Let \"mira desktop-entries list\"/\"import\" and the matching REST "
       "endpoints read already-installed application-menu (.desktop) entries "
       "and add them as games — this is how a Flatpak app gets added, since "
       "every Flatpak-exported entry already carries the app id needed to "
       "relaunch it. Manual: nothing is added until you pick which ones."},

      {"desktop_import.extra_dirs", Type::StringArray, json::array(), Tier::Advanced,
       "Extra directories to search for .desktop files, beyond the standard "
       "$XDG_DATA_HOME/applications, $XDG_DATA_DIRS entries, and the two "
       "well-known Flatpak export directories."},

      {"runner_sources.proton_ge.repo", Type::String, std::string(runner_sources::kProtonGERepo),
       Tier::Advanced, "GitHub \"owner/repo\" Proton-GE builds are downloaded from."},

      {"runner_sources.proton_ge.asset_pattern", Type::String,
       std::string(runner_sources::kProtonGEAssetPattern), Tier::Expert,
       "Glob a release's assets are filtered to before offering one to download — "
       "excludes non-x86_64 builds and checksum files."},

      {"runner_sources.wine_ge.repo", Type::String, std::string(runner_sources::kWineGERepo),
       Tier::Advanced, "GitHub \"owner/repo\" Wine-GE builds are downloaded from."},

      {"runner_sources.wine_ge.asset_pattern", Type::String,
       std::string(runner_sources::kWineGEAssetPattern), Tier::Expert,
       "Glob a release's assets are filtered to before offering one to download."},

      {"runner_sources.legendary.repo", Type::String, std::string(runner_sources::kLegendaryRepo),
       Tier::Advanced, "GitHub \"owner/repo\" Legendary (the Epic Games Store CLI client) is downloaded from."},

      {"runner_sources.legendary.asset_pattern", Type::String,
       std::string(runner_sources::kLegendaryAssetPattern), Tier::Expert,
       "Glob a release's assets are filtered to before offering one to download — "
       "Legendary ships one standalone Linux binary per release, not an archive."},

      {"runner_sources.gog.repo", Type::String, std::string(runner_sources::kGogdlRepo),
       Tier::Advanced, "GitHub \"owner/repo\" gogdl (the GOG downloader) is downloaded from."},

      {"runner_sources.gog.asset_pattern", Type::String, std::string(runner_sources::kGogdlAssetPattern),
       Tier::Expert,
       "Glob a release's assets are filtered to before offering one to download — "
       "gogdl ships one standalone Linux binary per release, not an archive."},

      {"runner_sources.itch.repo", Type::String, std::string(runner_sources::kButlerRepo),
       Tier::Advanced, "GitHub \"owner/repo\" butler (itch.io's launcher-integration daemon) is downloaded from."},

      {"runner_sources.itch.asset_pattern", Type::String, std::string(runner_sources::kButlerAssetPattern),
       Tier::Expert,
       "Glob a release's assets are filtered to before offering one to download — "
       "butler ships zipped, with shared libraries the binary needs alongside it."},

      {"runner_sources.humble.repo", Type::String, std::string(runner_sources::kHumbleCliRepo),
       Tier::Advanced, "GitHub \"owner/repo\" humble-cli (an unofficial Humble Bundle CLI) is downloaded from."},

      {"runner_sources.humble.asset_pattern", Type::String, std::string(runner_sources::kHumbleCliAssetPattern),
       Tier::Expert, "Glob a release's assets are filtered to before offering one to download."},

      {"metadata.enabled", Type::Bool, true, Tier::Basic,
       "Fetch cover art and store metadata (description, genre, ProtonDB compatibility "
       "tier) automatically when a game is detected. Steam-owned games use Steam's own "
       "public APIs and ProtonDB, no key needed; everything else needs "
       "steamgriddb.api_key set to fetch cover art, and has no metadata source at all."},

      // Basic, not Expert: without this every non-Steam game in the library
      // is stuck with a generated placeholder, and a setting you have to
      // turn on "advanced" to discover is one nobody discovers.
      {"steamgriddb.api_key", Type::String, "", Tier::Basic,
       "Free API key from steamgriddb.com. Non-Steam games need it to get cover art at all — "
       "there is no other free source for one. Steam-owned games never need it. Left empty, "
       "fetching metadata for a non-Steam game fails with no_steamgriddb_key rather than "
       "appearing to succeed."},

      {"events.sse_keepalive_s", Type::Int, 0, Tier::Expert,
       "Seconds between keepalive comments on the event stream. 0 disables them; a Unix "
       "socket does not need them and a timer would cost idle wakeups.",
       Range(0, 3600)},
  };

  for (Entry& entry : entries_) {
    entry.category = CategoryFor(entry.key);
  }
  // The schema type alone can't say "this string is a credential" or "this
  // string is a runner reference the UI could offer a picker for" — named
  // here instead of guessed at.
  if (const auto it = std::ranges::find(entries_, "steamgriddb.api_key", &Entry::key);
      it != entries_.end()) {
    it->is_secret = true;
  }
  if (const auto it = std::ranges::find(entries_, "default_runner.windows", &Entry::key);
      it != entries_.end()) {
    it->is_runner_ref = true;
  }
}

const Schema& Schema::Instance() {
  static const Schema instance;
  return instance;
}

const Entry* Schema::Find(std::string_view key) const {
  const auto it = std::ranges::find(entries_, key, &Entry::key);
  return it == entries_.end() ? nullptr : &*it;
}

nlohmann::json::json_pointer Schema::Pointer(std::string_view dotted_key) {
  std::string pointer;
  for (const char c : dotted_key) pointer += (c == '.') ? '/' : c;
  return nlohmann::json::json_pointer("/" + pointer);
}

json Schema::Defaults() const {
  json document = json::object();
  for (const Entry& entry : entries_) document[Pointer(entry.key)] = entry.default_value;
  return document;
}

std::optional<std::string> Schema::Validate(std::string_view key, const json& value) const {
  const Entry* entry = Find(key);
  if (entry == nullptr) return std::format("unknown setting \"{}\"", key);

  const bool type_ok = [&] {
    switch (entry->type) {
      case Type::Bool:        return value.is_boolean();
      case Type::Int:         return value.is_number_integer();
      case Type::Double:      return value.is_number();
      case Type::String:      return value.is_string();
      case Type::StringArray: return value.is_array() && std::ranges::all_of(value, [](const json& item) {
                                       return item.is_string();
                                     });
      case Type::Object:      return value.is_object();
    }
    return false;
  }();
  if (!type_ok) return std::format("expected {}", ToString(entry->type));

  if (entry->constraint) return entry->constraint.validate(value);
  return std::nullopt;
}

std::vector<std::string> Schema::ValidateDocument(const json& document) const {
  std::vector<std::string> problems;
  for (const Entry& entry : entries_) {
    const auto pointer = Pointer(entry.key);
    if (!document.contains(pointer)) continue;  // absent means "use the default"
    if (auto problem = Validate(entry.key, document[pointer])) {
      problems.push_back(std::format("{}: {}", entry.key, *problem));
    }
  }
  return problems;
}

std::string_view ToString(Tier tier) {
  switch (tier) {
    case Tier::Basic:    return "basic";
    case Tier::Advanced: return "advanced";
    case Tier::Expert:   return "expert";
  }
  return "basic";
}

std::string_view ToString(Type type) {
  switch (type) {
    case Type::Bool:        return "a boolean";
    case Type::Int:         return "an integer";
    case Type::Double:      return "a number";
    case Type::String:      return "a string";
    case Type::StringArray: return "an array of strings";
    case Type::Object:      return "an object";
  }
  return "a value";
}

std::optional<Tier> TierFromString(std::string_view text) {
  if (text == "basic") return Tier::Basic;
  if (text == "advanced") return Tier::Advanced;
  if (text == "expert") return Tier::Expert;
  return std::nullopt;
}

}  // namespace mira::config
