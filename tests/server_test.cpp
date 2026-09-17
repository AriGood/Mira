#include <doctest.h>
#include <httplib.h>
#include <sys/socket.h>

#include <chrono>
#include <filesystem>
#include <thread>

#include "api/EventBus.h"
#include "api/Server.h"
#include "config/Config.h"
#include "store/GameStore.h"

using namespace mira;
namespace fs = std::filesystem;

namespace {

fs::path TempDir(const char* name) {
  const fs::path dir = fs::temp_directory_path() / "mira-tests" / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

// Runs a real Server over a real UDS socket for the lifetime of the test —
// PATCH /v1/games/{id} env handling is otherwise only ever exercised by hand
// over curl/the CLI, which is exactly the kind of "looks right, isn't" gap
// this project has repeatedly found by testing real behavior instead.
class LiveServer {
public:
  explicit LiveServer(const fs::path& state_dir)
      : config_(state_dir / "settings.toml"), games_(state_dir / "games.toml"),
        socket_path_(state_dir / "mirad.sock"), server_(config_, games_, events_) {
    config_.Load();
    games_.Load();
    thread_ = std::thread([this] { [[maybe_unused]] auto _ = server_.Serve(socket_path_); });
    // Serve() binds synchronously before it blocks accepting, but the thread
    // itself needs a moment to actually start running; poll for the socket
    // file rather than a fixed sleep.
    for (int i = 0; i < 200 && !fs::exists(socket_path_); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  ~LiveServer() {
    server_.Stop();
    if (thread_.joinable()) thread_.join();
  }

  httplib::Client Client() {
    httplib::Client client(socket_path_.string(), 80);
    client.set_address_family(AF_UNIX);
    return client;
  }

  store::GameStore& games() { return games_; }

private:
  config::Config config_;
  store::GameStore games_;
  api::EventBus events_;
  fs::path socket_path_;
  api::Server server_;
  std::thread thread_;
};

}  // namespace

TEST_CASE("PATCH /v1/games/{id} env: per-key null removes just that key") {
  LiveServer server(TempDir("server-env-patch"));

  model::Game game;
  game.id = "celeste";
  game.name = "Celeste";
  game.env = {{"FOO", "1"}, {"BAR", "2"}};
  REQUIRE(server.games().Upsert(game).has_value());

  httplib::Client client = server.Client();
  auto res = client.Patch("/v1/games/celeste", R"({"env": {"BAR": null, "BAZ": "3"}})", "application/json");
  REQUIRE(res != nullptr);
  CHECK(res->status == 200);

  auto stored = server.games().Find("celeste");
  REQUIRE(stored.has_value());
  CHECK(stored->env.contains("FOO"));
  CHECK_FALSE(stored->env.contains("BAR"));
  CHECK(stored->env.at("BAZ") == "3");
}

TEST_CASE("PATCH /v1/games/{id} env: a top-level null clears every entry") {
  LiveServer server(TempDir("server-env-patch-clear"));

  model::Game game;
  game.id = "celeste";
  game.name = "Celeste";
  game.env = {{"FOO", "1"}, {"BAR", "2"}};
  REQUIRE(server.games().Upsert(game).has_value());

  httplib::Client client = server.Client();
  auto res = client.Patch("/v1/games/celeste", R"({"env": null})", "application/json");
  REQUIRE(res != nullptr);
  CHECK(res->status == 200);

  auto stored = server.games().Find("celeste");
  REQUIRE(stored.has_value());
  CHECK(stored->env.empty());
}
