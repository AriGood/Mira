#pragma once

#include <atomic>
#include <filesystem>
#include <memory>

#include "api/EventBus.h"
#include "config/Config.h"
#include "core/Result.h"
#include "proc/ProcessSupervisor.h"
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

private:
  void RegisterRoutes();

  config::Config& config_;
  store::GameStore& games_;
  EventBus& events_;
  std::unique_ptr<httplib::Server> http_;
  proc::ProcessSupervisor supervisor_;
  std::atomic<bool> stopping_{false};  // checked by open SSE connections; see EventBus::WaitNext
};

}  // namespace mira::api
