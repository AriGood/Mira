#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

// The plain data mirad's REST API speaks, as C++ structs.
//
// Deliberately free of Qt and of httplib: these are what the endpoints in
// MiradClient.h return and what every widget in the frontend reads, so they
// are the one part of the client layer the UI is allowed to depend on
// directly. See docs/api.md for the JSON each one mirrors.
namespace mira_gui {

struct HealthStatus {
  bool reachable = false;
  std::string detail;
};

// Mirrors the subset of the `Game` fields (docs/api.md, GET /v1/games) a
// library list needs — not the full record.
struct GameSummary {
  std::string id;
  std::string name;
  std::string status;
  std::string platform;
  std::string runner_ref;
  std::string last_error;
  bool reviewed = false;
  double confidence = 0.0;
  std::optional<std::int64_t> last_played_at;
  std::int64_t play_seconds = 0;
};

struct GamesResult {
  bool ok = false;
  std::string error;
  std::vector<GameSummary> games;
};

// docs/api.md's `game.state` event, trimmed to what a row's Launch/Stop
// button needs — see ParseGameState.
struct GameStateEvent {
  std::string id;
  std::string state;  // "running" | "exited" | "crashed"
};

struct DeleteResult {
  bool ok = false;
  std::string error;
};

// POST /v1/games/{id}/launch and /stop both return just ok/error — the
// actual outcome (running, exited, crashed) arrives later as a `game.state`
// SSE event (docs/api.md), since launch returns as soon as the process
// exists, not when it finishes.
struct LaunchResult {
  bool ok = false;
  std::string error;
};

struct StopResult {
  bool ok = false;
  std::string error;
};

struct ScanResult {
  bool ok = false;
  std::string error;
  int added = 0;
  int missing = 0;
  int restored = 0;
};

// The full record GET /v1/games/{id} returns (docs/api.md) — everything a
// detail/edit view needs, beyond GameSummary's list-row subset.
struct GameDetail {
  struct Candidate {
    std::string rel_path;
    std::string kind;
    double score = 0.0;
    bool chosen = false;
    bool is_installer = false;  // name + size say this is a setup.exe, not the game
  };

  std::string id;
  std::string name;
  std::string status;
  std::string platform;
  std::string install_path;
  std::string exe_path;
  std::string args;
  std::string working_dir;
  std::string runner_ref;
  std::string data_dir;
  std::string last_error;
  bool reviewed = false;
  double confidence = 0.0;
  std::optional<std::int64_t> last_played_at;
  std::int64_t play_seconds = 0;
  // `runner_config`/`env` are arbitrary JSON objects (docs/api.md) with no
  // fixed shape to build widgets for, so they round-trip as raw JSON text —
  // pretty-printed for display, re-parsed on save (MiradClient.cpp).
  std::string runner_config_json;
  std::string env_json;
  std::vector<Candidate> candidates;
};

struct GameDetailResult {
  bool ok = false;
  std::string error;
  GameDetail game;
};

// A subset of PATCH /v1/games/{id}'s body (docs/api.md): only the fields
// present are sent, matching the endpoint's own "any subset" contract.
struct GamePatch {
  std::optional<std::string> name;
  std::optional<std::string> exe_path;
  std::optional<std::string> args;
  std::optional<std::string> working_dir;
  std::optional<std::string> runner_ref;
  std::optional<std::string> data_dir;
  // Raw JSON text (must parse to an object) — see GameDetail's comment.
  std::optional<std::string> runner_config_json;
  std::optional<std::string> env_json;
};

struct PatchGameResult {
  bool ok = false;
  std::string error;
};

// One GET /v1/config/schema entry (docs/api.md): every backend setting
// declared exactly once in src/config/Schema.cpp, which is what lets a
// settings screen exist with zero hardcoded knowledge of what settings
// there are. `type` is one of the human strings config::ToString(Type)
// produces: "a boolean" | "an integer" | "a number" | "a string" |
// "an array of strings" | "an object". `tier` is "basic" | "advanced" |
// "expert" — a frontend should show basic by default and fold the rest
// behind a disclosure, never omit them.
struct ConfigSchemaEntry {
  std::string key;
  std::string type;
  std::string tier;
  std::string doc;
  std::string default_display;
};

struct ConfigSchemaResult {
  bool ok = false;
  std::string error;
  std::vector<ConfigSchemaEntry> entries;
};

struct ConfigResult {
  bool ok = false;
  std::string error;
  // Every leaf of GET /v1/config's document, flattened to dotted keys
  // matching Schema entries' own `key` (e.g. "scan.debounce_ms"), each
  // stringified for display/editing: a bool as "true"/"false", a number in
  // its natural text form, an array of strings comma-joined. The opaque
  // `frontend` table (docs/architecture.md) is excluded — it isn't part of
  // the schema this screen renders.
  std::map<std::string, std::string> values;
};

// One edit to send in a PATCH /v1/config body. `type` is the schema type
// string (ConfigSchemaEntry::type), so the string typed into the UI can be
// converted back to the right JSON kind before sending.
struct ConfigEdit {
  std::string key;
  std::string type;
  std::string value;
};

struct PatchConfigResult {
  bool ok = false;
  std::string error;
};

// One entry from GET /v1/runners (docs/api.md): an installed build of one
// runner kind, discovered fresh on every call. `reference` is what a game's
// `runner_ref` field and the `default_runner.*` settings both use — the
// only string that actually round-trips, `name`/`version` are just for
// display.
struct RunnerInfo {
  std::string kind;
  std::string name;
  std::string version;
  std::string reference;
};

struct RunnersResult {
  bool ok = false;
  std::string error;
  std::vector<RunnerInfo> runners;
};

// One key from GET /v1/games/{id}/config (docs/api.md): every schema key
// resolved through default -> settings.toml -> this game's overrides,
// tagged with which layer supplied it. Only entries with `overridable: true`
// (config::Resolver::IsOverridable) make sense to show as editable — some
// settings describe the daemon rather than a game (e.g. `library_roots`)
// and are excluded there for exactly that reason.
struct GameConfigEntry {
  std::string key;
  std::string value_display;  // stringified like ConfigResult::values
  std::string layer;          // "default" | "config" | "game"
  bool overridable = false;
};

struct GameConfigResult {
  bool ok = false;
  std::string error;
  std::vector<GameConfigEntry> entries;
};

// One edit to send in a PATCH /v1/games/{id}/config body. `clear` sends a
// JSON null for `key`, removing this game's override and falling back to
// the next layer down — the per-game equivalent of ConfigEdit, which has no
// such concept since a global setting has no further layer to fall back to.
struct GameConfigEdit {
  std::string key;
  std::string type;
  std::string value;
  bool clear = false;
};

struct PatchGameConfigResult {
  bool ok = false;
  std::string error;
};

// One release from GET /v1/runners/catalog (docs/api.md): a runner build
// that is *available to install*, as opposed to RunnerInfo, which is one
// already installed. `tag` is what POST /v1/runners/download takes.
struct RunnerRelease {
  std::string tag;
  std::string asset_name;
  std::int64_t size_bytes = 0;
  std::string published_at;
  bool has_checksum = false;
};

struct RunnerCatalogResult {
  bool ok = false;
  std::string error;
  std::vector<RunnerRelease> releases;
};

// POST /v1/runners/download returns 202 immediately and reports progress on
// the event stream, so "ok" here only means the download started.
struct RunnerDownloadResult {
  bool ok = false;
  std::string error;
};

// A `runners.download.started` / `.finished` / `.failed` payload.
struct RunnerDownloadEvent {
  std::string kind;
  std::string tag;
  std::string state;  // "started" | "finished" | "failed"
  std::string error;  // only on "failed"
};

// POST /v1/steam/scan.
struct SteamScanResult {
  bool ok = false;
  std::string error;
  int added = 0;
  int updated = 0;
};

// POST /v1/games/{id}/run — an arbitrary executable inside this game's own
// prefix, tracked like a normal launch.
struct RunInPrefixResult {
  bool ok = false;
  std::string error;
};

// POST /v1/games/{id}/finish-install.
struct FinishInstallResult {
  bool ok = false;
  std::string error;
};

// The frontend's own preferences, which live in frontend.toml — the sibling
// file the backend stores verbatim and never interprets
// (docs/architecture.md), reachable as the opaque `frontend` key of
// GET/PATCH /v1/config.
//
// Deliberately not settings.toml: every key there has to be declared in
// src/config/Schema.cpp and means something to the daemon, whereas none of
// this does. A window size is not a setting mirad should have an opinion
// about, and putting it there would make the schema answer for it.
//
// Every field is optional because the file is allowed to be absent, partial
// or hand-edited — an unset field means "use the built-in default", not
// zero.
struct FrontendPrefs {
  std::optional<int> window_width;
  std::optional<int> window_height;
  std::optional<int> tile_width;
  std::optional<std::string> library_filter;  // a sidebar filter key
  std::optional<int> sidebar_width;
  std::optional<int> details_width;
  std::optional<std::string> sort_by;  // "name" | "last_played" | "playtime" | "status"
  std::optional<bool> sort_descending;
  // Whether opening the frontend also kicks off POST /v1/library/scan.
  // Worth turning off for a large library on slow storage, where the scan
  // is the slowest thing about startup and the daemon's own watcher
  // (library::Watcher) already keeps the library current while it runs.
  std::optional<bool> scan_on_startup;
};

struct FrontendPrefsResult {
  bool ok = false;
  std::string error;
  FrontendPrefs prefs;
};

}  // namespace mira_gui
