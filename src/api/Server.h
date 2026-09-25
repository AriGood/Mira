#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <thread>

#include "api/EventBus.h"
#include "config/Config.h"
#include "core/BackgroundQueue.h"
#include "core/Result.h"
#include "metadata/FetchQueue.h"
#include "proc/ProcessSupervisor.h"
#include "runner/Downloader.h"
#include "store/GameStore.h"

// httplib::Server is used only by Server.cpp; forward-declared here so
// including this header does not pull the whole vendored library into every
// translation unit that wires up a daemon.
namespace httplib {
class Server;
}

namespace mira::api {

// The REST-over-UDS surface described in docs/api.md. Owns the socket and the
// httplib server; everything it touches (Config, GameStore, EventBus) is
// injected so the routes can be exercised without a real socket in tests.
class Server {
public:
  Server(config::Config& config, store::GameStore& games, EventBus& events);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Binds the UDS socket (removing a stale one first) and blocks serving
  // until Stop() is called from another thread. The bind happens inside this
  // call so a failure is reported through the return value rather than a
  // separate two-step API.
  Result<void> Serve(const std::filesystem::path& socket_path);
  void Stop();

  // Closes out any session a previous mirad left behind (see
  // proc::ProcessSupervisor::Reconcile) — called once at startup, before
  // Serve(), so a session already re-adopted is visible to the very first
  // GET /v1/games a client makes.
  void ReconcileSessions();

  // Called after a request changes library_roots, so the watcher can follow.
  // Set once, before Serve().
  void SetOnLibraryRootsChanged(std::function<void()> callback) { on_roots_changed_ = std::move(callback); }

private:
  void RegisterRoutes();
  void WatchExternalGames();
  // Installs a runner build in the background, publishing runners.download.*.
  // With `replacing` ("kind:name"), games and the default using it move over.
  void InstallRunnerAsync(const std::string& kind, const std::string& source, const runner::ReleaseAsset& asset,
                          const std::string& replacing);

  config::Config& config_;
  store::GameStore& games_;
  EventBus& events_;
  std::unique_ptr<httplib::Server> http_;
  proc::ProcessSupervisor supervisor_;
  metadata::FetchQueue metadata_fetches_;
  BackgroundQueue tricks_queue_;
  BackgroundQueue artwork_selects_;
  BackgroundQueue artwork_thumbs_;
  std::function<void()> on_roots_changed_;
  std::atomic<bool> stopping_{false};  // checked by open SSE connections; see EventBus::WaitNext
  std::thread external_watch_;
};

}  // namespace mira::api
