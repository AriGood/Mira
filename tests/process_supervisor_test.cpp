#include <doctest.h>
#include <signal.h>
#include <sys/wait.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>

#include "api/EventBus.h"
#include "core/Command.h"
#include "proc/ProcessSupervisor.h"
#include "runner/Exec.h"
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

// Waits up to `timeout` for `predicate()` to become true, polling rather
// than sleeping the whole timeout — these tests spawn real subprocesses, so
// exact timing isn't guaranteed.
bool WaitFor(std::function<bool()> predicate, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return predicate();
}
}  // namespace

TEST_CASE("ProcessSupervisor::Launch runs post_script once the game exits cleanly") {
  const fs::path state = TempDir("proc-post-script-state");
  const fs::path marker = state / "post-ran";

  store::GameStore games(state / "games.toml");
  games.Load();
  api::EventBus events;
  proc::ProcessSupervisor supervisor(games, events, /*stop_timeout_s=*/2);

  model::Game game;
  game.id = "quick-exit";
  REQUIRE(games.Upsert(game).has_value());

  Command command;
  command.argv = {"sh", "-c", "exit 0"};
  const std::string post_script = "touch " + marker.string();
  REQUIRE(supervisor.Launch(game, command, post_script).has_value());

  CHECK(WaitFor([&] { return fs::exists(marker); }, std::chrono::seconds(5)));
  CHECK(WaitFor([&] { return !supervisor.IsRunning("quick-exit"); }, std::chrono::seconds(5)));

  auto stored = games.Find("quick-exit");
  REQUIRE(stored.has_value());
  CHECK(stored->last_error.empty());  // clean exit, not a crash
}

TEST_CASE("ProcessSupervisor::Launch rejects a duplicate launch while one is already running") {
  const fs::path state = TempDir("proc-duplicate-state");
  store::GameStore games(state / "games.toml");
  games.Load();
  api::EventBus events;
  proc::ProcessSupervisor supervisor(games, events);

  model::Game game;
  game.id = "long-running";
  REQUIRE(games.Upsert(game).has_value());

  Command command;
  command.argv = {"sleep", "5"};
  REQUIRE(supervisor.Launch(game, command).has_value());

  const auto second = supervisor.Launch(game, command);
  REQUIRE_FALSE(second.has_value());
  CHECK(second.error().code == "already_running");

  REQUIRE(supervisor.Stop("long-running").has_value());
  CHECK(WaitFor([&] { return !supervisor.IsRunning("long-running"); }, std::chrono::seconds(5)));
}

TEST_CASE("ProcessSupervisor::TrackSteamLaunch detects and tracks a process by SteamAppId, "
         "records playtime, and runs post_script on exit") {
  const fs::path state = TempDir("proc-steam-track-state");
  const fs::path marker = state / "post-ran";

  store::GameStore games(state / "games.toml");
  games.Load();
  api::EventBus events;
  proc::ProcessSupervisor supervisor(games, events);

  model::Game game;
  game.id = "steam-game";
  REQUIRE(games.Upsert(game).has_value());

  // Simulate what Steam itself would have produced: a process carrying
  // SteamAppId in its environment. Not spawned via ProcessSupervisor at all
  // -- exactly the scenario TrackSteamLaunch exists for. In the real
  // scenario this process's parent is the actual Steam client, which reaps
  // it once it exits; here the test itself is the parent (SpawnDetached was
  // called directly, not through Steam), so it has to reap it the same way
  // or the child sits as a zombie forever -- kill(pid, 0) still succeeds
  // for a zombie, which would make AnyAlive() never report it gone.
  Command fake_steam_process;
  fake_steam_process.argv = {"sh", "-c", "sleep 2"};
  fake_steam_process.env["SteamAppId"] = "999999";
  auto pid = runner::SpawnDetached(fake_steam_process);
  REQUIRE(pid.has_value());
  std::thread reaper([pid = *pid] { ::waitpid(pid, nullptr, 0); });
  reaper.detach();

  const std::string post_script = "touch " + marker.string();
  REQUIRE(supervisor.TrackSteamLaunch(game, "999999", post_script).has_value());

  CHECK(WaitFor([&] { return supervisor.IsRunning("steam-game"); }, std::chrono::seconds(5)));
  CHECK(WaitFor(
      [&] {
        auto so_far = events.Since(0);
        return std::ranges::any_of(so_far, [](const model::Event& e) {
          return e.type == "game.state" && e.payload.value("state", "") == "running";
        });
      },
      std::chrono::seconds(5)));

  CHECK(WaitFor([&] { return fs::exists(marker); }, std::chrono::seconds(10)));
  CHECK(WaitFor([&] { return !supervisor.IsRunning("steam-game"); }, std::chrono::seconds(5)));

  auto stored = games.Find("steam-game");
  REQUIRE(stored.has_value());
  CHECK(stored->play_seconds >= 1);
}

TEST_CASE("ProcessSupervisor::Stop refuses a not-yet-confirmed TrackSteamLaunch instead of "
         "signalling pid 0") {
  const fs::path state = TempDir("proc-steam-unconfirmed-state");
  store::GameStore games(state / "games.toml");
  games.Load();
  api::EventBus events;
  proc::ProcessSupervisor supervisor(games, events);

  model::Game game;
  game.id = "never-shows-up";
  REQUIRE(games.Upsert(game).has_value());

  // No process with this appid actually exists -- TrackSteamLaunch reserves
  // the slot immediately (pid 0 sentinel) and only replaces it once
  // detection succeeds, so Stop() called in that window must not try to
  // kill(-0, ...) (every process in mirad's own process group).
  REQUIRE(supervisor.TrackSteamLaunch(game, "111111111").has_value());
  CHECK(supervisor.IsRunning("never-shows-up"));

  const auto stopped = supervisor.Stop("never-shows-up");
  REQUIRE_FALSE(stopped.has_value());
  CHECK(stopped.error().code == "not_yet_confirmed");
}

// The Proton/Wine path is why Stop() can't rely on the process group alone:
// umu-run, wineserver, each winedevice and the game .exe all call
// setsid()/setpgid() during startup, so the group mirad created ends up with
// one member while the game itself runs outside it. Measured against a real
// launch before this was fixed: 1 of 16 processes signalled, the game left
// running and orphaned while mirad reported it exited. FindPrefixProcesses
// is the replacement handle — the prefix every process in the tree
// inherits — so these cover what it must and must not match.

TEST_CASE("FindPrefixProcesses finds a process that left its process group") {
  const fs::path prefix = TempDir("proc-prefix-escaped");

  // setsid() is exactly what wineserver does, and the reason the group is
  // empty by the time anyone asks. The env var is the only thing left
  // tying this process to the game.
  Command command;
  command.argv = {"sh", "-c", "setsid sleep 30 & sleep 30"};
  command.env["WINEPREFIX"] = prefix.string();
  auto pid = runner::SpawnDetached(command);
  REQUIRE(pid.has_value());

  CHECK(WaitFor([&] { return proc::FindPrefixProcesses(prefix.string()).size() >= 2; },
                std::chrono::seconds(5)));

  for (pid_t found : proc::FindPrefixProcesses(prefix.string())) ::kill(found, SIGKILL);
  ::waitpid(*pid, nullptr, 0);
  CHECK(WaitFor([&] { return proc::FindPrefixProcesses(prefix.string()).empty(); },
                std::chrono::seconds(5)));
}

TEST_CASE("FindPrefixProcesses does not match a game whose prefix is a string prefix") {
  // "…/animal" must not stop "…/animal-well". A substring search over
  // /proc/<pid>/environ would, which is why the match is per entry with an
  // explicit separator check.
  const fs::path base = TempDir("proc-prefix-neighbour");
  const fs::path narrow = base / "animal";
  const fs::path wide = base / "animal-well";
  fs::create_directories(narrow);
  fs::create_directories(wide);

  Command command;
  command.argv = {"sh", "-c", "sleep 30"};
  command.env["WINEPREFIX"] = wide.string();
  auto pid = runner::SpawnDetached(command);
  REQUIRE(pid.has_value());

  CHECK(WaitFor([&] { return !proc::FindPrefixProcesses(wide.string()).empty(); },
                std::chrono::seconds(5)));
  CHECK(proc::FindPrefixProcesses(narrow.string()).empty());

  ::kill(-*pid, SIGKILL);
  ::kill(*pid, SIGKILL);
  ::waitpid(*pid, nullptr, 0);
}

TEST_CASE("FindPrefixProcesses matches umu's rewritten WINEPREFIX and an empty one matches nothing") {
  // umu rewrites WINEPREFIX to "<data_dir>/pfx/" before the game runs, so
  // the separator case is the normal case, not an edge one.
  const fs::path data_dir = TempDir("proc-prefix-pfx");
  const fs::path pfx = data_dir / "pfx";
  fs::create_directories(pfx);

  Command command;
  command.argv = {"sh", "-c", "sleep 30"};
  command.env["WINEPREFIX"] = pfx.string() + "/";
  auto pid = runner::SpawnDetached(command);
  REQUIRE(pid.has_value());

  CHECK(WaitFor([&] { return !proc::FindPrefixProcesses(data_dir.string()).empty(); },
                std::chrono::seconds(5)));
  // A game with no prefix at all (a native one) must never sweep up every
  // process on the machine.
  CHECK(proc::FindPrefixProcesses("").empty());

  ::kill(-*pid, SIGKILL);
  ::kill(*pid, SIGKILL);
  ::waitpid(*pid, nullptr, 0);
}
