#include <doctest.h>
#include <httplib.h>
#include <sys/socket.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include <json.hpp>

#include "api/EventBus.h"
#include "api/Server.h"
#include "config/Config.h"
#include "metadata/MetadataFetcher.h"
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
  const config::Config& config() const { return config_; }

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

TEST_CASE("GET /v1/games excludes hidden-tagged games by default; ?tag= filters, including for hidden") {
  LiveServer server(TempDir("server-tags"));

  model::Game visible;
  visible.id = "celeste";
  visible.name = "Celeste";
  visible.tags = {"platformer"};
  REQUIRE(server.games().Upsert(visible).has_value());

  model::Game hidden;
  hidden.id = "umu-launcher";
  hidden.name = "umu-launcher";
  hidden.tags = {"hidden", "tool"};
  REQUIRE(server.games().Upsert(hidden).has_value());

  httplib::Client client = server.Client();

  auto default_list = client.Get("/v1/games");
  REQUIRE(default_list != nullptr);
  CHECK(default_list->body.find("\"celeste\"") != std::string::npos);
  CHECK(default_list->body.find("\"umu-launcher\"") == std::string::npos);

  auto hidden_list = client.Get("/v1/games?tag=hidden");
  REQUIRE(hidden_list != nullptr);
  CHECK(hidden_list->body.find("\"umu-launcher\"") != std::string::npos);
  CHECK(hidden_list->body.find("\"celeste\"") == std::string::npos);

  auto tool_list = client.Get("/v1/games?tag=tool");
  REQUIRE(tool_list != nullptr);
  CHECK(tool_list->body.find("\"umu-launcher\"") != std::string::npos);
}

TEST_CASE("PATCH /v1/games/{id} tags replaces the array wholesale") {
  LiveServer server(TempDir("server-tags-patch"));

  model::Game game;
  game.id = "celeste";
  game.name = "Celeste";
  game.tags = {"platformer", "indie"};
  REQUIRE(server.games().Upsert(game).has_value());

  httplib::Client client = server.Client();
  auto res = client.Patch("/v1/games/celeste", R"({"tags": ["hidden"]})", "application/json");
  REQUIRE(res != nullptr);
  CHECK(res->status == 200);

  auto stored = server.games().Find("celeste");
  REQUIRE(stored.has_value());
  REQUIRE(stored->tags.size() == 1);
  CHECK(stored->tags[0] == "hidden");

  auto cleared = client.Patch("/v1/games/celeste", R"({"tags": []})", "application/json");
  REQUIRE(cleared != nullptr);
  CHECK(cleared->status == 200);
  CHECK(server.games().Find("celeste")->tags.empty());
}

TEST_CASE("GET /v1/games/{id}/artwork?type= serves the requested slot, 404s for an unknown one") {
  LiveServer server(TempDir("server-artwork-type"));

  model::Game game;
  game.id = "celeste";
  game.name = "Celeste";
  REQUIRE(server.games().Upsert(game).has_value());

  const fs::path artwork_dir = metadata::ArtworkDir(server.config(), "celeste");
  fs::create_directories(artwork_dir);
  {
    std::ofstream cover(artwork_dir / "cover.jpg");
    cover << "cover-bytes";
  }
  {
    std::ofstream hero(artwork_dir / "hero.jpg");
    hero << "hero-bytes";
  }
  {
    const fs::path metadata_file = metadata::MetadataFile(server.config(), "celeste");
    fs::create_directories(metadata_file.parent_path());
    std::ofstream meta(metadata_file);
    meta << nlohmann::json{
        {"artwork", {{"file", "cover.jpg"}, {"content_type", "image/jpeg"}, {"source", "steam_cdn"}}},
        {"hero", {{"file", "hero.jpg"}, {"content_type", "image/jpeg"}, {"source", "steam_cdn"}}},
    }.dump();
  }

  httplib::Client client = server.Client();

  auto cover_res = client.Get("/v1/games/celeste/artwork");
  REQUIRE(cover_res != nullptr);
  CHECK(cover_res->status == 200);
  CHECK(cover_res->body == "cover-bytes");

  auto hero_res = client.Get("/v1/games/celeste/artwork?type=hero");
  REQUIRE(hero_res != nullptr);
  CHECK(hero_res->status == 200);
  CHECK(hero_res->body == "hero-bytes");

  auto missing_res = client.Get("/v1/games/celeste/artwork?type=logo");
  REQUIRE(missing_res != nullptr);
  CHECK(missing_res->status == 404);
}

TEST_CASE("GET /v1/runners/{kind}/schema reflects what each runner actually reads out of runner_config") {
  LiveServer server(TempDir("server-runner-schema"));
  httplib::Client client = server.Client();

  auto proton = client.Get("/v1/runners/proton/schema");
  REQUIRE(proton != nullptr);
  CHECK(proton->status == 200);
  CHECK(proton->body.find("\"gameid\"") != std::string::npos);

  // Every kind but proton reads nothing out of runner_config today.
  auto native = client.Get("/v1/runners/native/schema");
  REQUIRE(native != nullptr);
  CHECK(native->status == 200);
  CHECK(native->body == "[]");

  auto bogus = client.Get("/v1/runners/bogus/schema");
  REQUIRE(bogus != nullptr);
  CHECK(bogus->status == 404);
}

TEST_CASE("DELETE /v1/runners/{reference} removes an installed build's directory") {
  const fs::path state_dir = TempDir("server-runner-delete-state");
  const fs::path runners_dir = TempDir("server-runner-delete-runners");
  LiveServer server(state_dir);
  httplib::Client client = server.Client();

  // A minimal directory ProtonRunner::Discover recognizes (proton +
  // toolmanifest.vdf present, version file "<timestamp> <name>").
  const fs::path build_dir = runners_dir / "Fake-Proton-1";
  fs::create_directories(build_dir);
  std::ofstream(build_dir / "proton").close();
  std::ofstream(build_dir / "toolmanifest.vdf").close();
  std::ofstream(build_dir / "version") << "1700000000 Fake-Proton-1";

  auto patched = client.Patch("/v1/config",
                              nlohmann::json{{"runner_search_paths", nlohmann::json::array({runners_dir.string()})}}
                                  .dump(),
                              "application/json");
  REQUIRE(patched != nullptr);
  REQUIRE(patched->status == 200);

  REQUIRE(fs::exists(build_dir));
  auto deleted = client.Delete("/v1/runners/proton:Fake-Proton-1");
  REQUIRE(deleted != nullptr);
  CHECK(deleted->status == 200);
  CHECK_FALSE(fs::exists(build_dir));
}

TEST_CASE("DELETE /v1/runners/{reference} refuses a path outside every configured search root") {
  LiveServer server(TempDir("server-runner-delete-outside"));
  httplib::Client client = server.Client();

  auto listed = client.Get("/v1/runners");
  REQUIRE(listed != nullptr);
  const bool has_system_wine = listed->body.find("\"wine:system\"") != std::string::npos;
  INFO("has_system_wine=", has_system_wine, " (depends on whether this machine has wine installed)");

  // wine:system is discovered via PATH, not wine_search_paths -- must never
  // be deletable, since that's the real system wine binary. Only makes
  // sense to check when there's a real one to try against.
  if (has_system_wine) {
    auto deleted = client.Delete("/v1/runners/wine:system");
    REQUIRE(deleted != nullptr);
    CHECK(deleted->status == 400);
    CHECK(deleted->body.find("path_outside_root") != std::string::npos);
  }
}

TEST_CASE("DELETE /v1/runners/{reference} rejects a kind with no separate builds, or a non-concrete name") {
  LiveServer server(TempDir("server-runner-delete-invalid"));
  httplib::Client client = server.Client();

  auto native = client.Delete("/v1/runners/native:whatever");
  REQUIRE(native != nullptr);
  CHECK(native->status == 400);
  CHECK(native->body.find("not_a_build") != std::string::npos);

  auto auto_ref = client.Delete("/v1/runners/proton:auto");
  REQUIRE(auto_ref != nullptr);
  CHECK(auto_ref->status == 400);
  CHECK(auto_ref->body.find("invalid_reference") != std::string::npos);
}
