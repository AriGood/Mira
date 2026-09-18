#pragma once

#include <QObject>

#include <functional>
#include <string>
#include <vector>

#include "Types.h"

// A minimal REST client for mirad, following the same contract as `mira`
// (src/cli/main.cpp): plain HTTP over the daemon's Unix domain socket, no
// dependency on mira_core. See docs/api.md and docs/architecture.md.
//
// Only endpoints live here. Transport.h owns the socket, the timeouts and
// the error envelope. JsonMapping.h owns the JSON conversions, and Async.h
// owns the thread hop. Methods run on a throwaway worker thread and deliver
// results back on the main thread, so UI never blocks on the socket.
namespace mira_gui {

class MiradClient {
public:
  // The socket every call above goes to. Exposed because the UI shows it.
  static std::string ResolveSocketPath();

  // GET /v1/health.
  static void CheckHealthAsync(QObject* context, std::function<void(HealthStatus)> callback);

  // GET /v1/games[?status=...][?tag=...]. An empty filter omits that query
  // param entirely. `tag_filter` composes with `status_filter` the way
  // mirad does (docs/api.md); `?tag=hidden` is the only call that returns a
  // hidden-tagged game at all.
  static void ListGamesAsync(QObject* context, std::function<void(GamesResult)> callback,
                             const std::string& status_filter = std::string(),
                             const std::string& tag_filter = std::string());

  // DELETE /v1/games/{id}[?delete_files=true][?delete_prefix=true]. With both
  // flags false, this only forgets the game and never touches disk.
  // `delete_files` removes its install_path, `delete_prefix` its data_dir;
  // mirad refuses either if the path isn't really inside a configured
  // library_roots/prefix_root (docs/api.md), so a 400 here is a guard
  // tripping, not a missing permission.
  static void DeleteGameAsync(QObject* context, const std::string& id, bool delete_files,
                              bool delete_prefix, std::function<void(DeleteResult)> callback);

  // POST /v1/games/{id}/launch. Returns once the process exists, not once
  // it exits — 409 if the game isn't `ready` (message explains why, e.g.
  // needs_install) or 400 if the runner reference doesn't resolve.
  static void LaunchGameAsync(QObject* context, const std::string& id,
                              std::function<void(LaunchResult)> callback);

  // POST /v1/games/{id}/stop. SIGTERMs the game's process group; 409 if it
  // isn't currently running.
  static void StopGameAsync(QObject* context, const std::string& id,
                            std::function<void(StopResult)> callback);

  // POST /v1/library/scan. Runs synchronously on mirad's side, hence
  // its own read timeout.
  static void ScanLibraryAsync(QObject* context, std::function<void(ScanResult)> callback);

  // GET /v1/games/{id}, for the detail/edit view.
  static void GetGameAsync(QObject* context, const std::string& id,
                           std::function<void(GameDetailResult)> callback);

  // PATCH /v1/games/{id}. Setting any of these fields marks the game as reviewed.
  static void PatchGameAsync(QObject* context, const std::string& id, const GamePatch& patch,
                             std::function<void(PatchGameResult)> callback);

  // GET /v1/config/schema.
  static void GetConfigSchemaAsync(QObject* context,
                                   std::function<void(ConfigSchemaResult)> callback);

  // GET /v1/config, flattened.
  static void GetConfigAsync(QObject* context, std::function<void(ConfigResult)> callback);

  // PATCH /v1/config. A bad value anywhere in the patch means nothing
  // in it is applied.
  static void PatchConfigAsync(QObject* context, const std::vector<ConfigEdit>& edits,
                               std::function<void(PatchConfigResult)> callback);

  // POST /v1/config/reset?key=<dotted.key>.
  static void ResetConfigKeyAsync(QObject* context, const std::string& key,
                                  std::function<void(PatchConfigResult)> callback);

  // GET /v1/config, reading only the opaque `frontend` table.
  static void GetFrontendPrefsAsync(QObject* context,
                                    std::function<void(FrontendPrefsResult)> callback);

  // PATCH /v1/config with a `frontend` key. Merge-patch, so only the fields
  // set in `prefs` are written and a key this build doesn't know about
  // (an older or newer frontend's) survives untouched.
  static void SaveFrontendPrefsAsync(QObject* context, const FrontendPrefs& prefs,
                                     std::function<void(PatchConfigResult)> callback);

  // The same PATCH, run on the calling thread.
  //
  // For the one caller that has nowhere to deliver a result to and no time
  // to wait for one: a window saving its layout from closeEvent. The async
  // form hands the request to a detached thread, which the process can
  // outrun on its way out. One round trip over a Unix socket is cheap
  // enough to just wait for, and the short timeout below means an
  // unreachable daemon cannot turn quitting into a hang.
  static PatchConfigResult SaveFrontendPrefsBlocking(const FrontendPrefs& prefs);

  // GET /v1/games/{id}/artwork. Binary, not JSON, and a 404 is the ordinary
  // answer for a game nothing has been fetched for yet — see ArtworkResult.
  static void GetArtworkAsync(QObject* context, const std::string& id,
                              std::function<void(ArtworkResult)> callback);

  // POST /v1/games/{id}/metadata/refresh. Returns 202 immediately; watch for
  // game.metadata_ready/.metadata_failed. `announce` marks this as
  // user-initiated so mirad reports the outcome as a `notification` event.
  static void RefreshMetadataAsync(QObject* context, const std::string& id, bool announce,
                                   std::function<void(MetadataRefreshResult)> callback);

  // POST /v1/games/metadata/refresh-missing. Bulk version of the above.
  static void RefreshMissingArtworkAsync(QObject* context,
                                         std::function<void(RefreshMissingArtworkResult)> callback);

  // GET /v1/runners.
  static void ListRunnersAsync(QObject* context, std::function<void(RunnersResult)> callback);

  // GET /v1/runners/catalog?kind=proton|wine — what is available to
  // install. Unlike everything else here this goes out to the GitHub API,
  // so it has real network latency and its own longer timeout.
  static void GetRunnerCatalogAsync(QObject* context, const std::string& kind,
                                    std::function<void(RunnerCatalogResult)> callback);

  // POST /v1/runners/download. Returns 202 as soon as the download starts;
  // the outcome arrives as a runners.download.finished/.failed event, since
  // a build can be 500+ MB.
  static void DownloadRunnerAsync(QObject* context, const std::string& kind,
                                  const std::string& tag,
                                  std::function<void(RunnerDownloadResult)> callback);

  // POST /v1/steam/scan. Idempotent: updates Steam-owned fields without
  // touching anything the user configured.
  static void ScanSteamAsync(QObject* context, std::function<void(SteamScanResult)> callback);

  // POST /v1/games/{id}/run. Runs `exe_path` inside this game's prefix,
  // provisioning one on demand — which is how a needs_install game's
  // installer actually gets run, since Scanner never auto-provisions one.
  static void RunInPrefixAsync(QObject* context, const std::string& id,
                               const std::string& exe_path, const std::string& args,
                               std::function<void(RunInPrefixResult)> callback);

  // POST /v1/games/{id}/finish-install — flips a needs_install game to
  // ready once exe_path points at whatever the installer produced. 409 if
  // exe_path is still empty.
  static void FinishInstallAsync(QObject* context, const std::string& id,
                                 std::function<void(FinishInstallResult)> callback);

  // GET /v1/games/{id}/config — this game's resolved settings, tagged by
  // layer (see GameConfigEntry).
  static void GetGameConfigAsync(QObject* context, const std::string& id,
                                 std::function<void(GameConfigResult)> callback);

  // PATCH /v1/games/{id}/config. Same all-or-nothing validation as
  // PATCH /v1/config (docs/api.md) — only send edits that actually changed.
  static void PatchGameConfigAsync(QObject* context, const std::string& id,
                                   const std::vector<GameConfigEdit>& edits,
                                   std::function<void(PatchGameConfigResult)> callback);

  // --- SSE payload parsing -------------------------------------------------
  //
  // These take a `data:` line's raw JSON rather than making a request, since
  // the events arrive through EventStream, not through a call.

  // Parses a `game.added`/`game.updated` payload (Server.cpp publishes the
  // full model::ToJson(game) record for both) into the same summary
  // GET /v1/games returns. False unless `data` is a JSON object carrying a
  // non-empty string id — callers dispatch on the event type first, and this
  // is the second line of defence behind that.
  static bool ParseGameSummary(const std::string& data, GameSummary* out);

  // Parses a `game.state` payload (`{"id", "state": "running" | "exited" |
  // "crashed", ...}`, docs/api.md) down to just id/state — enough to know
  // which row's Launch/Stop button to flip. Unlike game.added/updated this
  // carries no other game fields (not even play_seconds), so an
  // "exited"/"crashed" state is a signal to re-fetch, not something to patch
  // a row from directly.
  static bool ParseGameState(const std::string& data, GameStateEvent* out);

  // Parses `game.launched`. `tracked` defaults to false when the payload
  // omits it: an older daemon published this event only for the case where
  // nothing was watching, so that is what its silence meant.
  static bool ParseGameLaunched(const std::string& data, GameLaunchedEvent* out);

  // Parses `game.removed`'s payload (`{"id": "..."}`, Server.cpp).
  static std::string ParseRemovedId(const std::string& data);

  // Parses a `game.metadata_ready`/`.metadata_failed` payload. `state` is
  // the event type, which the payload does not repeat; false if `data` is
  // not a JSON object with an id.
  static bool ParseMetadataEvent(const std::string& data, MetadataEvent* out);

  // Parses a `notification` payload (`{"level": "...", "message": "..."}`).
  // False if `data` is not a JSON object with a message.
  static bool ParseNotification(const std::string& data, NotificationEvent* out);

  // Parses a `runners.download.started`/`.finished`/`.failed` payload.
  // `state` comes from the event type, which the payload itself doesn't
  // repeat.
  static bool ParseRunnerDownload(const std::string& event_type, const std::string& data,
                                  RunnerDownloadEvent* out);
};

}  // namespace mira_gui
