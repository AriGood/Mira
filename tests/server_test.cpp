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
  config::Config& MutableConfig() { return config_; }

private:
  config::Config config_;
  store::GameStore games_;
  api::EventBus events_;
  fs::path socket_path_;
  api::Server server_;
  std::thread thread_;
};

// Polling POST .../stop is the only externally-visible "has this launch
// actually finished yet" signal available over the API (ProcessSupervisor's
// own IsRunning() is test-only, not exposed to a client) — a 409 means
// ProcessSupervisor no longer considers the game running, i.e. the watcher
// thread already reaped it and recorded the outcome.
bool WaitForExit(httplib::Client& client, const std::string& id,
                 std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    auto res = client.Post(std::format("/v1/games/{}/stop", id));
    if (res && res->status == 200 && res->body.find("not_running") != std::string::npos) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

// Unlike WaitForExit, this never calls stop -- for a test that cares about
// the game's actual output, repeatedly SIGTERMing it as a polling mechanism
// (WaitForExit's approach) races the signal against a fast script's own
// completion and can kill it before it finishes writing anything.
bool WaitForLogContains(httplib::Client& client, const std::string& id, std::string_view needle,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    auto res = client.Get(std::format("/v1/games/{}/log?lines=50", id));
    if (res && res->status == 200 && res->body.find(needle) != std::string::npos) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

std::string LastError(httplib::Client& client, const std::string& id) {
  auto res = client.Get(std::format("/v1/games/{}", id));
  if (!res) return "<no response>";
  const auto body = nlohmann::json::parse(res->body, nullptr, false);
  if (body.is_discarded()) return "<unparseable>";
  return body.value("last_error", std::string());
}

// A small script, since command_wrappers/launch.env behavior needs actual
// env vars visible to a real child process to verify against — not something
// observable from argv alone over HTTP.
fs::path WriteEnvCheckScript(const fs::path& dir, std::string_view expect_foo) {
  fs::create_directories(dir);
  const fs::path script = dir / "check.sh";
  std::ofstream(script) << std::format("#!/bin/sh\n[ \"$FOO\" = \"{}\" ] && exit 0 || exit 9\n", expect_foo);
  return script;
}

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

TEST_CASE("POST /v1/games/{id}/launch refuses a command_wrappers entry that isn't on PATH") {
  LiveServer server(TempDir("server-launch-bad-wrapper"));

  model::Game game;
  game.id = "true-game";
  game.name = "true-game";
  game.platform = model::Platform::Native;
  game.status = model::GameStatus::Ready;
  game.install_path = "/bin";
  game.exe_path = "true";
  game.overrides["command_wrappers"] = nlohmann::json::array({"this-wrapper-does-not-exist-anywhere-xyz"});
  REQUIRE(server.games().Upsert(game).has_value());

  httplib::Client client = server.Client();
  auto res = client.Post("/v1/games/true-game/launch");
  REQUIRE(res != nullptr);
  CHECK(res->status == 400);
  CHECK(res->body.find("wrapper_not_found") != std::string::npos);
}

TEST_CASE("POST /v1/games/{id}/launch splits a multi-token command_wrappers entry into separate argv") {
  LiveServer server(TempDir("server-launch-wrapper-split"));

  model::Game game;
  game.id = "wrapped-game";
  game.name = "wrapped-game";
  game.platform = model::Platform::Native;
  game.status = model::GameStatus::Ready;
  game.install_path = "/bin";
  game.exe_path = "true";
  // If this weren't split on spaces, exec would look for a binary literally
  // named "sh -c true" and fail with exit 127 — a passing "true" here is
  // only possible if ApplyCommandWrappers actually split it into ["sh",
  // "-c", "true"].
  game.overrides["command_wrappers"] = nlohmann::json::array({"sh -c true"});
  REQUIRE(server.games().Upsert(game).has_value());

  httplib::Client client = server.Client();
  auto launched = client.Post("/v1/games/wrapped-game/launch");
  REQUIRE(launched != nullptr);
  CHECK(launched->status == 200);

  REQUIRE(WaitForExit(client, "wrapped-game"));
  CHECK(LastError(client, "wrapped-game").empty());
}

TEST_CASE("POST /v1/games/{id}/launch applies launch.env under the resolved command's own env") {
  LiveServer server(TempDir("server-launch-env"));
  const fs::path install_dir = TempDir("server-launch-env-install");
  WriteEnvCheckScript(install_dir, "from-launch-env");

  model::Game game;
  game.id = "env-game";
  game.name = "env-game";
  game.platform = model::Platform::Native;
  game.status = model::GameStatus::Ready;
  game.install_path = install_dir.string();
  game.exe_path = "check.sh";  // no +x needed: NativeRunner always runs .sh through sh
  game.overrides["launch.env"] = nlohmann::json::array({"FOO=from-launch-env"});
  REQUIRE(server.games().Upsert(game).has_value());

  httplib::Client client = server.Client();
  auto launched = client.Post("/v1/games/env-game/launch");
  REQUIRE(launched != nullptr);
  CHECK(launched->status == 200);

  REQUIRE(WaitForExit(client, "env-game"));
  CHECK(LastError(client, "env-game").empty());
}

TEST_CASE("POST /v1/games/{id}/launch: a game's own env wins over launch.env") {
  LiveServer server(TempDir("server-launch-env-precedence"));
  const fs::path install_dir = TempDir("server-launch-env-precedence-install");
  WriteEnvCheckScript(install_dir, "from-game-env");

  model::Game game;
  game.id = "env-precedence-game";
  game.name = "env-precedence-game";
  game.platform = model::Platform::Native;
  game.status = model::GameStatus::Ready;
  game.install_path = install_dir.string();
  game.exe_path = "check.sh";
  game.env = {{"FOO", "from-game-env"}};
  game.overrides["launch.env"] = nlohmann::json::array({"FOO=from-launch-env"});  // must lose
  REQUIRE(server.games().Upsert(game).has_value());

  httplib::Client client = server.Client();
  auto launched = client.Post("/v1/games/env-precedence-game/launch");
  REQUIRE(launched != nullptr);
  CHECK(launched->status == 200);

  REQUIRE(WaitForExit(client, "env-precedence-game"));
  CHECK(LastError(client, "env-precedence-game").empty());
}

TEST_CASE("POST /v1/games/{id}/launch with launch.gamemode never blocks or fails the launch") {
  LiveServer server(TempDir("server-launch-gamemode"));

  model::Game game;
  game.id = "gamemode-game";
  game.name = "gamemode-game";
  game.platform = model::Platform::Native;
  game.status = model::GameStatus::Ready;
  game.install_path = "/bin";
  game.exe_path = "true";
  game.overrides["launch.gamemode"] = true;
  REQUIRE(server.games().Upsert(game).has_value());

  httplib::Client client = server.Client();
  auto launched = client.Post("/v1/games/gamemode-game/launch");
  REQUIRE(launched != nullptr);
  CHECK(launched->status == 200);

  // Whether a real gamemoded is reachable on this machine or not, the
  // launch itself must always complete cleanly -- GameMode registration is
  // best-effort and must never surface as a launch failure or a crash.
  REQUIRE(WaitForExit(client, "gamemode-game"));
  CHECK(LastError(client, "gamemode-game").empty());
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

TEST_CASE("POST /v1/games/{id}/artwork validates before enqueuing a candidate download") {
  LiveServer server(TempDir("server-artwork-select"));

  model::Game game;
  game.id = "celeste";
  game.name = "Celeste";
  REQUIRE(server.games().Upsert(game).has_value());

  httplib::Client client = server.Client();

  auto missing_game = client.Post("/v1/games/no-such-game/artwork?type=hero", R"({"candidate_id": 1})",
                                  "application/json");
  REQUIRE(missing_game != nullptr);
  CHECK(missing_game->status == 404);

  auto missing_type = client.Post("/v1/games/celeste/artwork", R"({"candidate_id": 1})", "application/json");
  REQUIRE(missing_type != nullptr);
  CHECK(missing_type->status == 400);

  auto bad_body = client.Post("/v1/games/celeste/artwork?type=hero", R"({"candidate_id": "not-a-number"})",
                              "application/json");
  REQUIRE(bad_body != nullptr);
  CHECK(bad_body->status == 400);

  auto ok = client.Post("/v1/games/celeste/artwork?type=hero", R"({"candidate_id": 1})", "application/json");
  REQUIRE(ok != nullptr);
  CHECK(ok->status == 202);
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

TEST_CASE("DELETE /v1/games/{id}?delete_metadata=true removes cached metadata/artwork, "
          "and only those paths") {
  LiveServer server(TempDir("server-delete-metadata"));

  model::Game game;
  game.id = "celeste";
  game.name = "Celeste";
  REQUIRE(server.games().Upsert(game).has_value());

  const fs::path metadata_file = metadata::MetadataFile(server.config(), "celeste");
  const fs::path artwork_dir = metadata::ArtworkDir(server.config(), "celeste");
  fs::create_directories(metadata_file.parent_path());
  std::ofstream(metadata_file) << R"({"artwork": {"file": "cover.png"}})";
  fs::create_directories(artwork_dir);
  std::ofstream(artwork_dir / "cover.png") << "not really a png";

  // A sibling game's own metadata must survive untouched.
  const fs::path sibling_metadata = metadata::MetadataFile(server.config(), "peak");
  std::ofstream(sibling_metadata) << R"({})";

  httplib::Client client = server.Client();
  auto res = client.Delete("/v1/games/celeste?delete_metadata=true");
  REQUIRE(res != nullptr);
  CHECK(res->status == 200);

  CHECK_FALSE(fs::exists(metadata_file));
  CHECK_FALSE(fs::exists(artwork_dir));
  CHECK(fs::exists(sibling_metadata));
}

TEST_CASE("DELETE /v1/games/{id}?purge=true removes files, prefix, and metadata together") {
  LiveServer server(TempDir("server-delete-purge-state"));
  const fs::path library_root = TempDir("server-delete-purge-library");
  const fs::path prefix_root = TempDir("server-delete-purge-prefix");
  REQUIRE(server.MutableConfig().Set("library_roots", nlohmann::json::array({library_root.string()})).has_value());
  REQUIRE(server.MutableConfig().Set("prefix_root", prefix_root.string()).has_value());

  const fs::path install_path = library_root / "Celeste";
  const fs::path data_dir = prefix_root / "celeste";
  fs::create_directories(install_path);
  std::ofstream(install_path / "Celeste.exe") << "fake exe";
  fs::create_directories(data_dir);

  model::Game game;
  game.id = "celeste";
  game.name = "Celeste";
  game.install_path = install_path.string();
  game.data_dir = data_dir.string();
  REQUIRE(server.games().Upsert(game).has_value());

  const fs::path metadata_file = metadata::MetadataFile(server.config(), "celeste");
  fs::create_directories(metadata_file.parent_path());
  std::ofstream(metadata_file) << "{}";

  httplib::Client client = server.Client();
  auto res = client.Delete("/v1/games/celeste?purge=true");
  REQUIRE(res != nullptr);
  CHECK(res->status == 200);

  CHECK_FALSE(fs::exists(install_path));
  CHECK_FALSE(fs::exists(data_dir));
  CHECK_FALSE(fs::exists(metadata_file));
  CHECK_FALSE(server.games().Find("celeste").has_value());
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

TEST_CASE("POST /v1/games/manual adds a ready native game outside any configured library root") {
  LiveServer server(TempDir("server-manual-add-state"));
  const fs::path outside_dir = TempDir("server-manual-add-outside");
  std::ofstream(outside_dir / "game") << "not really an exe";

  httplib::Client client = server.Client();
  const nlohmann::json body = {{"install_path", outside_dir.string()}, {"exe_path", "game"}};
  auto res = client.Post("/v1/games/manual", body.dump(), "application/json");
  REQUIRE(res != nullptr);
  REQUIRE(res->status == 200);

  const auto parsed = nlohmann::json::parse(res->body, nullptr, false);
  CHECK(parsed.value("status", "") == "ready");
  CHECK(parsed.value("platform", "") == "native");
  CHECK(parsed.value("install_path", "") == outside_dir.string());
  CHECK(parsed.value("exe_path", "") == "game");
  CHECK(parsed.value("source", "") == "manual");

  const std::string id = parsed.value("id", "");
  REQUIRE_FALSE(id.empty());
  CHECK(server.games().Find(id).has_value());
}

TEST_CASE("POST /v1/games/manual with is_installer=true creates a needs_install game") {
  LiveServer server(TempDir("server-manual-add-installer-state"));
  const fs::path outside_dir = TempDir("server-manual-add-installer-outside");
  std::ofstream(outside_dir / "Setup.exe") << "not really an installer";

  httplib::Client client = server.Client();
  const nlohmann::json body = {{"install_path", outside_dir.string()}, {"exe_path", "Setup.exe"}, {"is_installer", true}};
  auto res = client.Post("/v1/games/manual", body.dump(), "application/json");
  REQUIRE(res != nullptr);
  REQUIRE(res->status == 200);

  const auto parsed = nlohmann::json::parse(res->body, nullptr, false);
  CHECK(parsed.value("status", "") == "needs_install");
  CHECK(parsed.value("platform", "") == "windows");
  CHECK_FALSE(parsed.value("last_error", "").empty());
}

TEST_CASE("POST /v1/games/manual to the same install_path updates rather than duplicates") {
  LiveServer server(TempDir("server-manual-add-dup-state"));
  const fs::path outside_dir = TempDir("server-manual-add-dup-outside");
  std::ofstream(outside_dir / "game") << "not really an exe";

  httplib::Client client = server.Client();
  const nlohmann::json body = {{"install_path", outside_dir.string()}, {"exe_path", "game"}, {"name", "First Name"}};
  auto first = client.Post("/v1/games/manual", body.dump(), "application/json");
  REQUIRE(first != nullptr);
  REQUIRE(first->status == 200);
  const std::string id = nlohmann::json::parse(first->body).value("id", "");

  const nlohmann::json second_body = {{"install_path", outside_dir.string()}, {"exe_path", "game"}, {"name", "Second Name"}};
  auto second = client.Post("/v1/games/manual", second_body.dump(), "application/json");
  REQUIRE(second != nullptr);
  REQUIRE(second->status == 200);
  const auto parsed = nlohmann::json::parse(second->body, nullptr, false);
  CHECK(parsed.value("id", "") == id);
  CHECK(parsed.value("name", "") == "Second Name");
  CHECK(server.games().All().size() == 1);
}

TEST_CASE("GET /v1/games/{id}/log is an empty list before any launch, not a 404 or 500") {
  LiveServer server(TempDir("server-log-empty"));

  model::Game game;
  game.id = "never-launched";
  game.name = "never-launched";
  REQUIRE(server.games().Upsert(game).has_value());

  httplib::Client client = server.Client();
  auto res = client.Get("/v1/games/never-launched/log");
  REQUIRE(res != nullptr);
  CHECK(res->status == 200);
  const auto body = nlohmann::json::parse(res->body, nullptr, false);
  REQUIRE(body.contains("lines"));
  CHECK(body["lines"].empty());
}

TEST_CASE("POST /v1/games/{id}/launch through mira-run populates GET .../log with the game's own output") {
  LiveServer server(TempDir("server-log-populated"));
  const fs::path install_dir = TempDir("server-log-populated-install");
  const fs::path script = install_dir / "run.sh";
  std::ofstream(script) << "#!/bin/sh\necho hello-from-the-game\n";

  model::Game game;
  game.id = "logged-game";
  game.name = "logged-game";
  game.platform = model::Platform::Native;
  game.status = model::GameStatus::Ready;
  game.install_path = install_dir.string();
  game.exe_path = "run.sh";  // no +x needed, see NativeRunner's .sh handling
  REQUIRE(server.games().Upsert(game).has_value());

  httplib::Client client = server.Client();
  auto launched = client.Post("/v1/games/logged-game/launch");
  REQUIRE(launched != nullptr);
  CHECK(launched->status == 200);

  REQUIRE(WaitForLogContains(client, "logged-game", "hello-from-the-game"));
  auto res = client.Get("/v1/games/logged-game/log?lines=50");
  REQUIRE(res != nullptr);
  CHECK(res->status == 200);
  CHECK(res->body.find("mira-run") != std::string::npos);
}

TEST_CASE("POST /v1/games/{id}/finish-install refuses while exe_path is still the installer") {
  LiveServer server(TempDir("server-finish-install-state"));
  const fs::path dir = TempDir("server-finish-install-game");
  std::ofstream(dir / "setup.exe") << "installer";

  model::Game game;
  game.id = "pending";
  game.name = "Pending";
  game.install_path = dir.string();
  game.exe_path = "setup.exe";
  game.status = model::GameStatus::NeedsInstall;
  game.candidates = {{.rel_path = "setup.exe", .is_installer = true}};
  REQUIRE(server.games().Upsert(game));

  httplib::Client client = server.Client();
  auto res = client.Post("/v1/games/pending/finish-install");
  REQUIRE(res != nullptr);
  CHECK(res->status == 409);

  std::ofstream(dir / "game.exe") << "game";
  REQUIRE(client.Patch("/v1/games/pending", R"({"exe_path": "game.exe"})", "application/json"));
  res = client.Post("/v1/games/pending/finish-install");
  REQUIRE(res != nullptr);
  CHECK(res->status == 200);
}

TEST_CASE("POST /v1/games/{id}/install refuses a game that isn't needs_install") {
  LiveServer server(TempDir("server-install-ready-state"));
  model::Game game;
  game.id = "done";
  game.name = "Done";
  game.status = model::GameStatus::Ready;
  REQUIRE(server.games().Upsert(game));

  httplib::Client client = server.Client();
  auto res = client.Post("/v1/games/done/install");
  REQUIRE(res != nullptr);
  CHECK(res->status == 409);
}

TEST_CASE("GET /v1/games/{id}/installer describes a hand-picked file; progress is idle before any install") {
  LiveServer server(TempDir("server-installer-info-state"));
  const fs::path dir = TempDir("server-installer-info-game");
  std::ofstream(dir / "picked.exe") << "...Inno Setup...";

  model::Game game;
  game.id = "pick";
  game.name = "Pick";
  game.install_path = dir.string();
  game.status = model::GameStatus::NeedsInstall;
  REQUIRE(server.games().Upsert(game));

  httplib::Client client = server.Client();
  auto res = client.Get("/v1/games/pick/installer", httplib::Params{{"path", "picked.exe"}}, httplib::Headers{});
  REQUIRE(res != nullptr);
  REQUIRE(res->status == 200);
  const auto parsed = nlohmann::json::parse(res->body);
  CHECK(parsed.value("format", "") == "inno");
  CHECK(parsed.value("silent", false));

  res = client.Get("/v1/games/pick/install/progress");
  REQUIRE(res != nullptr);
  CHECK(nlohmann::json::parse(res->body).value("state", "") == "idle");
}
