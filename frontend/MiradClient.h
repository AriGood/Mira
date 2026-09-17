#pragma once

#include <QObject>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

// A minimal REST client for mirad, following the same contract as `mira`
// (src/cli/main.cpp): plain HTTP over the daemon's Unix domain socket, no
// dependency on mira_core. See docs/api.md and docs/architecture.md.
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

struct DeleteResult {
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

class MiradClient {
public:
  // $MIRA_SOCKET if set — the same override `mirad --socket` accepts on the
  // daemon side, so `MIRA_SOCKET=/path/to.sock mirad --socket
  // /path/to.sock` and `MIRA_SOCKET=/path/to.sock mira-gui` unambiguously
  // talk to each other regardless of what else is running. Without it,
  // mirrors core/Paths.h's DefaultSocket(): $XDG_RUNTIME_DIR/mira/mirad.sock,
  // falling back to /tmp/mira if XDG_RUNTIME_DIR is unset. Does not yet honor
  // a socket_path override from settings.toml — that needs a `GET /v1/config`
  // round trip through a socket we haven't resolved yet, so it's out of scope
  // until there's a settings screen to wire it through.
  static std::string ResolveSocketPath();

  // Calls GET /v1/health on a worker thread and delivers the result back on
  // `context`'s thread via a queued call, so the UI thread never blocks on
  // the socket. Safe to call even if `context` is destroyed before the
  // request finishes: Qt drops the callback instead of invoking it.
  static void CheckHealthAsync(QObject* context, std::function<void(HealthStatus)> callback);

  // Same pattern as CheckHealthAsync, for GET /v1/games[?status=...]. An
  // empty `status_filter` omits the query param entirely (every status).
  static void ListGamesAsync(QObject* context, std::function<void(GamesResult)> callback,
                             const std::string& status_filter = std::string());

  // DELETE /v1/games/{id}. Never touches the game's files on disk (docs/api.md)
  // — this only forgets it, the same as `delete_data` being left unset.
  static void DeleteGameAsync(QObject* context, const std::string& id,
                              std::function<void(DeleteResult)> callback);

  // POST /v1/library/scan. Runs synchronously on mirad's side (docs/api.md
  // notes there's no job queue yet), so this still goes through the async
  // worker-thread dance to keep the UI thread free while it waits.
  static void ScanLibraryAsync(QObject* context, std::function<void(ScanResult)> callback);

  // GET /v1/games/{id}, for the detail/edit view.
  static void GetGameAsync(QObject* context, const std::string& id,
                           std::function<void(GameDetailResult)> callback);

  // PATCH /v1/games/{id}. Per docs/api.md, setting any of these fields marks
  // the game reviewed: true — a correction is the review.
  static void PatchGameAsync(QObject* context, const std::string& id, const GamePatch& patch,
                             std::function<void(PatchGameResult)> callback);

  // Parses one `data:` payload from a `game.added`/`game.updated` SSE event
  // (Server.cpp publishes the full `model::ToJson(game)` record for both —
  // see AutoSetup.cpp and Server.cpp) into the same summary GET /v1/games
  // returns. Returns false if `data` isn't a JSON object.
  static bool ParseGameSummary(const std::string& data, GameSummary* out);

  // Parses `game.removed`'s payload (`{"id": "..."}`, Server.cpp) down to
  // just the id.
  static std::string ParseRemovedId(const std::string& data);

  // GET /v1/config/schema.
  static void GetConfigSchemaAsync(QObject* context,
                                   std::function<void(ConfigSchemaResult)> callback);

  // GET /v1/config, flattened — see ConfigResult.
  static void GetConfigAsync(QObject* context, std::function<void(ConfigResult)> callback);

  // PATCH /v1/config. Per docs/api.md, a bad value anywhere in the patch
  // means nothing in it is applied — callers should only include edits
  // that actually changed, so one unrelated typo can't block the rest.
  static void PatchConfigAsync(QObject* context, const std::vector<ConfigEdit>& edits,
                               std::function<void(PatchConfigResult)> callback);

  // POST /v1/config/reset?key=<dotted.key>.
  static void ResetConfigKeyAsync(QObject* context, const std::string& key,
                                  std::function<void(PatchConfigResult)> callback);

  // GET /v1/runners. Freshly discovered on every call — no caching needed on
  // this side either.
  static void ListRunnersAsync(QObject* context, std::function<void(RunnersResult)> callback);

  // GET /v1/games/{id}/config — this game's resolved settings, tagged by
  // layer (see GameConfigEntry).
  static void GetGameConfigAsync(QObject* context, const std::string& id,
                                 std::function<void(GameConfigResult)> callback);

  // PATCH /v1/games/{id}/config. Same all-or-nothing validation as
  // PATCH /v1/config (docs/api.md) — only send edits that actually changed.
  static void PatchGameConfigAsync(QObject* context, const std::string& id,
                                   const std::vector<GameConfigEdit>& edits,
                                   std::function<void(PatchGameConfigResult)> callback);
};

// A long-lived connection to GET /v1/events (docs/api.md), which streams
// Server-Sent Events for `game.updated`/`game.added`/`game.removed` as the
// library watcher does its work. `mira watch` (src/cli/main.cpp) is the
// reference client this mirrors: a streaming GET split on blank lines.
//
// Unlike MiradClient's one-shot calls, this holds state (the last event id,
// for replay across reconnects) and its background thread runs for as long
// as the connection keeps getting reconnected, so it's an instance rather
// than a static call.
class EventStream {
public:
  // Starts (or restarts) the background connection. Delivers each event's
  // type and raw JSON data on `context`'s thread. Reconnects with a fixed
  // backoff on any drop — mirad itself may restart independently of the
  // frontend — replaying via Last-Event-ID so a reconnect doesn't miss
  // events still in mirad's 500-event buffer.
  //
  // Never explicitly stopped: this process has exactly one window for its
  // whole lifetime, so the background thread is simply abandoned on exit.
  // That's safe — Qt's context-object overload of invokeMethod (used
  // throughout MiradClient) drops queued calls once `context` is destroyed,
  // even from another thread.
  void Start(QObject* context, std::function<void(std::string type, std::string data)> on_event);
};

}  // namespace mira_gui
