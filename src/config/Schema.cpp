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

// A runner reference, "auto", or empty for "fall back to the default".
Validator OptionalRunnerRef() {
  return [](const json& value) -> std::optional<std::string> {
    if (!value.is_string()) return "expected a string";
    const std::string text = value.get<std::string>();
    if (text.empty() || text == "auto") return std::nullopt;
    return RunnerRef()(value);
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
         .label = "Library Folders",
         .type = Type::StringArray,
         .default_value = json::array({"~/Games"}),
         .doc = "Folders Mira watches for new games. Drop a game folder into one to add it."});

  s.Add({.key = "prefix_root",
         .label = "Prefix Folder",
         .type = Type::String,
         .default_value = "~/Games/prefixes",
         .doc = "The folder where each game's Wine or Proton prefix is created. Mira never scans it "
                "for games."});

  s.Add({.key = "prefix_naming",
         .label = "Prefix Folder Naming",
         .type = Type::String,
         .default_value = "name",
         .doc = "How a new prefix folder is named. \"name\" uses the game's title (for example "
                "\"celeste\", or \"celeste-2\" if that is taken). \"id\" uses the game's id (for "
                "example \"gog-1207660413\"). Only new games are affected. Existing games keep "
                "their folder until you move them.",
         .constraint = OneOf({"name", "id"})});

  s.Divider();

  s.Add({.key = "auto_setup",
         .label = "Set Up New Games Automatically",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Set up new games automatically when they are found. When off, new games wait "
                "until you configure them."});

  s.Add({.key = "open_config_on_add",
         .label = "Open Settings for New Games",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Open a new game's settings when it is added, so you can check what Mira "
                "detected."});

  s.Add({.key = "command_wrappers",
         .label = "Command Wrappers",
         .type = Type::StringArray,
         .default_value = json::array(),
         .scope = Scope::PerGame,
         .doc = "Programs that wrap the game's launch command, for example \"gamemoderun\" or "
                "\"gamescope -W 1920 -H 1080\". The first entry is the outermost wrapper. Each entry "
                "is split on spaces, so quotes do not work. If a wrapper is not installed, the "
                "launch fails with an error that names it.",
         .game_doc = "Programs that wrap this game's launch command. This list replaces the "
                     "global one for this game. The first entry is the outermost wrapper."});

  s.Add({.key = "library.remove_missing",
         .label = "Remove Missing Games",
         .type = Type::Bool,
         .default_value = false,
         .doc = "Remove a game from the library when its folder disappears, instead of marking it "
                "missing. A missing game keeps its settings and playtime in case a drive or network "
                "share is only offline. Mira never deletes game files either way."});

  s.Divider();

  s.Add({.key = "relocate.install_root",
         .label = "Move Destination Folder",
         .type = Type::String,
         .default_value = "",
         .doc = "The library folder that moved games go into. Empty uses the first library folder. "
                "It must be one of the library folders."});

  s.Add({.key = "relocate.allow_copy",
         .label = "Allow Moving Across Drives",
         .type = Type::Bool,
         .default_value = true,
         .doc = "When a move goes to another drive, copy the files and then delete the originals. "
                "When off, moves to another drive are refused."});

  // --- Metadata --------------------------------------------------------------
  s.Section("Metadata");

  s.Add({.key = "metadata.enabled",
         .label = "Fetch Metadata Automatically",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Fetch cover art and details (description, genre, ProtonDB rating) when a game is "
                "found. Steam games need no key. Other games need a SteamGridDB API key for cover "
                "art and have no details."});

  s.Add({.key = "steamgriddb.api_key",
         .label = "SteamGridDB API Key",
         .type = Type::String,
         .default_value = "",
         .doc = "A free key from steamgriddb.com. Games that are not from Steam need it to get cover "
                "art. Steam games never do.",
         .is_secret = true,
         .link = "https://www.steamgriddb.com/profile/preferences/api",
         .keywords = "sgdb cover art token"});

  s.Add({.key = "steamgriddb.nsfw",
         .label = "Include Adult Art",
         .type = Type::Bool,
         .default_value = false,
         .doc = "Also show art that SteamGridDB marks as adult in the art picker, with a label. "
                "Mira never picks it automatically.",
         .keywords = "sgdb"});

  s.Add({.key = "metadata.steamgriddb_id",
         .label = "SteamGridDB Game ID",
         .type = Type::Int,
         .default_value = 0,
         .scope = Scope::GameOnly,
         .doc = "The SteamGridDB game this game's art comes from. 0 uses the top search result for "
                "the game's name. Enter the ID from the game's steamgriddb.com page address when "
                "that result is the wrong game.",
         .constraint = Range(0, 1e12),
         .keywords = "sgdb"});

  s.Add({.key = "metadata.steam_art_by_name",
         .label = "Match Steam Art by Name",
         .type = Type::Bool,
         .default_value = true,
         .doc = "If a game has no other art, use the cover of the Steam game with the same name "
                "(case and punctuation are ignored). Needs no key. This sends the game's name to "
                "Steam's public search."});

  s.Add({.key = "metadata.protondb_for_non_steam",
         .label = "ProtonDB for Non-Steam Games",
         .type = Type::Bool,
         .default_value = false,
         .doc = "Show a ProtonDB rating for games that are not from Steam, found by matching the "
                "game's name to a Steam game. The match can be wrong, and each lookup sends the "
                "game's name to Steam's public search. It does not change how the game launches or "
                "its cover art."});

  s.Add({.key = "lutris.import_art",
         .label = "Use Lutris Artwork",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Use the cover, banner and icon that Lutris already downloaded for imported games."});

  s.Divider();

  s.Add({.key = "steam.import_playtime",
         .label = "Import Steam Playtime",
         .type = Type::Bool,
         .default_value = true,
         .doc = "When scanning Steam, use Steam's total playtime for a game if it is higher than "
                "what Mira recorded. Steam counts time from before Mira and from other computers. "
                "Mira keeps the larger of the two totals. Needs the Steam Web API key and Steam ID."});

  s.Add({.key = "steam.web_api_key",
         .label = "Steam Web API Key",
         .type = Type::String,
         .default_value = "",
         .doc = "A free key from steamcommunity.com/dev/apikey. Steam's local files only list "
                "installed games. With this key and your Steam ID, Mira can also list every game "
                "you own and import your Steam playtime.",
         .is_secret = true,
         .link = "https://steamcommunity.com/dev/apikey",
         .keywords = "token owned games"});

  s.Add({.key = "steam.steamid64",
         .label = "Steam ID (64-bit)",
         .type = Type::String,
         .default_value = "",
         .doc = "Your 64-bit Steam ID, a 17-digit number. It appears in your Steam profile URL or "
                "on a Steam ID lookup site. Needed together with the Steam Web API key.",
         .link = "https://store.steampowered.com/account/",
         .keywords = "steamid account user number"});

  // --- Sources ---------------------------------------------------------------
  s.Section("Sources");

  s.Add({.key = "steam.enabled",
         .label = "Enable Steam",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Find installed Steam games and launch them from Mira."});

  s.Add({.key = "epic.enabled",
         .label = "Enable Epic Games",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Use Legendary, a command line Epic Games Store client, to log in, import "
                "installed Epic games, and install or update games. Games still run through "
                "Mira's Wine or Proton runner. Run \"mira epic setup\" if Legendary is not "
                "installed."});

  s.Add({.key = "gog.enabled",
         .label = "Enable GOG",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Use gogdl, Heroic's GOG downloader, to log in, import installed GOG games, and "
                "install or update games. Games run through Mira's Wine or Proton runner, or "
                "natively when GOG offers a Linux build. Run \"mira gog setup\" if gogdl is not "
                "installed."});

  s.Add({.key = "itch.enabled",
         .label = "Enable Itch.io",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Use butler, itch.io's own tool, to log in, import installed itch.io games, and "
                "install or update games. Games run through Mira's Wine or Proton runner, or "
                "natively when a Linux build exists. Run \"mira itch setup\" if butler is not "
                "installed."});

  s.Add({.key = "humble.enabled",
         .label = "Enable Humble Bundle",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Use humble-cli, an unofficial Humble Bundle client, to list your bundles and "
                "download their items. Downloads are plain files (installers, archives, or "
                "DRM-free builds), so add them as games manually afterward."});

  s.Add({.key = "amazon.enabled",
         .label = "Enable Amazon Games",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Use nile, Heroic's Amazon Games client, to log in, list, and install Amazon and "
                "Prime Gaming games. Games run through Mira's Wine or Proton runner. Run \"mira "
                "amazon setup\" if nile is not installed."});

  s.Add({.key = "lutris.enabled",
         .label = "Enable Lutris",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Let Mira read Lutris's game database and per-game configs, so you can import "
                "Lutris games into the library."});

  s.Divider();

  s.Add({.key = "steam.root",
         .label = "Steam Folder",
         .type = Type::String,
         .default_value = "",
         .doc = "Steam's install folder. Empty looks in ~/.steam/steam, then ~/.local/share/Steam."});

  s.Add({.key = "steam.launch_mode",
         .label = "Steam Launch Mode",
         .type = Type::String,
         .default_value = "steam",
         .scope = Scope::PerGame,
         .doc = "How Steam games launch. \"steam\" asks the Steam client to start the game, which "
                "keeps achievements and the overlay working. Mira then follows the game by its "
                "process instead of tracking exits and crashes. \"direct\" makes Mira start the "
                "game itself with the Proton build and prefix Steam set up, with full tracking of "
                "stops, crashes and playtime.",
         .constraint = OneOf({"steam", "direct"})});

  s.Add({.key = "steam.track_process",
         .label = "Track Steam Process",
         .type = Type::Bool,
         .default_value = true,
         .scope = Scope::PerGame,
         .doc = "For games launched through the Steam client, watch for the game's process so "
                "Mira shows it as running and records playtime. Crashes and exit codes are not "
                "detected. When off, the game still launches, but Mira does not show it as "
                "running."});

  s.Divider();

  s.Add({.key = "epic.legendary_bin",
         .label = "Legendary Binary",
         .type = Type::String,
         .default_value = "",
         .doc = "Path to the legendary binary. Empty uses the copy Mira downloaded, then the one "
                "on your PATH."});

  s.Add({.key = "gog.gogdl_bin",
         .label = "gogdl Binary",
         .type = Type::String,
         .default_value = "",
         .doc = "Path to the gogdl binary. Empty uses the copy Mira downloaded, then the one on "
                "your PATH."});

  s.Add({.key = "itch.butler_bin",
         .label = "butler Binary",
         .type = Type::String,
         .default_value = "",
         .doc = "Path to the butler binary. Empty uses the copy Mira downloaded, then the one on "
                "your PATH."});

  s.Add({.key = "humble.humble_cli_bin",
         .label = "humble-cli Binary",
         .type = Type::String,
         .default_value = "",
         .doc = "Path to the humble-cli binary. Empty uses the copy Mira downloaded, then the one "
                "on your PATH."});

  s.Add({.key = "amazon.nile_bin",
         .label = "nile Binary",
         .type = Type::String,
         .default_value = "",
         .doc = "Path to the nile binary. Empty uses the copy Mira downloaded, then the one on "
                "your PATH."});

  s.Add({.key = "lutris.data_dir",
         .label = "Lutris Data Folder",
         .type = Type::String,
         .default_value = "",
         .doc = "The folder where Lutris keeps its database and game configs. Empty looks in "
                "$XDG_DATA_HOME/lutris, then ~/.local/share/lutris."});

  s.Divider();

  s.Add({.key = "gog.install_root",
         .label = "GOG Install Folder",
         .type = Type::String,
         .default_value = "~/.local/share/mira/gog",
         .doc = "The folder GOG games are installed into, with one subfolder per game. Mira never "
                "scans it for games, so it can be inside a library folder."});

  s.Add({.key = "gog.folder_naming",
         .label = "GOG Folder Naming",
         .type = Type::String,
         .default_value = "title",
         .doc = "How each GOG game's folder is named: \"title\" (for example \"Hollow Knight\") or "
                "\"id\" (the GOG product id).",
         .constraint = OneOf({"title", "id"})});

  s.Add({.key = "itch.collections",
         .label = "itch.io Collections",
         .type = Type::StringArray,
         .default_value = json::array(),
         .doc = "Extra itch.io collections to show, added by id or by link (https://itch.io/c/...). "
                "Your own collections always show. You cannot install a paid game you do not "
                "own."});

  s.Add({.key = "itch.install_root",
         .label = "itch.io Install Folder",
         .type = Type::String,
         .default_value = "~/.local/share/mira/itch",
         .doc = "The folder itch.io games are installed into. Mira never scans it for games, so "
                "it can be inside a library folder."});

  s.Add({.key = "amazon.install_root",
         .label = "Amazon Games Install Folder",
         .type = Type::String,
         .default_value = "~/.local/share/mira/amazon",
         .doc = "The folder Amazon games are installed into, with one subfolder per game. Mira "
                "never scans it for games, so it can be inside a library folder."});

  s.Add({.key = "humble.download_root",
         .label = "Humble Bundle Download Folder",
         .type = Type::String,
         .default_value = "~/Downloads/HumbleBundle",
         .doc = "The folder Humble Bundle downloads go into, with one subfolder per bundle."});

  // --- Runners ---------------------------------------------------------------
  s.Section("Runners");

  s.Add({.key = "default_runner.windows",
         .label = "Default Wine/Proton Runner",
         .type = Type::String,
         .default_value = "auto",
         .doc = "The runner for Windows games, written as \"kind:name\", for example "
                "\"proton:GE-Proton11-7\" or \"wine:system\". A name of \"latest\" picks the newest "
                "installed build. \"auto\" picks the best installed runner: Proton if any is "
                "installed, otherwise Wine.",
         .is_runner_ref = true});

  s.Add({.key = "default_runner.native",
         .label = "Default Native Runner",
         .type = Type::String,
         .default_value = "native:native",
         .doc = "The runner for native Linux games. The default starts them directly.",
         .constraint = RunnerRef()});

  for (const auto& [id, name] : {std::pair{"epic", "Epic Games"}, std::pair{"gog", "GOG"},
                                 std::pair{"itch", "itch.io"}, std::pair{"amazon", "Amazon Games"}}) {
    s.Add({.key = std::format("{}.runner", id),
           .label = std::format("{} Runner", name),
           .type = Type::String,
           .default_value = "",
           .doc = std::format("The runner for {} games that have no runner of their own. Empty "
                              "uses the default Wine/Proton runner.",
                              name),
           .constraint = OptionalRunnerRef(),
           .is_runner_ref = true});
  }

  s.Add({.key = "launch.pin_runner",
         .label = "Remember the Chosen Runner",
         .type = Type::Bool,
         .default_value = true,
         .doc = "The first time a Windows game with no runner launches, remember which Proton or "
                "Wine build was chosen and keep using it."});

  s.Divider();

  s.Add({.key = "runner_scan_common_dirs",
         .label = "Scan Common Runner Folders",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Also look for Proton and Wine builds where Steam, your distro, Heroic, Bottles and "
                "Lutris keep them, in addition to the search paths below."});

  s.Add({.key = "runner_search_paths",
         .label = "Proton Search Folders",
         .type = Type::StringArray,
         .default_value = json::array({"~/.steam/steam/compatibilitytools.d",
                          "~/.local/share/Steam/compatibilitytools.d",
                          "~/.local/share/mira/runners"}),
         .doc = "Folders searched for installed Proton builds. New downloads go into the first."});

  s.Add({.key = "wine_search_paths",
         .label = "Wine Search Folders",
         .type = Type::StringArray,
         .default_value = json::array({"~/.local/share/lutris/runners/wine"}),
         .doc = "Folders searched for Wine builds, each containing bin/wine. The system Wine on "
                "your PATH is also used. New downloads go into the first."});

  s.Divider();

  s.Add({.key = "runner_sources.proton_ge.repo",
         .label = "Proton-GE Download Repository",
         .type = Type::String,
         .default_value = std::string(runner_sources::kProtonGERepo),
         .doc = "The GitHub repository (\"owner/repo\") that Proton-GE is downloaded from."});

  s.Add({.key = "runner_sources.proton_ge.asset_pattern",
         .label = "Proton-GE File Pattern",
         .type = Type::String,
         .default_value = std::string(runner_sources::kProtonGEAssetPattern),
         .doc = "A glob pattern that picks which file of a release to download. The default skips "
                "non-x86_64 builds and checksum files."});

  s.Add({.key = "runner_sources.wine_ge.repo",
         .label = "Wine-GE Download Repository",
         .type = Type::String,
         .default_value = std::string(runner_sources::kWineGERepo),
         .doc = "The GitHub repository (\"owner/repo\") that Wine-GE is downloaded from."});

  s.Add({.key = "runner_sources.wine_ge.asset_pattern",
         .label = "Wine-GE File Pattern",
         .type = Type::String,
         .default_value = std::string(runner_sources::kWineGEAssetPattern),
         .doc = "A glob pattern that picks which file of a release to download."});

  s.Add({.key = "runner_sources.legendary.repo",
         .label = "Legendary Download Repository",
         .type = Type::String,
         .default_value = std::string(runner_sources::kLegendaryRepo),
         .doc = "The GitHub repository (\"owner/repo\") that Legendary is downloaded from."});

  s.Add({.key = "runner_sources.legendary.asset_pattern",
         .label = "Legendary File Pattern",
         .type = Type::String,
         .default_value = std::string(runner_sources::kLegendaryAssetPattern),
         .doc = "A glob pattern that picks which file of a release to download. Legendary ships "
                "as a single Linux binary, not an archive."});

  s.Add({.key = "runner_sources.gog.repo",
         .label = "gogdl Download Repository",
         .type = Type::String,
         .default_value = std::string(runner_sources::kGogdlRepo),
         .doc = "The GitHub repository (\"owner/repo\") that gogdl is downloaded from."});

  s.Add({.key = "runner_sources.gog.asset_pattern",
         .label = "gogdl File Pattern",
         .type = Type::String,
         .default_value = std::string(runner_sources::kGogdlAssetPattern),
         .doc = "A glob pattern that picks which file of a release to download. gogdl ships as a "
                "single Linux binary, not an archive."});

  s.Add({.key = "runner_sources.itch.repo",
         .label = "butler Download Repository",
         .type = Type::String,
         .default_value = std::string(runner_sources::kButlerRepo),
         .doc = "The GitHub repository (\"owner/repo\") that butler is downloaded from."});

  s.Add({.key = "runner_sources.itch.asset_pattern",
         .label = "butler File Pattern",
         .type = Type::String,
         .default_value = std::string(runner_sources::kButlerAssetPattern),
         .doc = "A glob pattern that picks which file of a release to download. butler ships as a "
                "zip that includes the libraries it needs."});

  s.Add({.key = "runner_sources.humble.repo",
         .label = "humble-cli Download Repository",
         .type = Type::String,
         .default_value = std::string(runner_sources::kHumbleCliRepo),
         .doc = "The GitHub repository (\"owner/repo\") that humble-cli is downloaded from."});

  s.Add({.key = "runner_sources.humble.asset_pattern",
         .label = "humble-cli File Pattern",
         .type = Type::String,
         .default_value = std::string(runner_sources::kHumbleCliAssetPattern),
         .doc = "A glob pattern that picks which file of a release to download."});

  s.Add({.key = "runner_sources.amazon.repo",
         .label = "nile Download Repository",
         .type = Type::String,
         .default_value = std::string(runner_sources::kNileRepo),
         .doc = "The GitHub repository (\"owner/repo\") that nile is downloaded from."});

  s.Add({.key = "runner_sources.amazon.asset_pattern",
         .label = "nile File Pattern",
         .type = Type::String,
         .default_value = std::string(runner_sources::kNileAssetPattern),
         .doc = "A glob pattern that picks which file of a release to download."});

  // --- Launching -------------------------------------------------------------
  s.Section("Launching");

  s.Add({.key = "launch.stop_timeout_s",
         .label = "Stop Timeout (seconds)",
         .type = Type::Int,
         .default_value = 10,
         .doc = "How long a game has to quit after you stop it before Mira force-kills it. This "
                "applies to the whole chain of launcher processes, not only the game.",
         .constraint = Range(0, 600)});

  s.Add({.key = "launch.log_max_mb",
         .label = "Maximum Log Size (MB)",
         .type = Type::Int,
         .default_value = 64,
         .scope = Scope::PerGame,
         .doc = "The largest a game's log file may be. When a new session starts, the previous "
                "log is deleted if it is over this size, so each game uses at most about twice "
                "this much disk.",
         .constraint = Range(1, 1024)});

  s.Add({.key = "launch.env",
         .label = "Environment Variables",
         .type = Type::StringArray,
         .default_value = json::array(),
         .scope = Scope::PerGame,
         .doc = "Environment variables (KEY=VALUE) set for every launch, for example "
                "\"MANGOHUD=1\" or \"DXVK_ASYNC=1\". A game's own Environment field wins over "
                "these. They do not apply to Steam games launched through the Steam client.",
         .game_doc = "Environment variables (KEY=VALUE) for this game, for example \"MANGOHUD=1\". "
                     "This list replaces the global one for this game. The game's own Environment "
                     "field wins over it."});

  s.Add({.key = "launch.pre_script",
         .label = "Pre-Launch Script",
         .type = Type::String,
         .default_value = "",
         .scope = Scope::PerGame,
         .doc = "A shell command that runs before a game starts, for example to mount a network "
                "drive or set the CPU governor. The game waits for it. If it fails, the launch is "
                "cancelled and shows the script's output. Empty disables it."});

  s.Add({.key = "launch.pre_timeout_s",
         .label = "Pre-Launch Timeout (seconds)",
         .type = Type::Int,
         .default_value = 30,
         .scope = Scope::PerGame,
         .doc = "How long the pre-launch script may run before the launch is cancelled. This stops "
                "a stuck script from blocking the game.",
         .constraint = Range(1, 600)});

  s.Add({.key = "launch.post_script",
         .label = "Post-Launch Script",
         .type = Type::String,
         .default_value = "",
         .scope = Scope::PerGame,
         .doc = "A shell command that runs after a game exits, however it ended, for example to "
                "undo a CPU governor change. It runs in the background and its result does not "
                "affect playtime or crash tracking. For Steam games launched through the Steam "
                "client, it only runs when process tracking is on."});

  s.Add({.key = "launch.post_timeout_s",
         .label = "Post-Launch Timeout (seconds)",
         .type = Type::Int,
         .default_value = 30,
         .scope = Scope::PerGame,
         .doc = "How long the post-launch script may run before Mira force-kills it.",
         .constraint = Range(1, 600)});

  s.Add({.key = "launch.gamemode",
         .label = "Use GameMode",
         .type = Type::Bool,
         .default_value = true,
         .scope = Scope::PerGame,
         .doc = "Turn on Feral GameMode, which boosts performance, while a game runs. This works "
                "without adding gamemoderun as a command wrapper. Does nothing if GameMode is not "
                "installed or not running."});

  // --- Installers ------------------------------------------------------------
  s.Section("Installers");

  s.Add({.key = "scan.auto_run_installers",
         .label = "Run Installers Automatically",
         .type = Type::Bool,
         .default_value = false,
         .doc = "Run an installer (Inno Setup, NSIS or MSI) silently as soon as Mira finds it. "
                "When off, installers wait until you run them yourself."});

  s.Add({.key = "install.retry_failed",
         .label = "Retry Failed Installs",
         .type = Type::Bool,
         .default_value = false,
         .doc = "Retry a failed automatic install on every scan, instead of waiting for you to "
                "run it again."});

  s.Add({.key = "install.runner",
         .label = "Installer Runner",
         .type = Type::String,
         .default_value = "",
         .doc = "The runner that runs installers, for example \"proton:GE-Proton11-7\" or "
                "\"wine:latest\". Empty uses the default Wine/Proton runner. The game keeps this "
                "runner afterward, because its prefix was created with it."});

  s.Add({.key = "install.timeout_s",
         .label = "Installer Timeout (seconds)",
         .type = Type::Int,
         .default_value = 0,
         .doc = "Stop a silent installer after this many seconds. 0 means no limit.",
         .constraint = Range(0, 86400)});

  s.Add({.key = "install.show_progress",
         .label = "Show Installer Progress",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Show the progress window of Inno Setup installers during a silent install. It "
                "still asks no questions. NSIS installers have no such window."});

  s.Divider();

  s.Add({.key = "install.detect_dirs",
         .label = "Install Detection Folders",
         .type = Type::StringArray,
         .default_value = json::array({"Program Files", "Program Files (x86)", "GOG Games", "Games"}),
         .doc = "Folders inside the prefix's drive_c where Mira looks for the installed game, when "
                "the installer did not use the game folder."});

  s.Add({.key = "install.inno_args",
         .label = "Inno Setup Arguments",
         .type = Type::String,
         .default_value = "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-",
         .doc = "Command line arguments for a silent Inno Setup install, such as GOG offline "
                "installers. Mira adds /DIR itself."});

  s.Add({.key = "install.nsis_args",
         .label = "NSIS Arguments",
         .type = Type::String,
         .default_value = "/S",
         .doc = "Command line arguments for a silent NSIS install. Mira adds /D itself."});

  s.Add({.key = "install.msi_args",
         .label = "MSI Arguments",
         .type = Type::String,
         .default_value = "/qn",
         .doc = "Command line arguments for a silent MSI install with msiexec. Mira adds TARGETDIR "
                "itself."});

  // --- Store launchers ------------------------------------------------------
  s.Section("Store Launchers");

  for (const auto& [id, name] : {std::pair{"battlenet", "Battle.net"}, std::pair{"ubisoft", "Ubisoft Connect"},
                                 std::pair{"ea", "EA app"}}) {
    s.Add({.key = std::format("{}.enabled", id),
           .label = std::format("Enable {}", name),
           .type = Type::Bool,
           .default_value = true,
           .doc = std::format("Show {} in the sidebar and import its games. Removing the source turns "
                              "this off.",
                              name)});
  }

  s.Add({.key = "launchers.auto_import",
         .label = "Import Launcher Games on Scan",
         .type = Type::Bool,
         .default_value = true,
         .doc = "On every scan, import games installed through Battle.net, Ubisoft Connect or the "
                "EA app."});

  s.Add({.key = "launchers.runner",
         .label = "Launcher Runner",
         .type = Type::String,
         .default_value = "",
         .doc = "The runner for a store launcher's prefix, for example \"proton:GE-Proton11-7\". "
                "Empty uses the default Wine/Proton runner. Games the launcher installs use the "
                "same runner.",
         .constraint = OptionalRunnerRef(),
         .is_runner_ref = true});

  s.Add({.key = "launchers.detect_timeout_s",
         .label = "Launcher Game Start Timeout (seconds)",
         .type = Type::Int,
         .default_value = 300,
         .doc = "How long to wait for a launcher to start a game, which may involve updates or a "
                "login, before Mira stops tracking it.",
         .constraint = Range(10, 3600)});

  s.Add({.key = "launchers.umu_lookup",
         .label = "Look Up umu Game IDs",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Look up each newly imported launcher game at umu.openwinecomponents.org so its "
                "protonfixes apply. Only the store name and the game's store id are sent."});

  s.Divider();

  s.Add({.key = "launchers.ubisoft.disable_overlay",
         .label = "Disable Ubisoft Overlay",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Turn off the Ubisoft Connect overlay when installing it. The overlay often breaks "
                "games under Wine."});

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
         .doc = "The rules that score candidate executables to find the game's main executable, "
                "applied in order. Remove a rule to turn it off."});

  s.Add({.key = "detect.name_match_bonus",
         .label = "Name Match Bonus",
         .type = Type::Double,
         .default_value = 3.0,
         .doc = "Score added when an executable's name is similar to its folder's name.",
         .constraint = Range(0.0, 100.0)});

  s.Add({.key = "detect.depth_penalty",
         .label = "Depth Penalty",
         .type = Type::Double,
         .default_value = 0.5,
         .doc = "Score subtracted for each folder level, so executables near the top are preferred.",
         .constraint = Range(0.0, 100.0)});

  s.Add({.key = "detect.low_confidence_threshold",
         .label = "Low Confidence Threshold",
         .type = Type::Double,
         .default_value = 0.5,
         .doc = "Games detected with less confidence than this are flagged for review. They are "
                "still set up and can still be launched.",
         .constraint = Range(0.0, 1.0)});

  s.Add({.key = "detect.deny_name_patterns",
         .label = "Non-Game Name Patterns",
         .type = Type::StringArray,
         .default_value = json(known_exe_patterns::kDeny),
         .doc = "Executables matching these glob patterns are heavily penalized, because they are "
                "usually helpers or uninstallers, not the game."});

  s.Add({.key = "detect.installer_name_patterns",
         .label = "Installer Name Patterns",
         .type = Type::StringArray,
         .default_value = json(known_exe_patterns::kInstaller),
         .doc = "Glob patterns for installer names. A match is treated as an installer, not the "
                "game, and is marked as needing install. It must also meet the minimum size below, "
                "either by itself or through a larger file in the same folder."});

  s.Add({.key = "detect.installer_min_size_mb",
         .label = "Minimum Installer Size (MB)",
         .type = Type::Int,
         .default_value = 50,
         .doc = "The minimum size, of the file itself or of a file beside it, for a name match to "
                "count as an installer. This keeps small helper programs with installer-like names "
                "from being misjudged.",
         .constraint = Range(0, 1'000'000)});

  // --- Scanning --------------------------------------------------------------
  s.Section("Scanning");

  s.Add({.key = "scan.tag_by_root",
         .label = "Tag Games by Library Folder",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Tag each new game with the name of the library folder it was found in. This helps "
                "you filter when you have several library folders. Existing games are not "
                "retagged."});

  s.Add({.key = "scan.debounce_ms",
         .label = "Scan Delay (ms)",
         .type = Type::Int,
         .default_value = 3000,
         .doc = "How long a new folder must stay unchanged before Mira scans it. Raise this if "
                "games arrive over a slow network share.",
         .constraint = Range(0, 600000)});

  s.Add({.key = "scan.max_depth",
         .label = "Maximum Scan Depth",
         .type = Type::Int,
         .default_value = 4,
         .doc = "How many folder levels deep to search a game folder for executables.",
         .constraint = Range(1, 16)});

  s.Add({.key = "scan.auto_extract_archives",
         .label = "Auto-Extract Archives",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Extract archives (.zip, .rar, .tar, .7z and split archives) dropped into a "
                "library folder, into a folder of the same name, then delete the archive. Needs "
                "7-Zip (7z or 7zz) installed."});

  s.Add({.key = "scan.ignore_globs",
         .label = "Ignored Paths",
         .type = Type::StringArray,
         .default_value = json::array({".*", "*/Redist*", "*/DirectX*", "*/_CommonRedist*", "*/DotNet*"}),
         .doc = "Paths matching these glob patterns are never treated as games."});

  // --- Desktop Entries -------------------------------------------------------
  s.Section("Desktop Entries");

  s.Add({.key = "desktop_entries.enabled",
         .label = "Enable Desktop Entries",
         .type = Type::Bool,
         .default_value = true,
         .scope = Scope::PerGame,
         .doc = "Add each ready game to your application menu, so you can launch it like any "
                "other app. Turn this off to keep Mira's games out of your menu.",
         .game_doc = "Add this game to your application menu."});

  s.Add({.key = "desktop_entries.directory",
         .label = "Desktop Entries Folder",
         .type = Type::String,
         .default_value = "~/.local/share/applications",
         .doc = "The folder where desktop entries are written. Mira only changes the files it "
                "created itself."});

  s.Add({.key = "desktop_entries.categories",
         .label = "Menu Categories",
         .type = Type::String,
         .default_value = "Game;",
         .scope = Scope::PerGame,
         .doc = "The desktop menu categories for new entries, separated by semicolons. They decide "
                "where the entries appear in your menu."});

  s.Add({.key = "desktop_entries.exec_mode",
         .label = "Menu Entry Launch Method",
         .type = Type::String,
         .default_value = "cli",
         .scope = Scope::PerGame,
         .doc = "What a menu entry runs. \"cli\" runs \"mira launch\", which needs the Mira daemon "
                "to be running already and shows an error if it is not. \"frontend\" opens Mira, "
                "which starts the daemon itself. Either way the game launches through Mira, so "
                "playtime is recorded.",
         .constraint = OneOf({"cli", "frontend"})});

  s.Divider();

  s.Add({.key = "desktop_import.enabled",
         .label = "Allow Importing from the Application Menu",
         .type = Type::Bool,
         .default_value = true,
         .doc = "Let Mira read the apps in your application menu so you can add them as games. "
                "This is how Flatpak apps are added. Nothing is added until you choose which "
                "ones."});

  s.Add({.key = "desktop_import.extra_dirs",
         .label = "Extra Application Folders",
         .type = Type::StringArray,
         .default_value = json::array(),
         .doc = "Extra folders to search for desktop entries, in addition to the standard "
                "application folders and the Flatpak export folders."});

  // --- Advanced --------------------------------------------------------------
  s.Section("Advanced");

  s.Add({.key = "socket_path",
         .label = "Socket Path",
         .type = Type::String,
         .default_value = "$XDG_RUNTIME_DIR/mira/mirad.sock",
         .doc = "The socket the daemon listens on. The frontend and the command line tool must "
                "use the same path.",
         .constraint = NonEmptyString()});

  s.Add({.key = "log.level",
         .label = "Log Level",
         .type = Type::String,
         .default_value = "info",
         .doc = "How much the daemon logs: debug, info, warn or error.",
         .constraint = OneOf({"debug", "info", "warn", "error"})});
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
    case Scope::GameOnly: return "game_only";
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
