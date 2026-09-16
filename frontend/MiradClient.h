#pragma once

#include <QObject>

#include <functional>
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
  bool reviewed = false;
  double confidence = 0.0;
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

class MiradClient {
public:
  // Mirrors core/Paths.h's DefaultSocket(): $XDG_RUNTIME_DIR/mira/mirad.sock,
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

  // Same pattern as CheckHealthAsync, for GET /v1/games.
  static void ListGamesAsync(QObject* context, std::function<void(GamesResult)> callback);

  // DELETE /v1/games/{id}. Never touches the game's files on disk (docs/api.md)
  // — this only forgets it, the same as `delete_data` being left unset.
  static void DeleteGameAsync(QObject* context, const std::string& id,
                              std::function<void(DeleteResult)> callback);

  // POST /v1/library/scan. Runs synchronously on mirad's side (docs/api.md
  // notes there's no job queue yet), so this still goes through the async
  // worker-thread dance to keep the UI thread free while it waits.
  static void ScanLibraryAsync(QObject* context, std::function<void(ScanResult)> callback);
};

}  // namespace mira_gui
