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
  std::string install_path;
  bool reviewed = false;
  double confidence = 0.0;
  std::optional<std::int64_t> last_played_at;
  std::int64_t play_seconds = 0;
  // Free-form, user-assigned (docs/api.md). "hidden" is the one convention
  // the frontend treats specially: excluded from the library by default,
  // shown only by the Hidden filter (Ctrl+H).
  std::vector<std::string> tags;
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

// POST /v1/games/{id}/launch. The actual outcome (running, exited,
// crashed) arrives later as a `game.state` SSE event (docs/api.md), since
// launch returns as soon as the process exists, not when it finishes.
//
// `tracked` is mirad's own answer to "are game.state events coming for this
// launch" (docs/api.md), not something inferred here. It is false only for
// a Steam-sourced game under `steam.launch_mode: "steam"` *with*
// `steam.track_process` off — mirad handed it to
// `steam://rungameid/<appid>` and is watching nothing. With track_process
// on (the default) mirad polls /proc for it and real game.state events do
// arrive, a few seconds later than a normal launch.
//
// A caller that assumes tracking when there is none marks the game as
// playing forever, because nothing will ever say it stopped; a caller that
// assumes none when there is marks it stopped while it runs.
struct LaunchResult {
  bool ok = false;
  std::string error;
  bool tracked = true;
};

// `game.launched` — the event mirad publishes for a Steam launch, carrying
// the same `tracked` the launch reply does. Needed as an event and not just
// a reply because the launch may have come from somewhere else entirely
// (the CLI, the other window), and then this is all a client ever sees.
struct GameLaunchedEvent {
  std::string id;
  bool tracked = false;
};

// GET /v1/games/{id}/artwork — the cached cover image itself, as bytes.
//
// `missing` is the 404 case and is not an error: most games have no cached
// artwork, and the placeholder cover is the intended answer for them. Only
// a reachability or server failure sets `error`.
struct ArtworkResult {
  bool ok = false;
  bool missing = false;
  std::string error;
  std::string bytes;
  std::string content_type;
};

// GET /v1/games/{id}/metadata — the store info mirad cached alongside the
// art. Only what a details panel shows; the cached JSON carries more
// (screenshots, requirements, DLC ids) than anything here reads.
// One entry from art_candidates (docs/api.md, GET .../metadata) — a
// SteamGridDB result not necessarily the one currently applied. `url`/
// `thumb` are left out: they're addresses on SteamGridDB's own CDN, and the
// frontend has no HTTP client for the open internet, only mirad's socket —
// a candidate is chosen by `id` and mirad fetches it, never the frontend.
struct ArtCandidate {
  std::int64_t id = 0;
  int width = 0;
  int height = 0;
  std::string style;
};

struct GameMetadata {
  std::string source;  // "steam" | "steamgriddb"
  std::string description;
  std::string release_date;
  std::vector<std::string> developers;
  std::vector<std::string> genres;
  std::string price;
  int metacritic_score = 0;
  std::string review_summary;  // "Very Positive", from Steam's own wording
  int review_total = 0;
  std::string protondb_tier;
  std::string website;
  // Which art slots are actually cached, so a panel knows whether asking for
  // one is worth a round trip. See GET /v1/games/{id}/artwork?type=.
  std::vector<std::string> art_slots;
  // Every cover/hero SteamGridDB returned, cached alongside whichever one is
  // active. Populated for a Steam-owned game too now (as alternates to
  // Steam's own CDN default, not a replacement for it) — empty only when no
  // steamgriddb.api_key is set, or SteamGridDB has no match for the name.
  std::vector<ArtCandidate> cover_candidates;
  std::vector<ArtCandidate> hero_candidates;
  // The candidate currently applied to each slot, when it came from one of
  // the lists above — unset for Steam's own CDN art, which isn't a
  // candidate. What ArtworkPickerDialog marks "(current)".
  std::optional<std::int64_t> cover_active_candidate_id;
  std::optional<std::int64_t> hero_active_candidate_id;
};

struct GameMetadataResult {
  bool ok = false;
  bool missing = false;  // never fetched, or fetched and found nothing
  std::string error;
  GameMetadata metadata;
};

// POST /v1/games/{id}/metadata/refresh — 202, so this says only that the
// fetch was accepted. The outcome arrives as game.metadata_ready or
// game.metadata_failed on the event stream (docs/api.md).
struct MetadataRefreshResult {
  bool ok = false;
  std::string error;
};

// POST /v1/games/metadata/refresh-missing. `count` is how many were enqueued.
struct RefreshMissingArtworkResult {
  bool ok = false;
  std::string error;
  int count = 0;
};

// A game.metadata_ready / game.metadata_failed payload, trimmed to what the
// UI acts on.
struct MetadataEvent {
  std::string id;
  // Both only on .metadata_failed. Branch on `code`, never on `error`:
  // `error` is a sentence written for a human to read.
  std::string code;
  std::string error;
};

// game.artwork_selected / .artwork_select_failed — the outcome of
// POST /v1/games/{id}/artwork?type=, which itself only returns 202.
struct ArtworkSelectEvent {
  std::string id;
  std::string slot;
  std::string error;  // .artwork_select_failed only
};

// POST /v1/games/{id}/artwork?type= — 202, so this is only "accepted", not
// "done". The outcome is ArtworkSelectEvent on the event stream.
struct ArtworkSelectResult {
  bool ok = false;
  std::string error;
};

// A `notification` event — mirad's own decision that this is worth telling
// the user about; the UI just renders it (see MiradClient::ParseNotification
// and mira_gui::notify::Toast).
struct NotificationEvent {
  std::string level;  // "info" | "success" | "warning" | "error"
  std::string message;
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
  std::vector<std::string> tags;
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
  // Replaces the whole set (docs/api.md) — there's no per-entry merge for a
  // plain list the way env's null-removes-a-key convention gives it one.
  std::optional<std::vector<std::string>> tags;
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

  // What the daemon will accept, when that has a shape worth rendering
  // (docs/api.md). Both are absent unless they apply, so a non-empty
  // `one_of` means "this is an enum, offer exactly these" and a set
  // `minimum` means "this is bounded, clamp the spin box to it". Without
  // them every setting is a free-text box and the rules only surface as a
  // rejection after saving.
  std::vector<std::string> one_of;
  std::optional<double> minimum;
  std::optional<double> maximum;
  std::string category;        // UI grouping; always present
  bool is_secret = false;      // mask this value's field
  bool is_runner_ref = false;  // offer a runner picker (GET /v1/runners) instead of free text
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

// POST /v1/lutris/import. `skipped` counts Lutris rows this import cannot
// use — a non-wine runner, or a wine game whose yaml records no prefix.
struct LutrisImportResult {
  bool ok = false;
  std::string error;
  int added = 0;
  int updated = 0;
  int skipped = 0;
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
// this does.
//
// Every field is optional because the file is allowed to be absent, partial
// or hand-edited — an unset field means "use the built-in default", not
// zero.
struct FrontendPrefs {
  std::optional<int> window_width;
  std::optional<int> window_height;
  std::optional<int> tile_width;
  std::optional<std::string> library_filter;  // a filter key
  std::optional<int> details_width;
  std::optional<std::string> sort_by;  // "name" | "last_played" | "playtime" | "status"
  std::optional<bool> sort_descending;
  // Whether opening the frontend also kicks off POST /v1/library/scan.
  // Worth turning off for a large library on slow storage, where the scan
  // is the slowest thing about startup and the daemon's own watcher
  // (library::Watcher) already keeps the library current while it runs.
  std::optional<bool> scan_on_startup;
  // Seconds a toast stays up before it's dismissed automatically; 0 means
  // until dismissed, which is the default. See notify::SetTimeoutSeconds —
  // toasts always go to the desktop's own notification service, so this is
  // also that notification's expire timeout.
  std::optional<int> notification_timeout_s;
  // A theme name (ui/Theme.h), or "auto" — the default — to follow the
  // desktop's own light/dark preference.
  std::optional<std::string> theme;
  // On (default): "Details & settings" edits a game inline in the right
  // panel instead of opening a dialog.
  std::optional<bool> game_settings_in_sidebar;
  // Shape adjustments layered over whatever the theme sets, in pixels — see
  // theme::Overrides. Unset means "leave it to the theme".
  std::optional<int> tile_spacing;
  std::optional<int> grid_margin;
  std::optional<int> tile_radius;
  std::optional<int> panel_radius;
  std::optional<int> control_radius;
  std::optional<int> hero_height;
  // Overridden keyboard shortcuts, id (ui/KeyBindings.h) -> a
  // QKeySequence::toString(PortableText) string. An id absent here just
  // means "whatever that action's own default is" -- see keybindings::All().
  std::optional<std::map<std::string, std::string>> shortcut_overrides;
};

struct FrontendPrefsResult {
  bool ok = false;
  std::string error;
  FrontendPrefs prefs;
};

}  // namespace mira_gui
