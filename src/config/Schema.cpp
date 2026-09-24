#include "config/Schema.h"

#include <algorithm>
#include <format>
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

// Stamps each entry with the section and group it was declared under, so
// neither is repeated on every entry.
class Sections {
public:
  explicit Sections(std::vector<Entry>& entries) : entries_(entries) {}

  void Section(std::string name) {
    category_ = std::move(name);
    group_ = 0;
  }
  void Divider() { ++group_; }
  void Add(Entry entry) {
    entry.category = category_;
    entry.group = group_;
    entries_.push_back(std::move(entry));
  }

private:
  std::vector<Entry>& entries_;
  std::string category_;
  int group_ = 0;
};

}  // namespace

Schema::Schema() {
  // Settings screens show these exactly in the order written here.
  //   s.Section("Name")  starts a category.   s.Divider()  draws a line.
  //   .key    the name on disk and in the API. Never rename it.
  //   .label  the name a settings screen shows.
  //   .scope  PerGame only if the daemon reads it through config::Resolver.
  Sections s(entries_);

  // --- Library ---------------------------------------------------------------
  s.Section("Library");

  s.Add({.key = "library_roots",
         .label = "Library Roots",
         .type = Type::StringArray,
         .default_value = json::array({"~/Games"}),
         .doc = "Folders watched for new games. Dropping a game folder into one of these is all "
                "that is required to add it."});

  s.Add({.key = "prefix_root",
         .label = "Prefix Root",
         .type = Type::String,
         .default_value = "~/Games/prefixes",
         .doc = "Where per-game data directories (Wine/Proton prefixes) are created. Always "
                "excluded from scanning, wherever it points."});

  s.Add({.key = "prefix_naming",
         .label = "Prefix Naming",
         .type = Type::String,
         .default_value = "name",
         .doc = "How a new prefix directory under prefix_root is named: \"name\" derives it from the "
                "game's title (e.g. \"celeste\", \"celeste-2\" on a collision); \"id\" uses the game's "
                "own id verbatim (e.g. \"gog-1207660413\"). Only affects newly-provisioned games -- "
                "already-provisioned ones keep their existing directory until explicitly relocated "
                "(POST /v1/games/{id}/relocate).",
         .constraint = OneOf({"name", "id"})});

  s.Add({.key = "prefix_provider",
         .label = "Prefix Creation",
         .type = Type::String,
         .default_value = "plain",
         .doc = "How a new prefix directory is created. \"plain\" lets the runner initialise it; "
                "\"template\" clones prefix_template, which is far faster on btrfs/xfs.",
         .constraint = OneOf({"plain", "template"})});

  s.Add({.key = "prefix_template",
         .label = "Prefix Template",
         .type = Type::String,
         .default_value = "",
         .doc = "An already-initialised prefix to clone when prefix_provider is \"template\". "
                "Cloned with reflinks where the filesystem supports them."});

  s.Divider();

  s.Add({.key = "auto_setup",
         .label = "Auto Setup",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Configure and provision newly detected games automatically. With this off, games "
                "are detected but wait for the frontend to configure them."});

  s.Add({.key = "open_config_on_add",
         .label = "Open Config on Add",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Ask the frontend to open its configuration menu when a game is added, so the "
                "auto-detected settings can be reviewed."});

  s.Add({.key = "command_wrappers",
         .label = "Command Wrappers",
         .type = Type::StringArray,
         .default_value = json::array(),
         .scope = Scope::PerGame,
         .doc = "Wrappers applied to the launch command in order, e.g. [\"gamemoderun\", \"gamescope -W "
                "1920 -H 1080\"]. The first entry ends up outermost. Each entry is split on spaces (like "
                "a game's own args -- no shell quoting support), and receives the game's command line as "
                "its arguments. A wrapper whose own binary isn't on PATH fails the launch with a clear "
                "error instead of a mysterious \"exited with code 127\"."});

  s.Add({.key = "library.remove_missing",
         .label = "Remove Missing",
         .type = Type::Bool,
         .default_value = false,
         .doc = "When a previously-detected game's folder disappears, forget it entirely instead of "
                "just marking it missing. Off by default: missing keeps the game's configuration, "
                "overrides, and playtime around in case a drive or network share is just temporarily "
                "offline. Never touches the game's files themselves either way — same as "
                "DELETE /v1/games/{id}."});

  s.Divider();

  s.Add({.key = "relocate.install_root",
         .label = "Relocate Destination",
         .type = Type::String,
         .default_value = "",
         .doc = "Library root that `mira relocate` moves game files into. Empty uses the first "
                "library_roots entry. Must be one of library_roots."});

  s.Add({.key = "relocate.allow_copy",
         .label = "Allow Cross-Filesystem Copy",
         .type = Type::Bool,
         .default_value = true,
         .doc = "When a relocate crosses filesystems, copy then delete the original. Off refuses "
                "cross-filesystem moves instead."});

  // --- Metadata --------------------------------------------------------------
  s.Section("Metadata");

  s.Add({.key = "metadata.enabled",
         .label = "Enable Metadata",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Fetch cover art and store metadata (description, genre, ProtonDB compatibility "
                "tier) automatically when a game is detected. Steam-owned games use Steam's own "
                "public APIs and ProtonDB, no key needed; everything else needs "
                "steamgriddb.api_key set to fetch cover art, and has no metadata source at all."});

  s.Add({.key = "steamgriddb.api_key",
         .label = "SteamGridDB API Key",
         .type = Type::String,
         .default_value = "",
         .doc = "Free API key from steamgriddb.com. Non-Steam games need it to get cover art at all — "
                "there is no other free source for one. Steam-owned games never need it. Left empty, "
                "fetching metadata for a non-Steam game fails with no_steamgriddb_key rather than "
                "appearing to succeed.",
         .is_secret = true});

  s.Add({.key = "metadata.protondb_for_non_steam",
         .label = "ProtonDB for Non-Steam Games",
         .type = Type::Bool,
         .default_value = false,
         .doc = "For a non-Steam game (Lutris, scanned, or manually added), look up a matching Steam "
                "AppID by name and fetch its ProtonDB compatibility tier — best-effort, and never "
                "changes how the game actually launches (that stays whatever runner_ref says). Off by "
                "default: matching by name can pick the wrong game, and this sends the game's name to "
                "Steam's public search API on every fetch. On has no effect on cover art, which is "
                "steamgriddb.api_key's own concern either way."});

  s.Add({.key = "lutris.import_art",
         .label = "Use Lutris Artwork",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Use the cover, banner and icon Lutris already downloaded for a Lutris-imported game."});

  s.Divider();

  s.Add({.key = "steam.import_playtime",
         .label = "Import Steam Playtime",
         .type = Type::Bool,
         .default_value = true,
         .doc = "On a Steam scan, adopt Steam's own playtime_forever total for a game when it's "
                "higher than what Mira recorded itself. Steam counts time played on any machine "
                "and long before Mira existed, so it's usually the larger and more complete "
                "number; the max of the two is kept so a Mira-tracked session is never lost to a "
                "stale Steam total. Needs steam.web_api_key + steam.steamid64."});

  s.Add({.key = "steam.web_api_key",
         .label = "Steam Web API Key",
         .type = Type::String,
         .default_value = "",
         .doc = "Free API key from steamcommunity.com/dev/apikey. Steam's on-disk files only "
                "describe games that are actually installed, so listing everything the account "
                "owns (GET /v1/library) and importing Steam's own playtime totals both need this "
                "plus steam.steamid64. Left empty, Steam simply contributes nothing to the "
                "library listing and playtime stays whatever Mira itself measured."});

  s.Add({.key = "steam.steamid64",
         .label = "Steam ID (64-bit)",
         .type = Type::String,
         .default_value = "",
         .doc = "The account's 64-bit Steam ID (steamcommunity.com profile URL, or a lookup "
                "site). Needed alongside steam.web_api_key — Steam's Web API identifies the "
                "account by this, not by the key."});

  // --- Sources ---------------------------------------------------------------
  s.Section("Sources");

  s.Add({.key = "steam.enabled",
         .label = "Enable Steam",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Detect installed Steam games and let Mira launch them alongside its own library."});

  s.Add({.key = "epic.enabled",
         .label = "Enable Epic Games",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Use Legendary (a native Epic Games Store CLI client) to authenticate, "
                "import already-installed Epic titles, and install/update new ones. The "
                "installed game itself still runs through Mira's own Wine/Proton runner, "
                "never through Legendary or Epic's own client. See \"mira epic setup\" "
                "if Legendary isn't already installed."});

  s.Add({.key = "gog.enabled",
         .label = "Enable GOG",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Use gogdl (Heroic's GOG downloader) to authenticate, import "
                "already-installed GOG titles, and install/update new ones. The "
                "installed game itself still runs through Mira's own Wine/Proton "
                "runner (or natively, for the titles GOG ships a Linux build of), "
                "never through gogdl or GOG Galaxy. See \"mira gog setup\" if gogdl "
                "isn't already installed."});

  s.Add({.key = "itch.enabled",
         .label = "Enable Itch.io",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Use butler (itch.io's own launcher-integration daemon) to "
                "authenticate, import already-installed itch.io titles, and "
                "install/update new ones. The installed game itself still runs "
                "through Mira's own Wine/Proton runner (or natively, for the many "
                "itch.io titles that ship a Linux build), never through butler or "
                "the itch app. See \"mira itch setup\" if butler isn't already "
                "installed."});

  s.Add({.key = "humble.enabled",
         .label = "Enable Humble Bundle",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Use humble-cli (an unofficial Humble Bundle CLI) to list purchased "
                "bundles and download items from them. Humble Bundle has no "
                "\"installed game\" concept of its own — a downloaded item is a "
                "plain file (installer, archive, or DRM-free build), added as a "
                "game manually afterward like any other manually-acquired title."});

  s.Add({.key = "amazon.enabled",
         .label = "Enable Amazon Games",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Use nile (Heroic's Amazon Games client) to log in, list and install Amazon Games / Prime "
                "Gaming titles. Installed games run through Mira's own Wine/Proton runner. See \"mira "
                "amazon setup\" if nile isn't already installed."});

  s.Add({.key = "lutris.enabled",
         .label = "Enable Lutris",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Let \"mira lutris import\" / POST /v1/lutris/import read Lutris's own "
                "game database (pga.db) and per-game configs and add them alongside "
                "Mira's own library."});

  s.Divider();

  s.Add({.key = "steam.root",
         .label = "Steam Root",
         .type = Type::String,
         .default_value = "",
         .doc = "Override for Steam's install directory. Empty auto-detects "
                "~/.steam/steam, then ~/.local/share/Steam."});

  s.Add({.key = "steam.launch_mode",
         .label = "Steam Launch Mode",
         .type = Type::String,
         .default_value = "steam",
         .scope = Scope::PerGame,
         .doc = "How launching a Steam game works. \"steam\" fires "
                "steam://rungameid/<appid> and lets the Steam client launch it — full "
                "achievements/overlay support; Mira didn't spawn the process, so it "
                "falls back to steam.track_process (below) rather than normal exit-"
                "code/signal tracking. \"direct\" has Mira exec the game itself, "
                "through the same Proton build and prefix Steam already set up, with "
                "normal Mira process tracking (stop/crash/playtime) — override per "
                "game via games.toml overrides if one game needs the other mode.",
         .constraint = OneOf({"steam", "direct"})});

  s.Add({.key = "steam.track_process",
         .label = "Track Steam Process",
         .type = Type::Bool,
         .default_value = true,
         .scope = Scope::PerGame,
         .doc = "For steam.launch_mode \"steam\": poll /proc for the actual game "
                "process (matched by the SteamAppId/SteamGameId environment variable "
                "Steam itself sets — the same one the Steamworks API reads) so Mira "
                "still shows the game as running and records playtime, even though "
                "it didn't spawn the process. No crash/exit-code detection either "
                "way — that's Steam's own client's job. Off disables the /proc scan "
                "entirely; launching still works, Mira just won't show it running."});

  s.Divider();

  s.Add({.key = "epic.legendary_bin",
         .label = "Epic Legendary Binary",
         .type = Type::String,
         .default_value = "",
         .doc = "Path to the legendary binary. Empty tries Mira's own managed download "
                "(see \"mira epic setup\"), then $PATH."});

  s.Add({.key = "gog.gogdl_bin",
         .label = "GOG GOGDL Binary",
         .type = Type::String,
         .default_value = "",
         .doc = "Path to the gogdl binary. Empty tries Mira's own managed download "
                "(see \"mira gog setup\"), then $PATH."});

  s.Add({.key = "itch.butler_bin",
         .label = "Itch Butler Binary",
         .type = Type::String,
         .default_value = "",
         .doc = "Path to the butler binary. Empty tries Mira's own managed download "
                "(see \"mira itch setup\"), then $PATH."});

  s.Add({.key = "humble.humble_cli_bin",
         .label = "Humble CLI Binary",
         .type = Type::String,
         .default_value = "",
         .doc = "Path to the humble-cli binary. Empty tries Mira's own managed "
                "download (see \"mira humble setup\"), then $PATH."});

  s.Add({.key = "amazon.nile_bin",
         .label = "Amazon Nile Binary",
         .type = Type::String,
         .default_value = "",
         .doc = "Path to the nile binary. Empty tries Mira's own managed download "
                "(see \"mira amazon setup\"), then $PATH."});

  s.Add({.key = "lutris.data_dir",
         .label = "Lutris Data Directory",
         .type = Type::String,
         .default_value = "",
         .doc = "Override for where Lutris keeps pga.db and its per-game configs. "
                "Empty auto-detects $XDG_DATA_HOME/lutris, then ~/.local/share/lutris."});

  s.Divider();

  s.Add({.key = "gog.install_root",
         .label = "Gog Install Dir",
         .type = Type::String,
         .default_value = "~/.local/share/mira/gog",
         .doc = "Where GOG titles are installed to, one folder per game (see gog.folder_naming) — "
                "unlike Legendary/butler, gogdl doesn't choose or remember an "
                "install location on its own, so Mira has to. Deliberately outside "
                "library_roots' usual defaults (e.g. ~/Games) — a real game install "
                "here would otherwise also get auto-detected as a second, bogus "
                "scan-sourced game."});

  s.Add({.key = "gog.folder_naming",
         .label = "GOG Folder Naming",
         .type = Type::String,
         .default_value = "title",
         .doc = "How a GOG install folder under gog.install_root is named: \"title\" (e.g. "
                "\"Hollow Knight\") or \"id\" (the GOG product id).",
         .constraint = OneOf({"title", "id"})});

  s.Add({.key = "itch.install_root",
         .label = "Itch Install Dir",
         .type = Type::String,
         .default_value = "~/.local/share/mira/itch",
         .doc = "Where itch.io titles are installed to — registered with butlerd as "
                "an install location on first use. Deliberately outside "
                "library_roots' usual defaults, same reasoning as gog.install_root."});

  s.Add({.key = "amazon.install_root",
         .label = "Amazon Install Dir",
         .type = Type::String,
         .default_value = "~/.local/share/mira/amazon",
         .doc = "Where Amazon titles are installed to, one folder per game. Outside library_roots for the "
                "same reason as gog.install_root."});

  s.Add({.key = "humble.download_root",
         .label = "Humble Install Dir",
         .type = Type::String,
         .default_value = "~/Downloads/HumbleBundle",
         .doc = "Where downloaded bundle items land, one subdirectory per bundle key."});

  // --- Runners ---------------------------------------------------------------
  s.Section("Runners");

  s.Add({.key = "default_runner.windows",
         .label = "Default Wine/Proton Runner",
         .type = Type::String,
         .default_value = "auto",
         .doc = "Runner for Windows games as \"kind:name\" (e.g. \"proton:GE-Proton11-7\", "
                "\"wine:system\"; \"latest\" as the name picks the newest installed build), or "
                "\"auto\" to pick the best installed runner — Proton if any build is present, "
                "otherwise Wine.",
         .is_runner_ref = true});

  s.Add({.key = "default_runner.native",
         .label = "Default Native Runner",
         .type = Type::String,
         .default_value = "native:native",
         .doc = "Runner for native Linux games.",
         .constraint = RunnerRef()});

  s.Add({.key = "launch.pin_runner",
         .label = "Pin Resolved Runner",
         .type = Type::Bool,
         .default_value = true,
         .doc = "When a Windows game with no runner set is launched, save the Proton/Wine build it "
                "resolved to so later launches use the same one."});

  s.Divider();

  s.Add({.key = "runner_search_paths",
         .label = "Runner Search Paths",
         .type = Type::StringArray,
         .default_value = json::array({"~/.steam/steam/compatibilitytools.d",
                          "~/.local/share/Steam/compatibilitytools.d",
                          "~/.local/share/mira/runners"}),
         .doc = "Directories scanned for installed Proton builds."});

  s.Add({.key = "wine_search_paths",
         .label = "Wine Search Paths",
         .type = Type::StringArray,
         .default_value = json::array({"~/.local/share/lutris/runners/wine"}),
         .doc = "Directories scanned for extra Wine builds (each a directory containing bin/wine), "
                "alongside the system wine on PATH."});

  s.Divider();

  s.Add({.key = "runner_sources.proton_ge.repo",
         .label = "Proton GE Repository",
         .type = Type::String,
         .default_value = std::string(runner_sources::kProtonGERepo),
         .doc = "GitHub \"owner/repo\" Proton-GE builds are downloaded from."});

  s.Add({.key = "runner_sources.proton_ge.asset_pattern",
         .label = "Proton GE Asset Pattern",
         .type = Type::String,
         .default_value = std::string(runner_sources::kProtonGEAssetPattern),
         .doc = "Glob a release's assets are filtered to before offering one to download — "
                "excludes non-x86_64 builds and checksum files."});

  s.Add({.key = "runner_sources.wine_ge.repo",
         .label = "Wine GE Repository",
         .type = Type::String,
         .default_value = std::string(runner_sources::kWineGERepo),
         .doc = "GitHub \"owner/repo\" Wine-GE builds are downloaded from."});

  s.Add({.key = "runner_sources.wine_ge.asset_pattern",
         .label = "Wine GE Asset Pattern",
         .type = Type::String,
         .default_value = std::string(runner_sources::kWineGEAssetPattern),
         .doc = "Glob a release's assets are filtered to before offering one to download."});

  s.Add({.key = "runner_sources.legendary.repo",
         .label = "Legendary Repository",
         .type = Type::String,
         .default_value = std::string(runner_sources::kLegendaryRepo),
         .doc = "GitHub \"owner/repo\" Legendary (the Epic Games Store CLI client) is downloaded from."});

  s.Add({.key = "runner_sources.legendary.asset_pattern",
         .label = "Legendary Asset Pattern",
         .type = Type::String,
         .default_value = std::string(runner_sources::kLegendaryAssetPattern),
         .doc = "Glob a release's assets are filtered to before offering one to download — "
                "Legendary ships one standalone Linux binary per release, not an archive."});

  s.Add({.key = "runner_sources.gog.repo",
         .label = "GOG Repository",
         .type = Type::String,
         .default_value = std::string(runner_sources::kGogdlRepo),
         .doc = "GitHub \"owner/repo\" gogdl (the GOG downloader) is downloaded from."});

  s.Add({.key = "runner_sources.gog.asset_pattern",
         .label = "GOG Asset Pattern",
         .type = Type::String,
         .default_value = std::string(runner_sources::kGogdlAssetPattern),
         .doc = "Glob a release's assets are filtered to before offering one to download — "
                "gogdl ships one standalone Linux binary per release, not an archive."});

  s.Add({.key = "runner_sources.itch.repo",
         .label = "Itch Repository",
         .type = Type::String,
         .default_value = std::string(runner_sources::kButlerRepo),
         .doc = "GitHub \"owner/repo\" butler (itch.io's launcher-integration daemon) is downloaded from."});

  s.Add({.key = "runner_sources.itch.asset_pattern",
         .label = "Itch Asset Pattern",
         .type = Type::String,
         .default_value = std::string(runner_sources::kButlerAssetPattern),
         .doc = "Glob a release's assets are filtered to before offering one to download — "
                "butler ships zipped, with shared libraries the binary needs alongside it."});

  s.Add({.key = "runner_sources.humble.repo",
         .label = "Humble Repository",
         .type = Type::String,
         .default_value = std::string(runner_sources::kHumbleCliRepo),
         .doc = "GitHub \"owner/repo\" humble-cli (an unofficial Humble Bundle CLI) is downloaded from."});

  s.Add({.key = "runner_sources.humble.asset_pattern",
         .label = "Humble Asset Pattern",
         .type = Type::String,
         .default_value = std::string(runner_sources::kHumbleCliAssetPattern),
         .doc = "Glob a release's assets are filtered to before offering one to download."});

  s.Add({.key = "runner_sources.amazon.repo",
         .label = "Amazon Repository",
         .type = Type::String,
         .default_value = std::string(runner_sources::kNileRepo),
         .doc = "GitHub \"owner/repo\" nile (the Amazon Games client) is downloaded from."});

  s.Add({.key = "runner_sources.amazon.asset_pattern",
         .label = "Amazon Asset Pattern",
         .type = Type::String,
         .default_value = std::string(runner_sources::kNileAssetPattern),
         .doc = "Glob a release's assets are filtered to before offering one to download."});

  // --- Launching -------------------------------------------------------------
  s.Section("Launching");

  s.Add({.key = "launch.stop_timeout_s",
         .label = "Stop Timeout (seconds)",
         .type = Type::Int,
         .default_value = 10,
         .doc = "How long to give a game to quit after \"stop\" before it's killed outright. The "
                "whole process group is signalled, since a real launch is umu -> proton -> wine -> "
                "the game.",
         .constraint = Range(0, 600)});

  s.Add({.key = "launch.log_max_mb",
         .label = "Log Max Size (MB)",
         .type = Type::Int,
         .default_value = 64,
         .scope = Scope::PerGame,
         .doc = "Cap on a game's own log file (see GET /v1/games/{id}/log), applied when a new session "
                "rotates the previous one out -- an oversized previous log is dropped instead of kept, "
                "so this bounds disk use to roughly 2x this value per game.",
         .constraint = Range(1, 1024)});

  s.Add({.key = "launch.env",
         .label = "Default Environment Variables",
         .type = Type::StringArray,
         .default_value = json::array(),
         .scope = Scope::PerGame,
         .doc = "KEY=VALUE environment variables set for every launch, e.g. [\"MANGOHUD=1\", "
                "\"DXVK_ASYNC=1\"]. A game's own env (per-game overrides) always wins over these. Not "
                "applied to a Steam game launched via steam.launch_mode \"steam\" -- Mira only hands "
                "off a steam:// URL there, it never builds the game's own command line."});

  s.Add({.key = "launch.pre_script",
         .label = "Default Pre-Launch Script",
         .type = Type::String,
         .default_value = "",
         .scope = Scope::PerGame,
         .doc = "Shell command run (via sh -c) before POST /v1/games/{id}/launch actually starts the "
                "game -- e.g. mounting a network drive a game needs, setting a CPU governor. Runs "
                "synchronously; a non-zero exit aborts the launch with the script's own output as the "
                "error. Empty disables it. Overridable per game (PATCH .../config)."});

  s.Add({.key = "launch.pre_timeout_s",
         .label = "Pre-Launch Timeout (seconds)",
         .type = Type::Int,
         .default_value = 30,
         .scope = Scope::PerGame,
         .doc = "How long launch.pre_script is given to finish before the launch is aborted outright, "
                "to keep a hung script from wedging a launch forever.",
         .constraint = Range(1, 600)});

  s.Add({.key = "launch.post_script",
         .label = "Default Post-Launch Script",
         .type = Type::String,
         .default_value = "",
         .scope = Scope::PerGame,
         .doc = "Shell command run once the game process exits (any reason: clean exit, crash, or "
                "stop), the mirror of launch.pre_script -- e.g. reverting a CPU governor change. Runs "
                "in the background; its own exit code is only logged, never affects the recorded "
                "playtime/crash state. Not run for a Steam game launched via steam.launch_mode "
                "\"steam\" unless steam.track_process is also on -- Mira otherwise never owns that "
                "process at all (see docs/api.md's Steam section)."});

  s.Add({.key = "launch.post_timeout_s",
         .label = "Post-Launch Timeout (seconds)",
         .type = Type::Int,
         .default_value = 30,
         .scope = Scope::PerGame,
         .doc = "How long launch.post_script is given to finish before it's killed outright, the "
                "mirror of launch.pre_timeout_s.",
         .constraint = Range(1, 600)});

  s.Add({.key = "launch.gamemode",
         .label = "Feral Gamemode",
         .type = Type::Bool,
         .default_value = false,
         .scope = Scope::PerGame,
         .doc = "Register this game with Feral Interactive's GameMode daemon automatically -- no "
                "command_wrappers entry needed. A no-op if the daemon isn't installed or isn't running; "
                "see GET /v1/gamemode/status."});

  // --- Installers ------------------------------------------------------------
  s.Section("Installers");

  s.Add({.key = "scan.auto_run_installers",
         .label = "Run Installers Automatically",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Silently run a detected installer (Inno Setup, NSIS) as soon as it's found, the same "
                "way a detected archive auto-extracts. Unrecognized formats and failed runs stay "
                "needs_install for `mira install`."});

  s.Add({.key = "install.retry_failed",
         .label = "Retry Failed Installs",
         .type = Type::Bool,
         .default_value = false,
         .doc = "Retry a failed automatic install on every scan instead of waiting for `mira install`."});

  s.Add({.key = "install.runner",
         .label = "Installer Runner",
         .type = Type::String,
         .default_value = "",
         .doc = "Runner used to run installers, e.g. \"proton:GE-Proton11-7\" or \"wine:latest\". "
                "Empty uses the game's own default (default_runner.windows). The game keeps it afterward, "
                "since its prefix was made with it."});

  s.Add({.key = "install.timeout_s",
         .label = "Installer Timeout (seconds)",
         .type = Type::Int,
         .default_value = 0,
         .doc = "Kill a silent installer after this many seconds. 0 = no limit.",
         .constraint = Range(0, 86400)});

  s.Add({.key = "install.show_progress",
         .label = "Show Installer Progress",
         .type = Type::Bool,
         .default_value = false,
         .doc = "Show Inno Setup's progress window during a silent install (/SILENT instead of "
                "/VERYSILENT). Still asks no questions. NSIS has no visible silent mode."});

  s.Divider();

  s.Add({.key = "install.detect_dirs",
         .label = "Install Detection Folders",
         .type = Type::StringArray,
         .default_value = json::array({"Program Files", "Program Files (x86)", "GOG Games", "Games"}),
         .doc = "Folders under the prefix's drive_c checked for a newly installed game when the installer "
                "didn't install into the game folder."});

  s.Add({.key = "install.inno_args",
         .label = "Inno Setup Arguments",
         .type = Type::String,
         .default_value = "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-",
         .doc = "Arguments for a silent Inno Setup install (GOG offline installers). /DIR is added."});

  s.Add({.key = "install.nsis_args",
         .label = "NSIS Arguments",
         .type = Type::String,
         .default_value = "/S",
         .doc = "Arguments for a silent NSIS install. /D is added."});

  // --- Store launchers ------------------------------------------------------
  s.Section("Store Launchers");

  s.Add({.key = "launchers.auto_import",
         .label = "Import Launcher Games on Scan",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Import games installed through a store launcher (Battle.net, Ubisoft Connect, EA app) "
                "on every scan."});

  s.Add({.key = "launchers.runner",
         .label = "Launcher Runner",
         .type = Type::String,
         .default_value = "",
         .doc = "Runner for a store launcher's prefix, e.g. \"proton:GE-Proton11-7\". Empty uses "
                "default_runner.windows. Games it installs use the same one."});

  s.Add({.key = "launchers.detect_timeout_s",
         .label = "Launcher Game Start Timeout (seconds)",
         .type = Type::Int,
         .default_value = 300,
         .doc = "How long to wait for a launcher to start a game (updates, login) before giving up tracking it.",
         .constraint = Range(10, 3600)});

  s.Add({.key = "launchers.umu_lookup",
         .label = "Look Up umu IDs",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Look up each newly imported launcher game at umu.openwinecomponents.org so its protonfixes "
                "apply. Sends only the store name and the game's store id."});

  s.Divider();

  s.Add({.key = "launchers.ubisoft.disable_overlay",
         .label = "Disable Ubisoft Overlay",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Turn off the Ubisoft Connect overlay when installing it; it often breaks games under Wine."});

  s.Add({.key = "launchers.battlenet.disable_hw_accel",
         .label = "Disable Battle.net Hardware Acceleration",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Turn off Battle.net's hardware acceleration when installing it; its UI renders blank under "
                "Wine otherwise."});

  // --- Detection -------------------------------------------------------------
  s.Section("Detection");

  s.Add({.key = "detect.rules",
         .label = "Detection Scoring Rules",
         .type = Type::StringArray,
         .default_value = json::array({"deny_patterns", "name_similarity", "depth", "shallowest"}),
         .doc = "Scoring rules applied to candidate executables, in order. Removing a rule "
                "disables it."});

  s.Add({.key = "detect.name_match_bonus",
         .label = "Name Match Bonus",
         .type = Type::Double,
         .default_value = 3.0,
         .doc = "Score added when an executable's name resembles its folder's name.",
         .constraint = Range(0.0, 100.0)});

  s.Add({.key = "detect.depth_penalty",
         .label = "Depth Penalty",
         .type = Type::Double,
         .default_value = 0.5,
         .doc = "Score subtracted per directory level, favouring executables near the top.",
         .constraint = Range(0.0, 100.0)});

  s.Add({.key = "detect.low_confidence_threshold",
         .label = "Low Confidence Threshold",
         .type = Type::Double,
         .default_value = 0.5,
         .doc = "Below this confidence a game is flagged for review. It is still configured and "
                "still launchable; the frontend just highlights it.",
         .constraint = Range(0.0, 1.0)});

  s.Add({.key = "detect.deny_name_patterns",
         .label = "Deny Name Patterns",
         .type = Type::StringArray,
         .default_value = json(known_exe_patterns::kDeny),
         .doc = "Executables matching these globs are heavily penalised: they are installers and "
                "helpers rather than games. Defaults are curated in "
                "src/config/KnownExePatterns.h — edit that file to add one, no need to touch this "
                "schema or recompile just to override it for yourself here."});

  s.Add({.key = "detect.installer_name_patterns",
         .label = "Installer Name Patterns",
         .type = Type::StringArray,
         .default_value = json(known_exe_patterns::kInstaller),
         .doc = "A candidate matching one of these globs, and meeting detect.installer_min_size_mb "
                "(itself, or sharing a folder with a file that does — installers are often a small "
                "stub exe next to a much larger separate payload), is flagged as an installer "
                "rather than the game itself: stored with status needs_install instead of being "
                "auto-provisioned as launchable."});

  s.Add({.key = "detect.installer_min_size_mb",
         .label = "Installer Min Size (MB)",
         .type = Type::Int,
         .default_value = 50,
         .doc = "Minimum size — of the candidate itself, or of any file alongside it — for a "
                "name-matched candidate to actually count as an installer, so a small stub or "
                "helper named like one doesn't get misflagged.",
         .constraint = Range(0, 1'000'000)});

  // --- Scanning --------------------------------------------------------------
  s.Section("Scanning");

  s.Add({.key = "scan.tag_by_root",
         .label = "Tag by Path Root",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Automatically tag each newly-detected game with the name of the library root folder "
                "it was found in (e.g. a game under ~/Games Mira gets tagged \"Games Mira\") -- useful "
                "for filtering multiple game folders (GET /v1/games?tag=...) with several "
                "library_roots configured. Only applied at detection time, not retroactively."});

  s.Add({.key = "scan.debounce_ms",
         .label = "Debounce Time (ms)",
         .type = Type::Int,
         .default_value = 3000,
         .doc = "How long a new folder must stop changing before it is scanned. Raise it if games "
                "arrive over a slow network share.",
         .constraint = Range(0, 600000)});

  s.Add({.key = "scan.max_depth",
         .label = "Max Scan Depth",
         .type = Type::Int,
         .default_value = 4,
         .doc = "How deep to search inside a game folder for executables.",
         .constraint = Range(1, 16)});

  s.Add({.key = "scan.auto_extract_archives",
         .label = "Auto-Extract Archives",
         .type = Type::Bool,
         .default_value = false,
         .doc = "Extract a .zip/.rar/.tar(.gz/.xz/.bz2)/.7z dropped directly into a library root, "
                "into a same-named folder, then delete the archive — so an archived game drop "
                "behaves like an already-extracted one. Off by default: silently deleting an "
                "archive is a real action to opt into, not assume. Extracting a .rar or .7z needs "
                "unrar/p7zip installed; a missing tool is reported, not silently skipped."});

  s.Add({.key = "scan.periodic_interval_s",
         .label = "Periodic Scan Interval (s)",
         .type = Type::Int,
         .default_value = 0,
         .doc = "Seconds between full rescans. 0 disables them, which is the default: inotify is "
                "authoritative and a timer would cost idle wakeups for nothing.",
         .constraint = Range(0, 86400)});

  s.Add({.key = "scan.ignore_globs",
         .label = "Globs to Ignore",
         .type = Type::StringArray,
         .default_value = json::array({".*", "*/Redist*", "*/DirectX*", "*/_CommonRedist*", "*/DotNet*"}),
         .doc = "Paths matching these globs are never treated as games."});

  // --- Desktop Entries -------------------------------------------------------
  s.Section("Desktop Entries");

  s.Add({.key = "desktop_entries.enabled",
         .label = "Enable Desktop Entries",
         .type = Type::Bool,
         .default_value = true,
         .scope = Scope::PerGame,
         .doc = "Add each ready game to your application menu as a .desktop entry, so it can be "
                "launched from the desktop like any other app. Turn this off to keep Mira's games "
                "out of your menu entirely."});

  s.Add({.key = "desktop_entries.directory",
         .label = "Desktop Entries Directory",
         .type = Type::String,
         .default_value = "~/.local/share/applications",
         .doc = "Where .desktop entries are written. Only files Mira created (mira-<id>.desktop) are "
                "ever touched."});

  s.Add({.key = "desktop_entries.categories",
         .label = "Desktop Entries Categories",
         .type = Type::String,
         .default_value = "Game;",
         .scope = Scope::PerGame,
         .doc = "Freedesktop Categories= value for generated entries, deciding where they appear in "
                "the menu."});

  s.Add({.key = "desktop_entries.exec_mode",
         .label = "Desktop Entries Execution Mode",
         .type = Type::String,
         .default_value = "cli",
         .scope = Scope::PerGame,
         .doc = "What a menu entry runs. \"cli\" (`mira launch <id>`) requires mirad to already be "
                "running and fails clearly if it isn't; \"frontend\" starts the frontend, which "
                "brings the daemon up itself. Either way the launch goes through Mira, which is "
                "what makes playtime get recorded — a menu entry that ran the game directly would "
                "launch fine and log nothing.",
         .constraint = OneOf({"cli", "frontend"})});

  s.Divider();

  s.Add({.key = "desktop_import.enabled",
         .label = "Enable Desktop Import",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Let \"mira desktop-entries list\"/\"import\" and the matching REST "
                "endpoints read already-installed application-menu (.desktop) entries "
                "and add them as games — this is how a Flatpak app gets added, since "
                "every Flatpak-exported entry already carries the app id needed to "
                "relaunch it. Manual: nothing is added until you pick which ones."});

  s.Add({.key = "desktop_import.extra_dirs",
         .label = "Extra Desktop Import Directories",
         .type = Type::StringArray,
         .default_value = json::array(),
         .doc = "Extra directories to search for .desktop files, beyond the standard "
                "$XDG_DATA_HOME/applications, $XDG_DATA_DIRS entries, and the two "
                "well-known Flatpak export directories."});

  // --- Advanced --------------------------------------------------------------
  s.Section("Advanced");

  s.Add({.key = "socket_path",
         .label = "Socket Path",
         .type = Type::String,
         .default_value = "$XDG_RUNTIME_DIR/mira/mirad.sock",
         .doc = "Unix socket the daemon listens on. The frontend and CLI must agree with this.",
         .constraint = NonEmptyString()});

  s.Add({.key = "log.level",
         .label = "Log Level",
         .type = Type::String,
         .default_value = "info",
         .doc = "Logging verbosity: debug, info, warn or error.",
         .constraint = OneOf({"debug", "info", "warn", "error"})});

  s.Add({.key = "events.sse_keepalive_s",
         .label = "SSE Keepalive (seconds)",
         .type = Type::Int,
         .default_value = 0,
         .doc = "Seconds between keepalive comments on the event stream. 0 disables them; a Unix "
                "socket does not need them and a timer would cost idle wakeups.",
         .constraint = Range(0, 3600)});
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

std::string_view ToString(Scope scope) {
  switch (scope) {
    case Scope::Global:  return "global";
    case Scope::PerGame: return "per_game";
  }
  return "global";
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

}  // namespace mira::config
