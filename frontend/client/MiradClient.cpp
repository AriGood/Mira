#include "MiradClient.h"

#include <json.hpp>

#include <string_view>
#include <utility>

#include "Async.h"
#include "JsonMapping.h"
#include "Transport.h"

namespace mira_gui {
namespace {

using nlohmann::json;

// Each of these is the blocking half of one endpoint, run on a worker thread
// by async::Run below. They are written as "ask transport, shape the reply"
// and nothing else — no socket, no timeouts, no error unwrapping.

HealthStatus GetHealthSync() {
  HealthStatus status;
  const transport::Reply reply = transport::Get("/v1/health");
  status.reachable = reply.ok;
  status.detail = reply.ok ? reply.body.value("status", std::string("ok")) : reply.error;
  return status;
}

GamesResult GetGamesSync(const std::string& status_filter, const std::string& tag_filter) {
  GamesResult result;
  std::string path = "/v1/games";
  std::string separator = "?";
  if (!status_filter.empty()) {
    path += separator + "status=" + status_filter;
    separator = "&";
  }
  if (!tag_filter.empty()) {
    path += separator + "tag=" + tag_filter;
  }
  const transport::Reply reply = transport::Get(path);
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_array()) {
    result.error = transport::UnexpectedResponse("GET /v1/games");
    return result;
  }

  result.ok = true;
  for (const json& entry : reply.body) result.games.push_back(mapping::ToGameSummary(entry));
  return result;
}

DeleteResult DeleteGameSync(const std::string& id, bool delete_files, bool delete_prefix,
                            bool delete_metadata) {
  // Every flag is opt-in server-side too: the bare DELETE never touches
  // disk, so an omitted param and "false" mean the same thing.
  std::string path = "/v1/games/" + id;
  std::string separator = "?";
  if (delete_files) {
    path += separator + "delete_files=true";
    separator = "&";
  }
  if (delete_prefix) {
    path += separator + "delete_prefix=true";
    separator = "&";
  }
  if (delete_metadata) path += separator + "delete_metadata=true";

  const transport::Reply reply = transport::Delete(path);
  return {reply.ok, reply.error};
}

LaunchResult LaunchGameSync(const std::string& id) {
  const transport::Reply reply = transport::Post("/v1/games/" + id + "/launch");
  // mirad answers `tracked` directly (docs/api.md): whether game.state
  // events are coming for this launch.
  const bool tracked = !reply.body.is_object() ||
                       reply.body.value("tracked",
                                        reply.body.value("status", std::string()) !=
                                            "launched_via_steam");
  return {reply.ok, reply.error, tracked};
}

StopResult StopGameSync(const std::string& id) {
  const transport::Reply reply = transport::Post("/v1/games/" + id + "/stop");
  return {reply.ok, reply.error};
}

ScanResult ScanLibrarySync() {
  ScanResult result;
  const transport::Reply reply =
      transport::Post("/v1/library/scan", {.read_timeout = std::chrono::seconds(30)});
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }

  result.ok = true;
  result.added = reply.body.value("added", 0);
  result.missing = reply.body.value("missing", 0);
  result.restored = reply.body.value("restored", 0);
  return result;
}

GameDetailResult GetGameSync(const std::string& id) {
  GameDetailResult result;
  const transport::Reply reply = transport::Get("/v1/games/" + id);
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_object()) {
    result.error = transport::UnexpectedResponse("GET /v1/games/" + id);
    return result;
  }

  result.ok = true;
  result.game = mapping::ToGameDetail(reply.body);
  return result;
}

PatchGameResult PatchGameSync(const std::string& id, const GamePatch& patch) {
  PatchGameResult result;
  json body = json::object();
  if (patch.name) body["name"] = *patch.name;
  if (patch.exe_path) body["exe_path"] = *patch.exe_path;
  if (patch.args) body["args"] = *patch.args;
  if (patch.working_dir) body["working_dir"] = *patch.working_dir;
  if (patch.runner_ref) body["runner_ref"] = *patch.runner_ref;
  if (patch.data_dir) body["data_dir"] = *patch.data_dir;
  if (patch.tags) body["tags"] = *patch.tags;

  const auto parse_object = [&](const std::string& text, const char* field,
                                const char* message) -> bool {
    const json parsed = json::parse(text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
      result.error = message;
      return false;
    }
    body[field] = parsed;
    return true;
  };
  if (patch.runner_config_json &&
      !parse_object(*patch.runner_config_json, "runner_config",
                    "Runner config must be a JSON object, e.g. {}")) {
    return result;
  }
  if (patch.env_json && !parse_object(*patch.env_json, "env",
                                      "Environment must be a JSON object of strings, e.g. {}")) {
    return result;
  }

  const transport::Reply reply = transport::Patch("/v1/games/" + id, body);
  return {reply.ok, reply.error};
}

ConfigSchemaResult GetConfigSchemaSync() {
  ConfigSchemaResult result;
  const transport::Reply reply = transport::Get("/v1/config/schema");
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_array()) {
    result.error = transport::UnexpectedResponse("GET /v1/config/schema");
    return result;
  }

  result.ok = true;
  for (const json& entry : reply.body) {
    ConfigSchemaEntry e;
    e.key = entry.value("key", std::string());
    e.type = entry.value("type", std::string());
    e.tier = entry.value("tier", std::string());
    e.doc = entry.value("doc", std::string());
    e.category = entry.value("category", std::string());
    e.is_secret = entry.value("is_secret", false);
    e.is_runner_ref = entry.value("is_runner_ref", false);
    if (entry.contains("default")) e.default_display = mapping::ToDisplayString(entry["default"]);
    if (entry.contains("one_of") && entry["one_of"].is_array()) {
      for (const json& option : entry["one_of"]) {
        if (option.is_string()) e.one_of.push_back(option.get<std::string>());
      }
    }
    if (entry.contains("minimum") && entry["minimum"].is_number()) {
      e.minimum = entry["minimum"].get<double>();
    }
    if (entry.contains("maximum") && entry["maximum"].is_number()) {
      e.maximum = entry["maximum"].get<double>();
    }
    result.entries.push_back(std::move(e));
  }
  return result;
}

ConfigResult GetConfigSync() {
  ConfigResult result;
  const transport::Reply reply = transport::Get("/v1/config");
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_object()) {
    result.error = transport::UnexpectedResponse("GET /v1/config");
    return result;
  }

  result.ok = true;
  mapping::FlattenConfig(reply.body, "", result.values);
  return result;
}

PatchConfigResult PatchConfigSync(const std::vector<ConfigEdit>& edits) {
  json body = json::object();
  for (const ConfigEdit& edit : edits) {
    mapping::AssignDottedKey(body, edit.key, mapping::TypedValueFromText(edit.type, edit.value));
  }
  const transport::Reply reply = transport::Patch("/v1/config", body);
  return {reply.ok, reply.error};
}

PatchConfigResult ResetConfigKeySync(const std::string& key) {
  const transport::Reply reply = transport::Post("/v1/config/reset?key=" + key);
  return {reply.ok, reply.error};
}

RunnersResult GetRunnersSync() {
  RunnersResult result;
  const transport::Reply reply = transport::Get("/v1/runners");
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_array()) {
    result.error = transport::UnexpectedResponse("GET /v1/runners");
    return result;
  }

  result.ok = true;
  for (const json& entry : reply.body) {
    RunnerInfo runner;
    runner.kind = entry.value("kind", std::string());
    runner.name = entry.value("name", std::string());
    runner.version = entry.value("version", std::string());
    runner.reference = entry.value("reference", std::string());
    result.runners.push_back(std::move(runner));
  }
  return result;
}

GameConfigResult GetGameConfigSync(const std::string& id) {
  GameConfigResult result;
  const transport::Reply reply = transport::Get("/v1/games/" + id + "/config");
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_object()) {
    result.error = transport::UnexpectedResponse("GET /v1/games/" + id + "/config");
    return result;
  }

  result.ok = true;
  // Schema::Entries() order.
  for (const auto& [key, entry] : reply.body.items()) {
    GameConfigEntry e;
    e.key = key;
    e.value_display = mapping::ToDisplayString(entry.value("value", json()));
    e.layer = entry.value("layer", std::string());
    e.overridable = entry.value("overridable", false);
    result.entries.push_back(std::move(e));
  }
  return result;
}

FrontendPrefsResult GetFrontendPrefsSync() {
  FrontendPrefsResult result;
  const transport::Reply reply = transport::Get("/v1/config");
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }

  result.ok = true;
  // Frontend falls back to defaults rather than refusing to start on absense
  // or wrong kind after a hand-edit.
  const json table = reply.body.value("frontend", json::object());
  if (!table.is_object()) return result;

  const auto read_int = [&table](const char* key, std::optional<int>& out) {
    if (table.contains(key) && table[key].is_number_integer()) out = table[key].get<int>();
  };
  read_int("window_width", result.prefs.window_width);
  read_int("window_height", result.prefs.window_height);
  read_int("tile_width", result.prefs.tile_width);
  read_int("sidebar_width", result.prefs.sidebar_width);
  const auto read_string = [&table](const char* key, std::optional<std::string>& out) {
    if (table.contains(key) && table[key].is_string()) out = table[key].get<std::string>();
  };
  const auto read_bool = [&table](const char* key, std::optional<bool>& out) {
    if (table.contains(key) && table[key].is_boolean()) out = table[key].get<bool>();
  };
  read_string("library_filter", result.prefs.library_filter);
  read_string("sort_by", result.prefs.sort_by);
  read_bool("sort_descending", result.prefs.sort_descending);
  read_bool("scan_on_startup", result.prefs.scan_on_startup);
  read_int("notification_timeout_s", result.prefs.notification_timeout_s);
  read_string("theme", result.prefs.theme);
  read_bool("game_settings_in_sidebar", result.prefs.game_settings_in_sidebar);
  read_int("tile_spacing", result.prefs.tile_spacing);
  read_int("grid_margin", result.prefs.grid_margin);
  read_int("tile_radius", result.prefs.tile_radius);
  read_int("panel_radius", result.prefs.panel_radius);
  read_int("control_radius", result.prefs.control_radius);
  if (table.contains("shortcuts") && table["shortcuts"].is_object()) {
    std::map<std::string, std::string> overrides;
    for (const auto& [id, keys] : table["shortcuts"].items()) {
      if (keys.is_string()) overrides[id] = keys.get<std::string>();
    }
    result.prefs.shortcut_overrides = std::move(overrides);
  }
  return result;
}

PatchConfigResult SaveFrontendPrefsSync(const FrontendPrefs& prefs) {
  json table = json::object();
  if (prefs.window_width) table["window_width"] = *prefs.window_width;
  if (prefs.window_height) table["window_height"] = *prefs.window_height;
  if (prefs.tile_width) table["tile_width"] = *prefs.tile_width;
  if (prefs.sidebar_width) table["sidebar_width"] = *prefs.sidebar_width;
  if (prefs.library_filter) table["library_filter"] = *prefs.library_filter;
  if (prefs.sort_by) table["sort_by"] = *prefs.sort_by;
  if (prefs.sort_descending) table["sort_descending"] = *prefs.sort_descending;
  if (prefs.scan_on_startup) table["scan_on_startup"] = *prefs.scan_on_startup;
  if (prefs.notification_timeout_s) {
    table["notification_timeout_s"] = *prefs.notification_timeout_s;
  }
  if (prefs.theme) table["theme"] = *prefs.theme;
  if (prefs.game_settings_in_sidebar) {
    table["game_settings_in_sidebar"] = *prefs.game_settings_in_sidebar;
  }
  if (prefs.tile_spacing) table["tile_spacing"] = *prefs.tile_spacing;
  if (prefs.grid_margin) table["grid_margin"] = *prefs.grid_margin;
  if (prefs.tile_radius) table["tile_radius"] = *prefs.tile_radius;
  if (prefs.panel_radius) table["panel_radius"] = *prefs.panel_radius;
  if (prefs.control_radius) table["control_radius"] = *prefs.control_radius;
  if (prefs.shortcut_overrides) {
    json shortcuts = json::object();
    for (const auto& [id, keys] : *prefs.shortcut_overrides) shortcuts[id] = keys;
    table["shortcuts"] = shortcuts;
  }

  // Short, because SaveFrontendPrefsBlocking runs this on the UI thread
  // while a window is closing.
  const transport::Reply reply = transport::Patch("/v1/config", json{{"frontend", table}},
                                                  {.read_timeout = std::chrono::seconds(2)});
  return {reply.ok, reply.error};
}

ArtworkResult GetArtworkSync(const std::string& id, const std::string& slot) {
  ArtworkResult result;
  const transport::Blob blob =
      transport::GetBinary("/v1/games/" + id + "/artwork?type=" + slot);
  if (blob.status == 404) {
    result.missing = true;
    return result;
  }
  if (!blob.ok) {
    result.error = blob.error;
    return result;
  }
  result.ok = true;
  result.bytes = blob.bytes;
  result.content_type = blob.content_type;
  return result;
}

GameMetadataResult GetMetadataSync(const std::string& id) {
  GameMetadataResult result;
  const transport::Reply reply = transport::Get("/v1/games/" + id + "/metadata");
  if (reply.status == 404) {
    result.missing = true;
    return result;
  }
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }

  result.ok = true;
  GameMetadata& out = result.metadata;
  out.source = reply.body.value("source", std::string());

  const auto strings = [](const json& array) {
    std::vector<std::string> values;
    if (!array.is_array()) return values;
    for (const json& item : array) {
      if (item.is_string()) values.push_back(item.get<std::string>());
    }
    return values;
  };

  // Every block is optional: which ones mirad cached depends on the source,
  // and on what that source had for this game.
  if (reply.body.contains("steam") && reply.body["steam"].is_object()) {
    const json& steam = reply.body["steam"];
    out.description = steam.value("short_description", std::string());
    out.release_date = steam.value("release_date", std::string());
    out.developers = strings(steam.value("developers", json::array()));
    out.genres = strings(steam.value("genres", json::array()));
    out.price = steam.value("price", std::string());
    out.metacritic_score = steam.value("metacritic_score", 0);
    out.website = steam.value("website", std::string());
    if (steam.contains("pc_requirements") && steam["pc_requirements"].is_object()) {
      const json& requirements = steam["pc_requirements"];
      out.requirements_min = requirements.value("minimum", std::string());
      out.requirements_rec = requirements.value("recommended", std::string());
    }
    if (steam.contains("dlc") && steam["dlc"].is_array()) {
      for (const json& item : steam["dlc"]) {
        if (item.is_number_integer()) out.dlc_ids.push_back(item.get<std::int64_t>());
      }
    }
    out.content_descriptors = strings(steam.value("content_descriptors", json::array()));
    out.achievements_total = steam.value("achievements_total", 0);
    out.screenshots = strings(steam.value("screenshots", json::array()));
    out.trailers = strings(steam.value("movies", json::array()));
  }
  if (reply.body.contains("steam_reviews") && reply.body["steam_reviews"].is_object()) {
    const json& reviews = reply.body["steam_reviews"];
    out.review_summary = reviews.value("score_description", std::string());
    out.review_total = reviews.value("total_reviews", 0);
  }
  if (reply.body.contains("protondb") && reply.body["protondb"].is_object()) {
    out.protondb_tier = reply.body["protondb"].value("tier", std::string());
  }
  // "artwork" is the cover slot under its pre-`hero` name — see docs/api.md.
  for (const char* key : {"artwork", "hero", "capsule", "header", "logo", "icon"}) {
    if (!reply.body.contains(key) || !reply.body[key].is_object()) continue;
    out.art_slots.push_back(std::string(key) == "artwork" ? "cover" : key);
  }

  const auto candidates = [&](const char* slot) {
    std::vector<ArtCandidate> list;
    if (!reply.body.contains("art_candidates") || !reply.body["art_candidates"].is_object() ||
        !reply.body["art_candidates"].contains(slot)) {
      return list;
    }
    for (const json& item : reply.body["art_candidates"][slot]) {
      ArtCandidate candidate;
      candidate.id = item.value("id", std::int64_t{0});
      candidate.width = item.value("width", 0);
      candidate.height = item.value("height", 0);
      candidate.style = item.value("style", std::string());
      list.push_back(candidate);
    }
    return list;
  };
  out.cover_candidates = candidates("cover");
  out.hero_candidates = candidates("hero");

  const auto active_id = [&](const char* key) -> std::optional<std::int64_t> {
    if (!reply.body.contains(key) || !reply.body[key].is_object() ||
        !reply.body[key].contains("candidate_id")) {
      return std::nullopt;
    }
    return reply.body[key].value("candidate_id", std::int64_t{0});
  };
  out.cover_active_candidate_id = active_id("artwork");
  out.hero_active_candidate_id = active_id("hero");
  return result;
}

MetadataRefreshResult RefreshMetadataSync(const std::string& id, bool announce) {
  const transport::Reply reply =
      transport::Post("/v1/games/" + id + "/metadata/refresh?announce=" + (announce ? "1" : "0"));
  return {reply.ok, reply.error};
}

ArtworkSelectResult SelectArtworkSync(const std::string& id, const std::string& slot,
                                      std::int64_t candidate_id) {
  const transport::Reply reply = transport::PostJson(
      "/v1/games/" + id + "/artwork?type=" + slot, json{{"candidate_id", candidate_id}});
  return {reply.ok, reply.error};
}

RefreshMissingArtworkResult RefreshMissingArtworkSync() {
  RefreshMissingArtworkResult result;
  const transport::Reply reply = transport::Post("/v1/games/metadata/refresh-missing");
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  result.ok = true;
  result.count = reply.body.value("count", 0);
  return result;
}

RunnerCatalogResult GetRunnerCatalogSync(const std::string& kind) {
  RunnerCatalogResult result;
  // The only call in this client that leaves the machine (GitHub releases),
  // so the default timeout is nowhere near enough.
  const transport::Reply reply = transport::Get("/v1/runners/catalog?kind=" + kind,
                                                {.read_timeout = std::chrono::seconds(30)});
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_array()) {
    result.error = transport::UnexpectedResponse("GET /v1/runners/catalog");
    return result;
  }

  result.ok = true;
  for (const json& entry : reply.body) {
    RunnerRelease release;
    release.tag = entry.value("tag", std::string());
    release.asset_name = entry.value("asset_name", std::string());
    release.size_bytes = entry.value("size_bytes", std::int64_t{0});
    release.published_at = entry.value("published_at", std::string());
    release.has_checksum = entry.value("has_checksum", false);
    result.releases.push_back(std::move(release));
  }
  return result;
}

RunnerDownloadResult DownloadRunnerSync(const std::string& kind, const std::string& tag) {
  const transport::Reply reply =
      transport::PostJson("/v1/runners/download", json{{"kind", kind}, {"tag", tag}});
  return {reply.ok, reply.error};
}

LutrisImportResult ImportLutrisSync() {
  LutrisImportResult result;
  // Same 30s as the Steam scan: this one reads Lutris's sqlite database
  // through the sqlite3 CLI and a yaml file per game.
  const transport::Reply reply =
      transport::Post("/v1/lutris/import", {.read_timeout = std::chrono::seconds(30)});
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  result.ok = true;
  result.added = reply.body.value("added", 0);
  result.updated = reply.body.value("updated", 0);
  result.skipped = reply.body.value("skipped", 0);
  return result;
}

SteamScanResult ScanSteamSync() {
  SteamScanResult result;
  const transport::Reply reply =
      transport::Post("/v1/steam/scan", {.read_timeout = std::chrono::seconds(30)});
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  result.ok = true;
  result.added = reply.body.value("added", 0);
  result.updated = reply.body.value("updated", 0);
  return result;
}

RunInPrefixResult RunInPrefixSync(const std::string& id, const std::string& exe_path,
                                  const std::string& args) {
  const transport::Reply reply = transport::PostJson(
      "/v1/games/" + id + "/run", json{{"exe_path", exe_path}, {"args", args}},
      // Provisions a prefix on demand if there isn't one yet, which is
      // genuinely slow (it's initialising Wine/Proton, see
      // docs/architecture.md).
      {.read_timeout = std::chrono::seconds(120)});
  return {reply.ok, reply.error};
}

FinishInstallResult FinishInstallSync(const std::string& id) {
  const transport::Reply reply = transport::Post("/v1/games/" + id + "/finish-install");
  return {reply.ok, reply.error};
}

PatchGameConfigResult PatchGameConfigSync(const std::string& id,
                                          const std::vector<GameConfigEdit>& edits) {
  json body = json::object();
  for (const GameConfigEdit& edit : edits) {
    body[edit.key] =
        edit.clear ? json(nullptr) : mapping::TypedValueFromText(edit.type, edit.value);
  }
  const transport::Reply reply = transport::Patch("/v1/games/" + id + "/config", body);
  return {reply.ok, reply.error};
}

GameLogResult GetGameLogSync(const std::string& id, int lines) {
  GameLogResult result;
  const transport::Reply reply =
      transport::Get("/v1/games/" + id + "/log?lines=" + std::to_string(lines));
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_object()) {
    result.error = transport::UnexpectedResponse("GET /v1/games/" + id + "/log");
    return result;
  }

  result.ok = true;
  if (reply.body.contains("lines") && reply.body["lines"].is_array()) {
    for (const json& line : reply.body["lines"]) {
      if (line.is_string()) result.lines.push_back(line.get<std::string>());
    }
  }
  return result;
}

GameModeStatusResult GetGameModeStatusSync() {
  GameModeStatusResult result;
  const transport::Reply reply = transport::Get("/v1/gamemode/status");
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  result.ok = true;
  result.installed = reply.body.value("installed", false);
  result.daemon_running = reply.body.value("daemon_running", false);
  return result;
}

TricksResult RunWinetricksSync(const std::string& id, const std::string& verb) {
  const transport::Reply reply =
      transport::PostJson("/v1/games/" + id + "/tricks", json{{"verb", verb}});
  return {reply.ok, reply.error};
}

RunnerRemoveResult DeleteRunnerSync(const std::string& kind, const std::string& name) {
  const transport::Reply reply = transport::Delete("/v1/runners/" + kind + ":" + name);
  return {reply.ok, reply.error};
}

RunnerSchemaResult GetRunnerSchemaSync(const std::string& kind) {
  RunnerSchemaResult result;
  const transport::Reply reply = transport::Get("/v1/runners/" + kind + "/schema");
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_array()) {
    result.error = transport::UnexpectedResponse("GET /v1/runners/" + kind + "/schema");
    return result;
  }

  result.ok = true;
  for (const json& entry : reply.body) {
    RunnerSchemaEntry e;
    e.key = entry.value("key", std::string());
    e.type = entry.value("type", std::string());
    e.doc = entry.value("doc", std::string());
    result.entries.push_back(std::move(e));
  }
  return result;
}

GameDetailResult AddManualGameSync(const std::string& install_path, const std::string& exe_path,
                                   const std::string& name, const std::string& platform,
                                   bool is_installer) {
  json body{{"install_path", install_path}, {"exe_path", exe_path}, {"is_installer", is_installer}};
  if (!name.empty()) body["name"] = name;
  if (!platform.empty()) body["platform"] = platform;

  GameDetailResult result;
  const transport::Reply reply = transport::PostJson("/v1/games/manual", body);
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_object()) {
    result.error = transport::UnexpectedResponse("POST /v1/games/manual");
    return result;
  }

  result.ok = true;
  result.game = mapping::ToGameDetail(reply.body);
  return result;
}

DesktopEntryCandidatesResult GetDesktopEntryCandidatesSync() {
  DesktopEntryCandidatesResult result;
  const transport::Reply reply = transport::Get("/v1/desktop-entries/candidates");
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_array()) {
    result.error = transport::UnexpectedResponse("GET /v1/desktop-entries/candidates");
    return result;
  }

  result.ok = true;
  for (const json& entry : reply.body) {
    DesktopEntryCandidate c;
    c.id = entry.value("id", std::string());
    c.name = entry.value("name", std::string());
    c.icon = entry.value("icon", std::string());
    result.candidates.push_back(std::move(c));
  }
  return result;
}

DesktopEntryImportResult ImportDesktopEntriesSync(const std::vector<std::string>& ids) {
  DesktopEntryImportResult result;
  const transport::Reply reply =
      transport::PostJson("/v1/desktop-entries/import", json{{"ids", ids}});
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  result.ok = true;
  result.added = reply.body.value("added", 0);
  result.updated = reply.body.value("updated", 0);
  return result;
}

DesktopEntrySyncResult SyncDesktopEntriesSync() {
  const transport::Reply reply = transport::Post("/v1/desktop-entries/sync");
  return {reply.ok, reply.error};
}

}  // namespace

std::string MiradClient::ResolveSocketPath() { return transport::SocketPath(); }

void MiradClient::CheckHealthAsync(QObject* context, std::function<void(HealthStatus)> callback) {
  async::Run(context, [] { return GetHealthSync(); }, std::move(callback));
}

void MiradClient::ListGamesAsync(QObject* context, std::function<void(GamesResult)> callback,
                                 const std::string& status_filter, const std::string& tag_filter) {
  async::Run(context, [status_filter, tag_filter] { return GetGamesSync(status_filter, tag_filter); },
             std::move(callback));
}

void MiradClient::DeleteGameAsync(QObject* context, const std::string& id, bool delete_files,
                                  bool delete_prefix, bool delete_metadata,
                                  std::function<void(DeleteResult)> callback) {
  async::Run(
      context, [id, delete_files, delete_prefix, delete_metadata] {
        return DeleteGameSync(id, delete_files, delete_prefix, delete_metadata);
      },
      std::move(callback));
}

void MiradClient::LaunchGameAsync(QObject* context, const std::string& id,
                                  std::function<void(LaunchResult)> callback) {
  async::Run(context, [id] { return LaunchGameSync(id); }, std::move(callback));
}

void MiradClient::StopGameAsync(QObject* context, const std::string& id,
                                std::function<void(StopResult)> callback) {
  async::Run(context, [id] { return StopGameSync(id); }, std::move(callback));
}

void MiradClient::ScanLibraryAsync(QObject* context, std::function<void(ScanResult)> callback) {
  async::Run(context, [] { return ScanLibrarySync(); }, std::move(callback));
}

void MiradClient::GetGameAsync(QObject* context, const std::string& id,
                               std::function<void(GameDetailResult)> callback) {
  async::Run(context, [id] { return GetGameSync(id); }, std::move(callback));
}

void MiradClient::PatchGameAsync(QObject* context, const std::string& id, const GamePatch& patch,
                                 std::function<void(PatchGameResult)> callback) {
  async::Run(context, [id, patch] { return PatchGameSync(id, patch); }, std::move(callback));
}

void MiradClient::GetConfigSchemaAsync(QObject* context,
                                       std::function<void(ConfigSchemaResult)> callback) {
  async::Run(context, [] { return GetConfigSchemaSync(); }, std::move(callback));
}

void MiradClient::GetConfigAsync(QObject* context, std::function<void(ConfigResult)> callback) {
  async::Run(context, [] { return GetConfigSync(); }, std::move(callback));
}

void MiradClient::PatchConfigAsync(QObject* context, const std::vector<ConfigEdit>& edits,
                                   std::function<void(PatchConfigResult)> callback) {
  async::Run(context, [edits] { return PatchConfigSync(edits); }, std::move(callback));
}

void MiradClient::ResetConfigKeyAsync(QObject* context, const std::string& key,
                                      std::function<void(PatchConfigResult)> callback) {
  async::Run(context, [key] { return ResetConfigKeySync(key); }, std::move(callback));
}

void MiradClient::ListRunnersAsync(QObject* context, std::function<void(RunnersResult)> callback) {
  async::Run(context, [] { return GetRunnersSync(); }, std::move(callback));
}

void MiradClient::GetFrontendPrefsAsync(QObject* context,
                                        std::function<void(FrontendPrefsResult)> callback) {
  async::Run(context, [] { return GetFrontendPrefsSync(); }, std::move(callback));
}

void MiradClient::SaveFrontendPrefsAsync(QObject* context, const FrontendPrefs& prefs,
                                         std::function<void(PatchConfigResult)> callback) {
  async::Run(context, [prefs] { return SaveFrontendPrefsSync(prefs); }, std::move(callback));
}

PatchConfigResult MiradClient::SaveFrontendPrefsBlocking(const FrontendPrefs& prefs) {
  return SaveFrontendPrefsSync(prefs);
}

void MiradClient::GetArtworkAsync(QObject* context, const std::string& id,
                                  std::function<void(ArtworkResult)> callback) {
  async::Run(context, [id] { return GetArtworkSync(id, "cover"); }, std::move(callback));
}

void MiradClient::GetArtworkSlotAsync(QObject* context, const std::string& id,
                                      const std::string& slot,
                                      std::function<void(ArtworkResult)> callback) {
  async::Run(context, [id, slot] { return GetArtworkSync(id, slot); }, std::move(callback));
}

void MiradClient::GetMetadataAsync(QObject* context, const std::string& id,
                                   std::function<void(GameMetadataResult)> callback) {
  async::Run(context, [id] { return GetMetadataSync(id); }, std::move(callback));
}

void MiradClient::RefreshMetadataAsync(QObject* context, const std::string& id, bool announce,
                                       std::function<void(MetadataRefreshResult)> callback) {
  async::Run(context, [id, announce] { return RefreshMetadataSync(id, announce); }, std::move(callback));
}

void MiradClient::SelectArtworkAsync(QObject* context, const std::string& id, const std::string& slot,
                                     std::int64_t candidate_id,
                                     std::function<void(ArtworkSelectResult)> callback) {
  async::Run(context, [id, slot, candidate_id] { return SelectArtworkSync(id, slot, candidate_id); },
             std::move(callback));
}

void MiradClient::RefreshMissingArtworkAsync(QObject* context,
                                             std::function<void(RefreshMissingArtworkResult)> callback) {
  async::Run(context, [] { return RefreshMissingArtworkSync(); }, std::move(callback));
}

void MiradClient::GetRunnerCatalogAsync(QObject* context, const std::string& kind,
                                        std::function<void(RunnerCatalogResult)> callback) {
  async::Run(context, [kind] { return GetRunnerCatalogSync(kind); }, std::move(callback));
}

void MiradClient::DownloadRunnerAsync(QObject* context, const std::string& kind,
                                      const std::string& tag,
                                      std::function<void(RunnerDownloadResult)> callback) {
  async::Run(context, [kind, tag] { return DownloadRunnerSync(kind, tag); }, std::move(callback));
}

void MiradClient::ScanSteamAsync(QObject* context, std::function<void(SteamScanResult)> callback) {
  async::Run(context, [] { return ScanSteamSync(); }, std::move(callback));
}

void MiradClient::ImportLutrisAsync(QObject* context,
                                    std::function<void(LutrisImportResult)> callback) {
  async::Run(context, [] { return ImportLutrisSync(); }, std::move(callback));
}

void MiradClient::RunInPrefixAsync(QObject* context, const std::string& id,
                                   const std::string& exe_path, const std::string& args,
                                   std::function<void(RunInPrefixResult)> callback) {
  async::Run(context, [id, exe_path, args] { return RunInPrefixSync(id, exe_path, args); },
             std::move(callback));
}

void MiradClient::FinishInstallAsync(QObject* context, const std::string& id,
                                     std::function<void(FinishInstallResult)> callback) {
  async::Run(context, [id] { return FinishInstallSync(id); }, std::move(callback));
}

void MiradClient::GetGameConfigAsync(QObject* context, const std::string& id,
                                     std::function<void(GameConfigResult)> callback) {
  async::Run(context, [id] { return GetGameConfigSync(id); }, std::move(callback));
}

void MiradClient::PatchGameConfigAsync(QObject* context, const std::string& id,
                                       const std::vector<GameConfigEdit>& edits,
                                       std::function<void(PatchGameConfigResult)> callback) {
  async::Run(context, [id, edits] { return PatchGameConfigSync(id, edits); }, std::move(callback));
}

void MiradClient::GetGameLogAsync(QObject* context, const std::string& id, int lines,
                                  std::function<void(GameLogResult)> callback) {
  async::Run(context, [id, lines] { return GetGameLogSync(id, lines); }, std::move(callback));
}

void MiradClient::GetGameModeStatusAsync(QObject* context,
                                         std::function<void(GameModeStatusResult)> callback) {
  async::Run(context, [] { return GetGameModeStatusSync(); }, std::move(callback));
}

void MiradClient::RunWinetricksAsync(QObject* context, const std::string& id,
                                     const std::string& verb,
                                     std::function<void(TricksResult)> callback) {
  async::Run(context, [id, verb] { return RunWinetricksSync(id, verb); }, std::move(callback));
}

void MiradClient::DeleteRunnerAsync(QObject* context, const std::string& kind,
                                    const std::string& name,
                                    std::function<void(RunnerRemoveResult)> callback) {
  async::Run(context, [kind, name] { return DeleteRunnerSync(kind, name); }, std::move(callback));
}

void MiradClient::GetRunnerSchemaAsync(QObject* context, const std::string& kind,
                                       std::function<void(RunnerSchemaResult)> callback) {
  async::Run(context, [kind] { return GetRunnerSchemaSync(kind); }, std::move(callback));
}

void MiradClient::AddManualGameAsync(QObject* context, const std::string& install_path,
                                     const std::string& exe_path, const std::string& name,
                                     const std::string& platform, bool is_installer,
                                     std::function<void(GameDetailResult)> callback) {
  async::Run(
      context,
      [install_path, exe_path, name, platform, is_installer] {
        return AddManualGameSync(install_path, exe_path, name, platform, is_installer);
      },
      std::move(callback));
}

void MiradClient::GetDesktopEntryCandidatesAsync(
    QObject* context, std::function<void(DesktopEntryCandidatesResult)> callback) {
  async::Run(context, [] { return GetDesktopEntryCandidatesSync(); }, std::move(callback));
}

void MiradClient::ImportDesktopEntriesAsync(QObject* context, const std::vector<std::string>& ids,
                                            std::function<void(DesktopEntryImportResult)> callback) {
  async::Run(context, [ids] { return ImportDesktopEntriesSync(ids); }, std::move(callback));
}

void MiradClient::SyncDesktopEntriesAsync(QObject* context,
                                          std::function<void(DesktopEntrySyncResult)> callback) {
  async::Run(context, [] { return SyncDesktopEntriesSync(); }, std::move(callback));
}

bool MiradClient::ParseGameSummary(const std::string& data, GameSummary* out) {
  const json entry = json::parse(data, nullptr, false);
  if (entry.is_discarded() || !entry.is_object()) return false;
  // An id is what makes this a game record. Without this check any JSON
  // object at all parsed as a game with every field empty.
  if (!entry.contains("id") || !entry["id"].is_string() || entry["id"].get<std::string>().empty()) {
    return false;
  }
  *out = mapping::ToGameSummary(entry);
  return true;
}

bool MiradClient::ParseGameState(const std::string& data, GameStateEvent* out) {
  const json entry = json::parse(data, nullptr, false);
  if (entry.is_discarded() || !entry.is_object()) return false;
  out->id = entry.value("id", std::string());
  out->state = entry.value("state", std::string());
  return !out->id.empty();
}

bool MiradClient::ParseGameLaunched(const std::string& data, GameLaunchedEvent* out) {
  const json entry = json::parse(data, nullptr, false);
  if (entry.is_discarded() || !entry.is_object()) return false;
  out->id = entry.value("id", std::string());
  if (out->id.empty()) return false;
  out->tracked = entry.value("tracked", false);
  return true;
}

std::string MiradClient::ParseRemovedId(const std::string& data) {
  const json entry = json::parse(data, nullptr, false);
  if (entry.is_discarded() || !entry.is_object()) return {};
  return entry.value("id", std::string());
}

bool MiradClient::ParseMetadataEvent(const std::string& data, MetadataEvent* out) {
  const json payload = json::parse(data, nullptr, false);
  if (!payload.is_object()) return false;
  const std::string id = payload.value("id", std::string());
  if (id.empty()) return false;
  out->id = id;
  out->code = payload.value("code", std::string());
  out->error = payload.value("error", std::string());
  return true;
}

bool MiradClient::ParseArtworkSelectEvent(const std::string& data, ArtworkSelectEvent* out) {
  const json payload = json::parse(data, nullptr, false);
  if (!payload.is_object()) return false;
  const std::string id = payload.value("id", std::string());
  if (id.empty()) return false;
  out->id = id;
  out->slot = payload.value("type", std::string());
  out->error = payload.value("error", std::string());
  return true;
}

bool MiradClient::ParseNotification(const std::string& data, NotificationEvent* out) {
  const json payload = json::parse(data, nullptr, false);
  if (payload.is_discarded() || !payload.is_object()) return false;
  out->message = payload.value("message", std::string());
  if (out->message.empty()) return false;
  out->level = payload.value("level", std::string("info"));
  return true;
}

bool MiradClient::ParseRunnerDownload(const std::string& event_type, const std::string& data,
                                      RunnerDownloadEvent* out) {
  constexpr std::string_view kPrefix = "runners.download.";
  if (!event_type.starts_with(kPrefix)) return false;

  const json entry = json::parse(data, nullptr, false);
  if (entry.is_discarded() || !entry.is_object()) return false;
  out->state = event_type.substr(kPrefix.size());
  out->kind = entry.value("kind", std::string());
  out->tag = entry.value("tag", std::string());
  out->error = entry.value("error", std::string());
  return true;
}

bool MiradClient::ParseTricksEvent(const std::string& event_type, const std::string& data,
                                   TricksEvent* out) {
  constexpr std::string_view kPrefix = "tricks.";
  if (!event_type.starts_with(kPrefix)) return false;

  const json entry = json::parse(data, nullptr, false);
  if (entry.is_discarded() || !entry.is_object()) return false;
  out->state = event_type.substr(kPrefix.size());
  out->id = entry.value("id", std::string());
  out->verb = entry.value("verb", std::string());
  out->error = entry.value("error", std::string());
  return true;
}

}  // namespace mira_gui
