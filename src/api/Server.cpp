#include "api/Server.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <thread>

#include <httplib.h>

#include "config/Resolver.h"
#include "config/Schema.h"
#include "core/Log.h"
#include "core/Strings.h"
#include "desktop/DesktopEntries.h"
#include "library/Catalog.h"
#include "library/AutoInstall.h"
#include "library/PrefixNaming.h"
#include "library/Relocate.h"
#include "library/Scanner.h"
#include "library/SourceRegistry.h"
#include "amazon/AmazonImporter.h"
#include "amazon/Nile.h"
#include "desktop/DesktopEntryScanner.h"
#include "epic/EpicImporter.h"
#include "epic/EpicInstaller.h"
#include "epic/Legendary.h"
#include "gog/Gog.h"
#include "gog/GogImporter.h"
#include "gog/GogInstaller.h"
#include "humble/Humble.h"
#include "itch/Itch.h"
#include "itch/ItchImporter.h"
#include "itch/ItchInstaller.h"
#include "launchers/Launchers.h"
#include "lutris/LutrisImporter.h"
#include "metadata/MetadataFetcher.h"
#include "proc/ProcessSupervisor.h"
#include "proc/Session.h"
#include "runner/Downloader.h"
#include "runner/Exec.h"
#include "runner/GameMode.h"
#include "runner/RunnerRegistry.h"
#include "runner/Winetricks.h"
#include "steam/SteamScanner.h"

namespace mira::api {
namespace {
using nlohmann::json;
using httplib::Request;
using httplib::Response;

json ErrorBody(std::string_view code, std::string_view message) {
  return {{"error", {{"code", code}, {"message", message}}}};
}

void SendError(Response& res, int status, std::string_view code, std::string_view message) {
  res.status = status;
  res.set_content(ErrorBody(code, message).dump(), "application/json");
}

void SendJson(Response& res, json body, int status = 200) {
  res.status = status;
  res.set_content(body.dump(), "application/json");
}

void SendResult(Response& res, const Result<void>& result) {
  if (result) {
    SendJson(res, json::object());
  } else {
    SendError(res, 400, result.error().code, result.error().message);
  }
}

// model::ToJson(game) plus whether it's actually running right now --
// ProcessSupervisor already tracks this live (IsRunning), it just never
// used to be exposed anywhere. Letting a client read this on every
// GET /v1/games refresh is what makes "is this running" self-correcting
// after a reconnect, instead of a value only ever pieced together from a
// stream of events with nothing authoritative to re-sync against.
json GameJson(const model::Game& game, const proc::ProcessSupervisor& supervisor) {
  json body = model::ToJson(game);
  body["running"] = supervisor.IsRunning(game.id);
  return body;
}

model::Game ParseGamePatch(const model::Game& base, const json& patch) {
  model::Game game = base;
  if (patch.contains("name") && patch["name"].is_string()) game.name = patch["name"];
  if (patch.contains("exe_path") && patch["exe_path"].is_string()) game.exe_path = patch["exe_path"];
  if (patch.contains("args") && patch["args"].is_string()) game.args = patch["args"];
  if (patch.contains("working_dir") && patch["working_dir"].is_string()) {
    game.working_dir = patch["working_dir"];
  }
  if (patch.contains("runner_ref") && patch["runner_ref"].is_string()) {
    game.runner_ref = patch["runner_ref"];
  }
  if (patch.contains("data_dir") && patch["data_dir"].is_string()) {
    game.data_dir = patch["data_dir"];
  }
  if (patch.contains("runner_config") && patch["runner_config"].is_object()) {
    game.runner_config.merge_patch(patch["runner_config"]);
  }
  // Replaced wholesale, not merged -- a plain list has no natural per-entry
  // merge semantics the way the env map's null-removes-a-key convention
  // does, so the client sends the full set it wants ({"tags": []} clears).
  if (patch.contains("tags") && patch["tags"].is_array()) {
    game.tags.clear();
    for (const auto& tag : patch["tags"]) {
      if (tag.is_string()) game.tags.push_back(tag.get<std::string>());
    }
  }
  // "env": null clears every entry; "env": {"K": null} removes just K
  // (same null-removes convention as ApplyOverridesPatch below) — merge-only
  // with no way to shrink the map left no way to actually unset a variable
  // once set, or reset it to empty without deleting and recreating the game.
  if (patch.contains("env") && patch["env"].is_null()) {
    game.env.clear();
  } else if (patch.contains("env") && patch["env"].is_object()) {
    for (const auto& [key, value] : patch["env"].items()) {
      if (value.is_null()) {
        game.env.erase(key);
      } else if (value.is_string()) {
        game.env[key] = value.get<std::string>();
      }
    }
  }
  game.reviewed = true;  // any correction counts as the human having looked
  return game;
}

// Applies a per-game config-override patch: flat {"dotted.key": value, ...},
// a null value removing that override. This is the only place overrides are
// read or written — /v1/games/{id} PATCH deals with the game's own fields
// (name, exe_path, ...) exclusively, never global-setting overrides, so the
// two are never mixed in one request body.
void ApplyOverridesPatch(model::Game& game, const json& patch) {
  for (const auto& [key, value] : patch.items()) {
    if (value.is_null()) {
      game.overrides.erase(key);
    } else {
      game.overrides[key] = value;
    }
  }
}

// Validated before ApplyOverridesPatch so a bad key or value rejects the
// whole patch before anything is written, matching config::Config::Patch's
// all-or-nothing behaviour.
std::optional<std::string> ValidateOverridesPatch(const json& patch) {
  for (const auto& [key, value] : patch.items()) {
    if (value.is_null()) continue;  // removal; nothing to validate
    if (!config::Resolver::IsOverridable(key)) {
      return std::format("\"{}\" cannot be overridden per game", key);
    }
    if (auto problem = config::Schema::Instance().Validate(key, value)) {
      return std::format("{}: {}", key, *problem);
    }
  }
  return std::nullopt;
}

// Wraps the launch command in each configured wrapper, in order: the first
// entry ends up outermost, so ["gamemoderun", "gamescope -W 1920 -H 1080"]
// runs `gamemoderun gamescope -W 1920 -H 1080 <game>`. Each entry is split on
// spaces, like game.args (see NativeRunner.cpp) -- no shell quoting support.
void ApplyCommandWrappers(Command& command, const std::vector<std::string>& wrappers) {
  for (auto it = wrappers.rbegin(); it != wrappers.rend(); ++it) {
    if (it->empty()) continue;
    const std::vector<std::string> tokens = strings::Split(*it, ' ');
    command.argv.insert(command.argv.begin(), tokens.begin(), tokens.end());
  }
}

// Checked before a launch actually spawns anything, so a wrapper that isn't
// installed is a clear 400 naming it, instead of the whole launch silently
// failing with exit code 127 from inside the outermost wrapper.
Result<void> CheckCommandWrappers(const std::vector<std::string>& wrappers) {
  for (const std::string& entry : wrappers) {
    if (entry.empty()) continue;
    const std::vector<std::string> tokens = strings::Split(entry, ' ');
    if (tokens.empty()) continue;
    if (!runner::FindOnPath(tokens[0])) {
      return Err("wrapper_not_found",
                std::format("command_wrappers entry \"{}\" is not on PATH", tokens[0]));
    }
  }
  return {};
}

// launch.env, applied under whatever BuildCommand already set (game.env
// merged in by the runner always wins, same "global default, per-game
// override" contract command_wrappers already has).
void ApplyLaunchEnv(Command& command, const std::vector<std::string>& entries) {
  for (const std::string& entry : entries) {
    const auto eq = entry.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = entry.substr(0, eq);
    if (!command.env.contains(key)) command.env[key] = entry.substr(eq + 1);
  }
}

// Runs launch.pre_script synchronously and blocks the request -- the Steam
// URL-handoff branch and the no-mira-run fallback both still need this; the
// normal wrapped path runs it inside mira-run instead, which is what lets
// it survive mirad dying mid-launch.
Result<void> RunPreScriptInline(const std::string& pre_script) {
  if (pre_script.empty()) return {};
  Command script;
  script.argv = {"sh", "-c", pre_script};
  const Result<runner::ExecResult> ran = runner::RunAndWait(script);
  if (!ran || ran->exit_code != 0) {
    return Err("pre_launch_failed", !ran ? ran.error().message
                                          : std::format("launch.pre_script exited {}: {}", ran->exit_code,
                                                        ran->output));
  }
  return {};
}

// mirad's own binary directory, via /proc/self/exe. Empty on failure;
// runner::ResolveSiblingBinary falls back to $PATH in that case.
std::filesystem::path OwnBinaryDir() {
  std::error_code ec;
  const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
  return ec ? std::filesystem::path() : exe.parent_path();
}

struct WrapperStatus {
  bool ok = false;
  bool read_timed_out = false;  // mirad's own read deadline, distinct from mira-run's launch.pre_timeout_s
  std::string code;             // "ok" / "pre_failed" / "pre_timeout"
  std::string detail;           // session path (ok) or the pre script's captured output (pre_failed)
};

// Blocks on mira-run's status pipe until it writes something and closes it,
// or `timeout_s` passes. Distinct from launch.pre_timeout_s expiring
// (mira-run's own budget for the script, reported as "pre_timeout" below);
// this is a hard ceiling on mira-run answering at all.
WrapperStatus ReadWrapperStatus(int fd, int timeout_s) {
  WrapperStatus result;
  std::string buffer;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
  char chunk[4096];
  while (std::chrono::steady_clock::now() < deadline && buffer.size() < 65536) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
    const int rc = ::poll(&pfd, 1, static_cast<int>(std::max<std::chrono::milliseconds::rep>(0, remaining.count())));
    if (rc <= 0) break;  // timed out, or poll itself failed
    const ssize_t n = ::read(fd, chunk, sizeof(chunk));
    if (n <= 0) break;  // EOF: mira-run closed its end after writing everything
    buffer.append(chunk, static_cast<std::size_t>(n));
  }
  const auto newline = buffer.find('\n');
  if (newline == std::string::npos) {
    result.read_timed_out = true;
    return result;
  }
  result.code = buffer.substr(0, newline);
  result.detail = buffer.substr(newline + 1);
  // For "ok" this is the session path, itself followed by mira-run's own
  // trailing newline -- strip it, or the path never matches the real file.
  while (!result.detail.empty() && (result.detail.back() == '\n' || result.detail.back() == '\r')) {
    result.detail.pop_back();
  }
  result.ok = (result.code == "ok");
  return result;
}

// Same check GET /v1/games/{id}/artwork's default "cover" slot uses to
// decide between serving a file and 404ing — reused here so "missing
// artwork" means the same thing to both endpoints.
bool HasCachedArtwork(const config::Config& config, const std::string& id) {
  const std::filesystem::path metadata_file = metadata::MetadataFile(config, id);
  std::ifstream meta_in(metadata_file);
  if (!meta_in) return false;
  const json info = json::parse(meta_in, nullptr, false);
  if (info.is_discarded() || !info.contains("artwork")) return false;
  const std::filesystem::path file =
      metadata::ArtworkDir(config, id) / info["artwork"].value("file", std::string());
  return std::ifstream(file, std::ios::binary).good();
}

void SyncDesktopEntries(config::Config& config, store::GameStore& games) {
  if (auto synced = desktop::DesktopEntries(config).Sync(games.All()); !synced) {
    log::Warn("could not update application menu entries: {}", synced.error().message);
  }
}

// Deletes `target` only if it's non-empty and really resolves inside one of
// `roots` — never wherever a game's install_path/data_dir field happens to
// say, in case a hand-edited games.toml points somewhere it shouldn't. A
// symlinked target is resolved with weakly_canonical before the containment
// check, so a symlink can't be used to delete outside a root either.
Result<void> DeleteUnderRoot(const std::string& target, const std::vector<std::filesystem::path>& roots) {
  if (target.empty()) return Err("nothing_to_delete", "this game has no such path recorded");
  std::error_code ec;
  const std::filesystem::path resolved = std::filesystem::weakly_canonical(target, ec);
  if (ec) return Err("path_error", ec.message());

  const bool contained = std::ranges::any_of(roots, [&](const std::filesystem::path& root) {
    const std::filesystem::path canon_root = std::filesystem::weakly_canonical(root, ec);
    if (ec) return false;
    const auto [root_end, nothing] = std::mismatch(canon_root.begin(), canon_root.end(), resolved.begin());
    return root_end == canon_root.end();
  });
  if (!contained) {
    return Err("path_outside_root", std::format("\"{}\" is not inside a configured root — refusing to delete", target));
  }

  std::filesystem::remove_all(resolved, ec);
  if (ec) return Err("delete_failed", ec.message());
  return {};
}

}  // namespace

Server::Server(config::Config& config, store::GameStore& games, EventBus& events)
    : config_(config),
      games_(games),
      events_(events),
      http_(std::make_unique<httplib::Server>()),
      supervisor_(games, events, config.GetInt("launch.stop_timeout_s")) {}

Server::~Server() {
  stopping_.store(true, std::memory_order_relaxed);
  if (launcher_watch_.joinable()) launcher_watch_.join();
}

// A game started from inside a running launcher (not through Mira) is
// picked up here, so it still shows as playing and its playtime counts.
void Server::WatchLauncherGames() {
  constexpr auto kTick = std::chrono::milliseconds(500);
  constexpr int kTicksPerScan = 6;
  for (int tick = 0; !stopping_.load(std::memory_order_relaxed); ++tick) {
    std::this_thread::sleep_for(kTick);
    if (tick % kTicksPerScan != 0) continue;
    for (const launchers::Launcher& launcher : launchers::All()) {
      const auto host = games_.Find(launchers::GameId(launcher));
      if (!host || host->data_dir.empty() || proc::FindPrefixProcesses(host->data_dir).empty()) continue;
      for (const model::Game& game : games_.All()) {
        if (game.source != launcher.id || supervisor_.IsRunning(game.id)) continue;
        const std::string win_dir = launchers::WindowsDir(game);
        if (proc::FindDirProcesses(game.data_dir, win_dir).empty()) continue;
        const config::Resolver resolver(config_, game.overrides);
        if (supervisor_.TrackLauncherLaunch(game, win_dir, 10, resolver.GetString("launch.post_script"))) {
          events_.Publish("game.launched", {{"id", game.id}, {"via", "launcher"}, {"tracked", true}});
        }
      }
    }
  }
}

void Server::ReconcileSessions() {
  supervisor_.Reconcile(games_.Dir() / "sessions");
  // A client that stayed open across a restart may still show games from
  // the old daemon as running.
  for (const model::Game& game : games_.All()) {
    if (supervisor_.IsRunning(game.id)) continue;
    json event = GameJson(game, supervisor_);
    event["state"] = "idle";
    events_.Publish("game.state", std::move(event));
  }
}

Result<void> Server::Serve(const std::filesystem::path& socket_path) {
  std::error_code ec;
  std::filesystem::create_directories(socket_path.parent_path(), ec);
  std::filesystem::remove(socket_path, ec);  // clear a stale socket from an unclean shutdown

  RegisterRoutes();

  http_->set_address_family(AF_UNIX);
  if (!http_->bind_to_port(socket_path.string(), 80)) {
    return Err("socket_bind_failed", std::format("cannot bind {}", socket_path.string()));
  }
  std::filesystem::permissions(socket_path, std::filesystem::perms::owner_read |
                                                std::filesystem::perms::owner_write,
                               ec);

  log::Info("listening on {}", socket_path.string());
  launcher_watch_ = std::thread(&Server::WatchLauncherGames, this);
  if (!http_->listen_after_bind()) {
    return Err("socket_listen_failed", "httplib server exited unexpectedly");
  }
  return {};
}

void Server::Stop() {
  stopping_.store(true, std::memory_order_relaxed);
  http_->stop();
}

void Server::RegisterRoutes() {
  http_->Get("/v1/health", [](const Request&, Response& res) {
    SendJson(res, {{"status", "ok"}});
  });

  // For the frontend to warn about a launch.gamemode = true that won't
  // actually do anything -- "installed" and "daemon_running" are reported
  // separately since they're different problems (not installed at all, vs
  // installed but the daemon isn't up right now).
  http_->Get("/v1/gamemode/status", [](const Request&, Response& res) {
    SendJson(res, {{"installed", gamemode::IsInstalled()}, {"daemon_running", gamemode::IsDaemonRunning()}});
  });

  // --- settings -----------------------------------------------------------

  http_->Get("/v1/config", [this](const Request&, Response& res) {
    json body = config_.Document();
    body["frontend"] = config_.FrontendSettings();
    SendJson(res, std::move(body));
  });

  http_->Get("/v1/config/schema", [](const Request&, Response& res) {
    json entries = json::array();
    for (const config::Entry& entry : config::Schema::Instance().Entries()) {
      entries.push_back({
          {"key", entry.key},
          {"label", entry.label},
          {"type", config::ToString(entry.type)},
          {"default", entry.default_value},
          {"scope", config::ToString(entry.scope)},
          {"doc", entry.doc},
          {"category", entry.category},
          {"group", entry.group},
      });
      // Present only when there's a shape to describe.
      if (!entry.constraint.one_of.empty()) entries.back()["one_of"] = entry.constraint.one_of;
      if (entry.constraint.minimum) entries.back()["minimum"] = *entry.constraint.minimum;
      if (entry.constraint.maximum) entries.back()["maximum"] = *entry.constraint.maximum;
      if (entry.is_secret) entries.back()["is_secret"] = true;
      if (entry.is_runner_ref) entries.back()["is_runner_ref"] = true;
    }
    SendJson(res, std::move(entries));
  });

  http_->Patch("/v1/config", [this](const Request& req, Response& res) {
    json patch = json::parse(req.body, nullptr, false);
    if (patch.is_discarded()) return SendError(res, 400, "invalid_json", "body is not valid JSON");
    Result<void> result = config_.Patch(patch);
    if (result) SyncDesktopEntries(config_, games_);
    SendResult(res, result);
  });

  http_->Post("/v1/config/reset", [this](const Request& req, Response& res) {
    Result<void> result;
    if (auto it = req.params.find("key"); it != req.params.end()) {
      result = config_.Reset(it->second);
    } else {
      config_.ResetAll();
      result = config_.Save();
    }
    if (result) SyncDesktopEntries(config_, games_);
    SendResult(res, result);
  });

  // --- games ----------------------------------------------------------------

  // "hidden" isn't a separate field — it's a tag (see model::Game::tags),
  // and the one tag this endpoint treats specially: a hidden game is left
  // out of the default/untagged list, same as it'd be hidden in a launcher
  // UI, without a whole extra field+schema entry for one boolean. Pass
  // ?tag=hidden explicitly to list exactly the hidden ones.
  http_->Get("/v1/games", [this](const Request& req, Response& res) {
    std::vector<model::Game> all = games_.All();
    json out = json::array();
    const auto status_filter = req.params.find("status");
    const auto tag_filter = req.params.find("tag");
    for (const model::Game& game : all) {
      if (status_filter != req.params.end() &&
          status_filter->second != model::ToString(game.status)) {
        continue;
      }
      if (tag_filter != req.params.end()) {
        if (!std::ranges::contains(game.tags, tag_filter->second)) continue;
      } else if (std::ranges::contains(game.tags, std::string("hidden"))) {
        continue;
      }
      out.push_back(GameJson(game, supervisor_));
    }
    SendJson(res, std::move(out));
  });

  http_->Get(R"(/v1/games/([^/]+))", [this](const Request& req, Response& res) {
    auto game = games_.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    SendJson(res, GameJson(*game, supervisor_));
  });

  // The tail of the log mira-run writes for this game (src/wrapper/main.cpp):
  // the game's own stdout/stderr, plus mira-run's own annotated pre/post
  // script output and exit summary — one file that explains a session, not
  // just a status badge. A game that's never been launched through the
  // wrapper (or was launched via the no-mira-run fallback) simply has no
  // log file yet — reported as an empty list, not a 404 or 500, since "no
  // log" is a completely ordinary state.
  http_->Get(R"(/v1/games/([^/]+)/log)", [this](const Request& req, Response& res) {
    auto game = games_.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");

    int requested_lines = 200;
    if (auto it = req.params.find("lines"); it != req.params.end()) {
      requested_lines = std::max(1, std::atoi(it->second.c_str()));
    }

    const std::filesystem::path log_file = games_.Dir() / "logs" / std::format("{}.log", game->id);
    std::ifstream in(log_file, std::ios::binary);
    if (!in) return SendJson(res, {{"lines", json::array()}});

    // Bounded read from the end, not the whole file -- launch.log_max_mb
    // can be configured up to 1GB, and this endpoint only ever needs a
    // handful of recent lines.
    constexpr std::streamoff kMaxTailBytes = 4 * 1024 * 1024;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(size > kMaxTailBytes ? size - kMaxTailBytes : 0);
    const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    std::vector<std::string> all_lines = strings::Split(content, '\n');
    if (!all_lines.empty() && all_lines.back().empty()) all_lines.pop_back();  // trailing newline
    const std::size_t take = std::min(all_lines.size(), static_cast<std::size_t>(requested_lines));
    json out = json::array();
    for (std::size_t i = all_lines.size() - take; i < all_lines.size(); ++i) out.push_back(all_lines[i]);
    SendJson(res, {{"lines", out}});
  });

  http_->Patch(R"(/v1/games/([^/]+))", [this](const Request& req, Response& res) {
    const std::string id = req.matches[1];
    json patch = json::parse(req.body, nullptr, false);
    if (patch.is_discarded()) return SendError(res, 400, "invalid_json", "body is not valid JSON");

    auto result = games_.Update(id, [&](model::Game& game) { game = ParseGamePatch(game, patch); });
    if (!result) return SendError(res, 404, result.error().code, result.error().message);
    SyncDesktopEntries(config_, games_);
    events_.Publish("game.updated", GameJson(*result, supervisor_));
    SendJson(res, GameJson(*result, supervisor_));
  });

  // Everything about how this game's *global settings* are overridden lives
  // under /config, mirroring GET/PATCH /v1/config itself: GET resolves every
  // key through default -> settings.toml -> this game's overrides, tagged
  // with which layer supplied it; PATCH sets or (with a null value) removes
  // overrides, flat {"dotted.key": value}. Deliberately not part of the
  // plain PATCH /v1/games/{id} above — a game's own fields (name, exe_path,
  // ...) and its overrides of unrelated global settings are different
  // concerns and don't belong in the same request body.
  http_->Get(R"(/v1/games/([^/]+)/config)", [this](const Request& req, Response& res) {
    auto game = games_.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    config::Resolver resolver(config_, game->overrides);
    SendJson(res, resolver.EffectiveDocument());
  });

  http_->Patch(R"(/v1/games/([^/]+)/config)", [this](const Request& req, Response& res) {
    const std::string id = req.matches[1];
    json patch = json::parse(req.body, nullptr, false);
    if (patch.is_discarded() || !patch.is_object()) {
      return SendError(res, 400, "invalid_json", "expected a flat {\"dotted.key\": value} object");
    }
    if (auto problem = ValidateOverridesPatch(patch)) {
      return SendError(res, 400, "invalid_setting", *problem);
    }
    auto result =
        games_.Update(id, [&](model::Game& game) { ApplyOverridesPatch(game, patch); });
    if (!result) return SendError(res, 404, result.error().code, result.error().message);
    // A per-game override can flip desktop_entries.enabled off for just this
    // game — every other mutation path syncs already (PATCH /v1/config,
    // PATCH /v1/games/{id}, DELETE /v1/games/{id}, ...); this one didn't,
    // so a game's own .desktop entry never got removed until something else
    // happened to trigger a sync.
    SyncDesktopEntries(config_, games_);
    SendJson(res, GameJson(*result, supervisor_));
  });

  http_->Delete(R"(/v1/games/([^/]+))", [this](const Request& req, Response& res) {
    auto game = games_.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");

    const bool purge = req.has_param("purge") && req.get_param_value("purge") == "true";
    const bool delete_files =
        purge || (req.has_param("delete_files") && req.get_param_value("delete_files") == "true");
    const bool delete_prefix =
        purge || (req.has_param("delete_prefix") && req.get_param_value("delete_prefix") == "true");
    const bool delete_metadata =
        purge || (req.has_param("delete_metadata") && req.get_param_value("delete_metadata") == "true");

    // Opt-in, and deliberately narrow: only ever deletes a path this game's
    // own record points at, and only if that path is really inside a
    // configured root — never wherever install_path/data_dir happen to say,
    // in case a hand-edited games.toml points somewhere it shouldn't.
    if (delete_files && game->source == "epic" && !game->source_ref.empty()) {
      // Legendary owns this install's manifest bookkeeping — uninstalling
      // through it instead of a plain directory delete keeps that manifest
      // in sync, so a later `legendary list-installed` doesn't still think
      // this title is here (and broken).
      if (auto uninstalled = epic::RunLegendary(config_, {"uninstall", game->source_ref, "-y"}); !uninstalled) {
        return SendError(res, 400, uninstalled.error().code, uninstalled.error().message);
      }
    } else if (delete_files) {
      if (auto deleted = DeleteUnderRoot(game->install_path, config_.GetPathArray("library_roots")); !deleted) {
        return SendError(res, 400, deleted.error().code, deleted.error().message);
      }
    }
    if (delete_prefix) {
      if (auto deleted = DeleteUnderRoot(game->data_dir, {config_.GetPath("prefix_root")}); !deleted) {
        return SendError(res, 400, deleted.error().code, deleted.error().message);
      }
    }
    if (delete_metadata) {
      // Metadata/artwork live under Mira's own ~/.config/mira tree, keyed
      // by game id — not a user-configured root, so no containment check
      // is needed the way library_roots/prefix_root's is. Best effort: a
      // cache file that was never written or already gone isn't an error.
      std::error_code ec;
      std::filesystem::remove(metadata::MetadataFile(config_, game->id), ec);
      if (ec) log::Warn("could not remove metadata for {}: {}", game->id, ec.message());
      std::filesystem::remove_all(metadata::ArtworkDir(config_, game->id), ec);
      if (ec) log::Warn("could not remove artwork for {}: {}", game->id, ec.message());
    }

    auto result = games_.Remove(req.matches[1]);
    if (!result) return SendError(res, 404, result.error().code, result.error().message);
    SyncDesktopEntries(config_, games_);
    events_.Publish("game.removed", {{"id", req.matches[1].str()}});
    SendJson(res, json::object());
  });

  // --- library ------------------------------------------------------------

  // Manages library_roots (already a plain config array — see GET/PATCH
  // /v1/config) is deliberately not duplicated here; this is the one library
  // endpoint that does real work beyond reading/writing settings. It runs
  // synchronously rather than returning a job id: there is no worker/job
  // queue yet (see docs/architecture.md), and a scan of a normal-sized
  // library completes well within an HTTP request.
  http_->Post("/v1/library/scan", [this](const Request&, Response& res) {
    library::Scanner scanner(config_, games_, events_);
    const library::ScanSummary summary = scanner.ScanAll();
    for (const model::Game& game : summary.added_games) metadata_fetches_.Enqueue(config_, events_, game);
    SendJson(res, {{"added", summary.added}, {"missing", summary.missing},
                   {"restored", summary.restored}});
  });

  // Runs library::Relocate across every tracked game, moving each into
  // Mira's canonical layout (see the per-game /relocate route's own
  // comment) — what "changed prefix_root/prefix_naming, now migrate what's
  // already on disk" actually means: neither setting alone moves anything.
  // Explicit-trigger only, same as the per-game route.
  http_->Post("/v1/library/relocate", [this](const Request&, Response& res) {
    int moved = 0;
    int failed = 0;
    for (const model::Game& game : games_.All()) {
      auto relocated = library::Relocate(config_, game);
      if (!relocated) {
        ++failed;
        log::Warn("relocate failed for {}: {}", game.id, relocated.error().message);
        continue;
      }
      if (relocated->install_path == game.install_path && relocated->data_dir == game.data_dir) continue;
      auto saved = games_.Update(game.id, [&](model::Game& g) {
        g.install_path = relocated->install_path;
        g.data_dir = relocated->data_dir;
        g.updated_at = model::NowSeconds();
      });
      if (!saved) {
        ++failed;
        continue;
      }
      events_.Publish("game.updated", GameJson(*saved, supervisor_));
      ++moved;
    }
    SyncDesktopEntries(config_, games_);
    SendJson(res, {{"moved", moved}, {"failed", failed}});
  });

  // --- steam ------------------------------------------------------------

  // Detected apps land in the same GameStore as everything else (see
  // SteamScanner's class comment) — no separate GET endpoint needed, they
  // just show up in GET /v1/games with runner_ref "steam:<appid>".
  http_->Post("/v1/steam/scan", [this](const Request&, Response& res) {
    steam::SteamScanner scanner(config_, games_, events_);
    auto summary = scanner.Scan();
    if (!summary) return SendError(res, 404, summary.error().code, summary.error().message);
    SyncDesktopEntries(config_, games_);
    for (const model::Game& game : summary->added_games) metadata_fetches_.Enqueue(config_, events_, game);
    SendJson(res, {{"added", summary->added}, {"updated", summary->updated}});
  });

  // --- lutris -----------------------------------------------------------

  // Imported games land in the same GameStore as everything else (see
  // LutrisImporter's class comment) — no separate GET endpoint needed, they
  // just show up in GET /v1/games.
  http_->Post("/v1/lutris/import", [this](const Request&, Response& res) {
    lutris::LutrisImporter importer(config_, games_, events_);
    auto summary = importer.Import();
    if (!summary) return SendError(res, 404, summary.error().code, summary.error().message);
    SyncDesktopEntries(config_, games_);
    for (const model::Game& game : summary->added_games) metadata_fetches_.Enqueue(config_, events_, game);
    SendJson(res, {{"added", summary->added}, {"updated", summary->updated}, {"skipped", summary->skipped}});
  });

  // --- epic -------------------------------------------------------------
  //
  // Wraps Legendary, a native-Linux Epic Games Store CLI client, for
  // everything protocol-shaped (auth, catalog, install/update). Mira never
  // runs Legendary's own `legendary launch` — an installed Epic game is
  // launched through Mira's own Wine/Proton runners like any other Windows
  // game (see docs/architecture.md). Nothing here assumes legendary is
  // already on the system — GET .../legendary/status is always safe to
  // call first, and POST .../legendary/install is how Mira gets it there.

  http_->Get("/v1/epic/legendary/status", [this](const Request&, Response& res) {
    const epic::LegendaryStatus status = epic::DetectLegendary(config_);
    SendJson(res, {{"installed", status.installed},
                  {"source", status.source},
                  {"path", status.path},
                  {"version", status.version}});
  });

  // Downloads and installs Legendary's latest matching GitHub release —
  // detached, same "don't block the request thread on a slow download"
  // pattern as POST /v1/runners/download above; epic.legendary.install.*
  // on the event stream is how a caller finds out it's done. Re-running
  // this later re-fetches the latest release, which is also how staying
  // up to date works — no separate update endpoint.
  http_->Post("/v1/epic/legendary/install", [this](const Request&, Response& res) {
    auto releases = runner::ListReleases(config_, "legendary");
    if (!releases) return SendError(res, 502, releases.error().code, releases.error().message);
    if (releases->empty()) return SendError(res, 404, "no_release_found", "no matching legendary release found");

    const runner::ReleaseAsset asset = releases->front();  // newest first
    events_.Publish("epic.legendary.install.started", {{"tag", asset.tag}});
    std::thread([this, asset] {
      if (auto installed = epic::InstallLegendaryBinary(config_, asset); !installed) {
        log::Error("legendary install failed ({}): {}", asset.tag, installed.error().message);
        events_.Publish("epic.legendary.install.failed", {{"tag", asset.tag}, {"error", installed.error().message}});
      } else {
        log::Info("installed legendary {}", asset.tag);
        events_.Publish("epic.legendary.install.finished", {{"tag", asset.tag}});
      }
    }).detach();

    SendJson(res, {{"status", "downloading"}, {"tag", asset.tag}}, 202);
  });

  // The one call to know the whole picture: is legendary installed, and are
  // we authenticated. Never errors — both are just fields on the result.
  http_->Get("/v1/epic/status", [this](const Request&, Response& res) {
    const epic::EpicAuthStatus status = epic::Status(config_);
    SendJson(res, {{"legendary", {{"installed", status.legendary.installed},
                                  {"source", status.legendary.source},
                                  {"path", status.legendary.path},
                                  {"version", status.legendary.version}}},
                  {"authenticated", status.authenticated},
                  {"account", status.account},
                  {"login_url", epic::kLoginUrl}});
  });

  // The user pastes back the code shown at epic::kLoginUrl, visited in
  // their own browser -- mirad itself never opens one (see Legendary.h).
  http_->Post("/v1/epic/auth", [this](const Request& req, Response& res) {
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("code") || !body["code"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"code": "..."})");
    }
    if (auto logged_in = epic::Login(config_, body["code"]); !logged_in) {
      return SendError(res, 400, logged_in.error().code, logged_in.error().message);
    }
    const epic::EpicAuthStatus status = epic::Status(config_);
    SendJson(res, {{"authenticated", status.authenticated}, {"account", status.account}});
  });

  http_->Post("/v1/epic/logout", [this](const Request&, Response& res) {
    if (auto logged_out = epic::Logout(config_); !logged_out) {
      return SendError(res, 400, logged_out.error().code, logged_out.error().message);
    }
    SendJson(res, {{"status", "logged_out"}});
  });

  // Imported games land in the same GameStore as everything else — no
  // separate GET endpoint needed, they just show up in GET /v1/games.
  http_->Post("/v1/epic/import", [this](const Request&, Response& res) {
    epic::EpicImporter importer(config_, games_, events_);
    auto summary = importer.Import();
    if (!summary) return SendError(res, 404, summary.error().code, summary.error().message);
    SyncDesktopEntries(config_, games_);
    for (const model::Game& game : summary->added_games) metadata_fetches_.Enqueue(config_, events_, game);
    SendJson(res, {{"added", summary->added}, {"updated", summary->updated}});
  });

  // --- gog ----------------------------------------------------------------
  //
  // Wraps gogdl (Heroic's GOG downloader) for auth and install/update.
  // Unlike Legendary, gogdl has no catalog/status subcommand of its own —
  // GET /v1/gog/status only ever reports whether gogdl itself is
  // installed and whether Mira has a stored, unexpired token; catalog
  // listing (GET /v1/library?source=gog) talks to GOG's own embed.gog.com
  // API directly (see src/gog/GogSource.cpp). Mira never runs GOG Galaxy —
  // an installed GOG game launches through Mira's own Wine/Proton runners
  // like any other Windows game, or natively when GOG shipped a Linux
  // build.

  http_->Get("/v1/gog/status", [this](const Request&, Response& res) {
    const gog::GogAuthStatus status = gog::Status(config_);
    SendJson(res, {{"gogdl", {{"installed", status.gogdl.installed},
                              {"source", status.gogdl.source},
                              {"path", status.gogdl.path},
                              {"version", status.gogdl.version}}},
                  {"authenticated", status.authenticated},
                  {"login_url", gog::kLoginUrl}});
  });

  http_->Post("/v1/gog/setup", [this](const Request&, Response& res) {
    auto releases = runner::ListReleases(config_, "gog");
    if (!releases) return SendError(res, 502, releases.error().code, releases.error().message);
    if (releases->empty()) return SendError(res, 404, "no_release_found", "no matching gogdl release found");

    const runner::ReleaseAsset asset = releases->front();
    events_.Publish("gog.setup.started", {{"tag", asset.tag}});
    std::thread([this, asset] {
      if (auto installed = gog::InstallGogBinary(config_, asset); !installed) {
        log::Error("gogdl install failed ({}): {}", asset.tag, installed.error().message);
        events_.Publish("gog.setup.failed", {{"tag", asset.tag}, {"error", installed.error().message}});
      } else {
        log::Info("installed gogdl {}", asset.tag);
        events_.Publish("gog.setup.finished", {{"tag", asset.tag}});
      }
    }).detach();

    SendJson(res, {{"status", "downloading"}, {"tag", asset.tag}}, 202);
  });

  // The user pastes back the "code" query param from the GOG login-success
  // redirect URL — mirad itself never opens a browser (same posture as
  // /v1/epic/auth).
  http_->Post("/v1/gog/auth", [this](const Request& req, Response& res) {
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("code") || !body["code"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"code": "..."})");
    }
    if (auto logged_in = gog::Login(config_, body["code"]); !logged_in) {
      return SendError(res, 400, logged_in.error().code, logged_in.error().message);
    }
    const gog::GogAuthStatus status = gog::Status(config_);
    SendJson(res, {{"authenticated", status.authenticated}});
  });

  http_->Post("/v1/gog/logout", [this](const Request&, Response& res) {
    if (auto logged_out = gog::Logout(config_); !logged_out) {
      return SendError(res, 400, logged_out.error().code, logged_out.error().message);
    }
    SendJson(res, {{"status", "logged_out"}});
  });

  // Unlike epic/steam/itch, this doesn't scan for already-installed
  // titles system-wide — gogdl has no such concept (see gog/Gog.h's class
  // comment). It re-identifies whatever's already under gog.install_root,
  // which is what GogInstaller itself installs into.
  http_->Post("/v1/gog/import", [this](const Request&, Response& res) {
    gog::GogImporter importer(config_, games_, events_);
    auto summary = importer.Import();
    if (!summary) return SendError(res, 404, summary.error().code, summary.error().message);
    SyncDesktopEntries(config_, games_);
    for (const model::Game& game : summary->added_games) metadata_fetches_.Enqueue(config_, events_, game);
    SendJson(res, {{"added", summary->added}, {"updated", summary->updated}});
  });

  // --- amazon -------------------------------------------------------------
  //
  // Wraps nile (Heroic's Amazon Games client) for login, library and
  // downloads; installing goes through /v1/library/install like the other
  // stores. Installed games run through Mira's own runners.

  http_->Get("/v1/amazon/status", [this](const Request&, Response& res) {
    const amazon::AmazonAuthStatus status = amazon::Status(config_);
    SendJson(res, {{"nile", {{"installed", status.nile.installed},
                             {"source", status.nile.source},
                             {"path", status.nile.path},
                             {"version", status.nile.version}}},
                  {"authenticated", status.authenticated}});
  });

  http_->Post("/v1/amazon/setup", [this](const Request&, Response& res) {
    auto releases = runner::ListReleases(config_, "amazon");
    if (!releases) return SendError(res, 502, releases.error().code, releases.error().message);
    if (releases->empty()) return SendError(res, 404, "no_release_found", "no matching nile release found");

    const runner::ReleaseAsset asset = releases->front();
    events_.Publish("amazon.setup.started", {{"tag", asset.tag}});
    std::thread([this, asset] {
      if (auto installed = amazon::InstallNileBinary(config_, asset); !installed) {
        log::Error("nile install failed ({}): {}", asset.tag, installed.error().message);
        events_.Publish("amazon.setup.failed", {{"tag", asset.tag}, {"error", installed.error().message}});
      } else {
        log::Info("installed nile {}", asset.tag);
        events_.Publish("amazon.setup.finished", {{"tag", asset.tag}});
      }
    }).detach();
    SendJson(res, {{"status", "downloading"}, {"tag", asset.tag}}, 202);
  });

  // Two steps: this returns the Amazon login URL; /v1/amazon/auth takes the
  // amazon.com URL the browser ends on after logging in.
  http_->Post("/v1/amazon/login", [this](const Request&, Response& res) {
    const auto url = amazon::BeginLogin(config_);
    if (!url) return SendError(res, 409, url.error().code, url.error().message);
    SendJson(res, {{"url", *url}});
  });

  http_->Post("/v1/amazon/auth", [this](const Request& req, Response& res) {
    const json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("redirect") || !body["redirect"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"redirect": "..."})");
    }
    if (auto logged_in = amazon::FinishLogin(config_, body["redirect"]); !logged_in) {
      return SendError(res, 400, logged_in.error().code, logged_in.error().message);
    }
    SendJson(res, {{"authenticated", true}});
  });

  http_->Post("/v1/amazon/logout", [this](const Request&, Response& res) {
    if (auto logged_out = amazon::Logout(config_); !logged_out) {
      return SendError(res, 400, logged_out.error().code, logged_out.error().message);
    }
    SendJson(res, {{"status", "logged_out"}});
  });

  http_->Post("/v1/amazon/import", [this](const Request&, Response& res) {
    amazon::AmazonImporter importer(config_, games_, events_);
    auto summary = importer.Import();
    if (!summary) return SendError(res, 404, summary.error().code, summary.error().message);
    SyncDesktopEntries(config_, games_);
    for (const model::Game& game : summary->added_games) metadata_fetches_.Enqueue(config_, events_, game);
    SendJson(res, {{"added", summary->added}, {"updated", summary->updated}});
  });

  // --- store launchers --------------------------------------------------
  //
  // Battle.net, Ubisoft Connect and the EA app, each installed into its own
  // prefix. Their games are imported as Mira games and launched through them.

  http_->Get("/v1/launchers", [this](const Request&, Response& res) {
    json list = json::array();
    for (const launchers::Launcher& launcher : launchers::All()) {
      const auto game = games_.Find(launchers::GameId(launcher));
      list.push_back({{"id", launcher.id},
                      {"name", launcher.name},
                      {"game_id", launchers::GameId(launcher)},
                      {"installed", launchers::Installed(games_, launcher)},
                      {"install_state", launchers::InstallState(launcher)},
                      {"interactive_install", launcher.interactive},
                      {"prefix", game ? game->data_dir : ""},
                      {"error", game ? game->last_error : ""}});
    }
    SendJson(res, list);
  });

  http_->Post(R"(/v1/launchers/([^/]+)/install)", [this](const Request& req, Response& res) {
    const launchers::Launcher* launcher = launchers::Find(req.matches[1].str());
    if (!launcher) return SendError(res, 404, "launcher_not_found", "no such launcher");
    if (!launchers::BeginInstall(*launcher)) {
      return SendError(res, 409, "install_running", std::format("{} is already installing", launcher->name));
    }
    events_.Publish("launcher.install.started", {{"id", launcher->id}});
    std::thread([this, launcher] {
      const auto done = launchers::Install(config_, games_, *launcher);
      if (const auto stored = games_.Find(launchers::GameId(*launcher))) {
        events_.Publish("game.updated", GameJson(*stored, supervisor_));
      }
      if (!done) {
        events_.Publish("launcher.install.failed", {{"id", launcher->id}, {"error", done.error().message}});
        return;
      }
      if (const auto imported = launchers::Import(config_, games_, events_, *launcher)) {
        for (const model::Game& game : imported->added_games) metadata_fetches_.Enqueue(config_, events_, game);
      }
      SyncDesktopEntries(config_, games_);
      events_.Publish("launcher.install.finished", {{"id", launcher->id}});
    }).detach();
    SendJson(res, {{"status", "installing"}, {"id", launcher->id}}, 202);
  });

  http_->Post(R"(/v1/launchers/([^/]+)/import)", [this](const Request& req, Response& res) {
    const launchers::Launcher* launcher = launchers::Find(req.matches[1].str());
    if (!launcher) return SendError(res, 404, "launcher_not_found", "no such launcher");
    const auto summary = launchers::Import(config_, games_, events_, *launcher);
    if (!summary) return SendError(res, 409, summary.error().code, summary.error().message);
    SyncDesktopEntries(config_, games_);
    for (const model::Game& game : summary->added_games) metadata_fetches_.Enqueue(config_, events_, game);
    SendJson(res, {{"added", summary->added}, {"updated", summary->updated}});
  });

  // Opens the launcher, optionally asking it to launch or install a game by
  // its store id (Battle.net product code, Ubisoft id, EA offer ids).
  http_->Post(R"(/v1/launchers/([^/]+)/open)", [this](const Request& req, Response& res) {
    const launchers::Launcher* launcher = launchers::Find(req.matches[1].str());
    if (!launcher) return SendError(res, 404, "launcher_not_found", "no such launcher");
    const json body = json::parse(req.body.empty() ? "{}" : req.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) return SendError(res, 400, "invalid_body", "expected a JSON object");
    const std::string action = body.value("action", std::string("launch"));
    if (action != "launch" && action != "install") {
      return SendError(res, 400, "invalid_action", "action must be launch or install");
    }
    model::Game target;
    target.source = "launcher";
    target.source_ref = launcher->id;
    if (const std::string ref = body.value("ref", std::string()); !ref.empty()) {
      target.source = launcher->id;
      target.source_ref = ref;
    }
    auto command = launchers::BuildCommand(config_, games_, target, action);
    if (!command) return SendError(res, 409, command.error().code, command.error().message);
    if (auto spawned = runner::SpawnDetached(*command); !spawned) {
      return SendError(res, 500, spawned.error().code, spawned.error().message);
    }
    SendJson(res, {{"status", "opened"}});
  });

  // --- itch -----------------------------------------------------------
  //
  // Wraps butlerd (itch.io's own launcher-integration daemon) for auth,
  // catalog, and install/update. Mira never runs the itch app — an
  // installed title launches through Mira's own Wine/Proton runners, or
  // natively for the many itch.io titles that ship a Linux build.

  http_->Get("/v1/itch/status", [this](const Request&, Response& res) {
    const itch::ItchAuthStatus status = itch::Status(config_);
    SendJson(res, {{"butler", {{"installed", status.butler.installed},
                               {"source", status.butler.source},
                               {"path", status.butler.path},
                               {"version", status.butler.version}}},
                  {"authenticated", status.authenticated},
                  {"login_url", itch::kApiKeysUrl}});
  });

  http_->Post("/v1/itch/setup", [this](const Request&, Response& res) {
    auto releases = runner::ListReleases(config_, "itch");
    if (!releases) return SendError(res, 502, releases.error().code, releases.error().message);
    if (releases->empty()) return SendError(res, 404, "no_release_found", "no matching butler release found");

    const runner::ReleaseAsset asset = releases->front();
    events_.Publish("itch.setup.started", {{"tag", asset.tag}});
    std::thread([this, asset] {
      if (auto installed = itch::InstallButlerBinary(config_, asset); !installed) {
        log::Error("butler install failed ({}): {}", asset.tag, installed.error().message);
        events_.Publish("itch.setup.failed", {{"tag", asset.tag}, {"error", installed.error().message}});
      } else {
        log::Info("installed butler {}", asset.tag);
        events_.Publish("itch.setup.finished", {{"tag", asset.tag}});
      }
    }).detach();

    SendJson(res, {{"status", "downloading"}, {"tag", asset.tag}}, 202);
  });

  // The user's itch.io API key (itch.io/user/settings/api-keys) — unlike
  // Epic/GOG this isn't a pasted redirect code, so there's no login URL to
  // print (see CmdItchLogin in the CLI).
  http_->Post("/v1/itch/auth", [this](const Request& req, Response& res) {
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("api_key") || !body["api_key"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"api_key": "..."})");
    }
    if (auto logged_in = itch::Login(config_, body["api_key"]); !logged_in) {
      return SendError(res, 400, logged_in.error().code, logged_in.error().message);
    }
    SendJson(res, {{"authenticated", true}});
  });

  http_->Post("/v1/itch/logout", [this](const Request&, Response& res) {
    if (auto logged_out = itch::Logout(config_); !logged_out) {
      return SendError(res, 400, logged_out.error().code, logged_out.error().message);
    }
    SendJson(res, {{"status", "logged_out"}});
  });

  http_->Post("/v1/itch/import", [this](const Request&, Response& res) {
    itch::ItchImporter importer(config_, games_, events_);
    auto summary = importer.Import();
    if (!summary) return SendError(res, 404, summary.error().code, summary.error().message);
    SyncDesktopEntries(config_, games_);
    for (const model::Game& game : summary->added_games) metadata_fetches_.Enqueue(config_, events_, game);
    SendJson(res, {{"added", summary->added}, {"updated", summary->updated}});
  });

  // --- humble -------------------------------------------------------------
  //
  // Wraps humble-cli (unofficial). Deliberately outside the
  // library::ILibrarySource registry — Humble Bundle has no
  // install/update/uninstall state of its own to report on, just
  // purchased bundles of downloadable files (see src/humble/Humble.h's
  // class comment). A downloaded item is never auto-imported as a
  // model::Game.

  http_->Get("/v1/humble/status", [this](const Request&, Response& res) {
    const humble::HumbleAuthStatus status = humble::Status(config_);
    SendJson(res, {{"humble_cli", {{"installed", status.humble_cli.installed},
                                   {"source", status.humble_cli.source},
                                   {"path", status.humble_cli.path},
                                   {"version", status.humble_cli.version}}},
                  {"authenticated", status.authenticated},
                  {"login_url", humble::kLoginUrl}});
  });

  http_->Post("/v1/humble/setup", [this](const Request&, Response& res) {
    auto releases = runner::ListReleases(config_, "humble");
    if (!releases) return SendError(res, 502, releases.error().code, releases.error().message);
    if (releases->empty()) return SendError(res, 404, "no_release_found", "no matching humble-cli release found");

    const runner::ReleaseAsset asset = releases->front();
    events_.Publish("humble.setup.started", {{"tag", asset.tag}});
    std::thread([this, asset] {
      if (auto installed = humble::InstallHumbleCliBinary(config_, asset); !installed) {
        log::Error("humble-cli install failed ({}): {}", asset.tag, installed.error().message);
        events_.Publish("humble.setup.failed", {{"tag", asset.tag}, {"error", installed.error().message}});
      } else {
        log::Info("installed humble-cli {}", asset.tag);
        events_.Publish("humble.setup.finished", {{"tag", asset.tag}});
      }
    }).detach();

    SendJson(res, {{"status", "downloading"}, {"tag", asset.tag}}, 202);
  });

  // The _simpleauth_sess cookie value from a logged-in browser session —
  // no login URL/code flow exists for Humble Bundle the way Epic/GOG have.
  http_->Post("/v1/humble/auth", [this](const Request& req, Response& res) {
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("session_key") || !body["session_key"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"session_key": "..."})");
    }
    if (auto logged_in = humble::Login(config_, body["session_key"]); !logged_in) {
      return SendError(res, 400, logged_in.error().code, logged_in.error().message);
    }
    SendJson(res, {{"authenticated", true}});
  });

  http_->Get("/v1/humble/library", [this](const Request&, Response& res) {
    auto bundles = humble::ListBundles(config_);
    if (!bundles) return SendError(res, 400, bundles.error().code, bundles.error().message);
    json out = json::array();
    for (const humble::BundleSummary& bundle : *bundles) {
      out.push_back({{"key", bundle.key}, {"name", bundle.name}, {"claimed", bundle.claimed}});
    }
    SendJson(res, std::move(out));
  });

  // Detached, same "don't block the request thread on a slow download"
  // pattern as everything else here — humble.download.started/finished/
  // failed on the event stream is how a caller finds out it's done.
  http_->Post("/v1/humble/download", [this](const Request& req, Response& res) {
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("bundle_key") || !body["bundle_key"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"bundle_key": "...", "item_numbers": "..."})");
    }
    const std::string bundle_key = body["bundle_key"];
    const std::string item_numbers = body.value("item_numbers", std::string());

    events_.Publish("humble.download.started", {{"bundle_key", bundle_key}});
    std::thread([this, bundle_key, item_numbers] {
      const Result<bool> result = humble::Download(config_, bundle_key, item_numbers);
      if (!result) {
        log::Error("humble download failed ({}): {}", bundle_key, result.error().message);
        events_.Publish("humble.download.failed", {{"bundle_key", bundle_key}, {"error", result.error().message}});
      } else if (!*result) {
        log::Warn("humble download for {} had nothing to download (a redeemed key with no Humble-hosted "
                 "files, most likely)",
                 bundle_key);
        events_.Publish("humble.download.finished",
                       {{"bundle_key", bundle_key},
                        {"path", humble::DownloadDir(config_, bundle_key).string()},
                        {"downloaded", false}});
      } else {
        log::Info("humble download finished: {}", bundle_key);
        events_.Publish("humble.download.finished",
                       {{"bundle_key", bundle_key},
                        {"path", humble::DownloadDir(config_, bundle_key).string()},
                        {"downloaded", true}});
      }
    }).detach();

    SendJson(res, {{"status", "downloading"}, {"bundle_key", bundle_key},
                  {"path", humble::DownloadDir(config_, bundle_key).string()}},
            202);
  });

  // --- library (what the account owns, across sources) ------------------
  //
  // Distinct from GET /v1/games, which is what Mira *tracks*. An
  // entitlement isn't a tracked game (see library/Catalog.h): these are
  // read through live from each source rather than persisted, and a title
  // becomes a model::Game only once it's installed.

  http_->Get("/v1/library", [this](const Request& req, Response& res) {
    const std::string source = req.has_param("source") ? req.get_param_value("source") : "";
    auto entries = library::ListCatalog(config_, games_, source);
    if (!entries) return SendError(res, 400, entries.error().code, entries.error().message);
    json out = json::array();
    for (const library::CatalogEntry& entry : *entries) {
      out.push_back({{"source", entry.source},
                     {"ref", entry.ref},
                     {"title", entry.title},
                     {"installed", entry.installed},
                     {"game_id", entry.game_id},
                     {"play_seconds", entry.play_seconds}});
    }
    SendJson(res, std::move(out));
  });

  // Installs/updates run detached, same reasoning as POST
  // /v1/runners/download above — a game download is easily minutes long.
  // library.install.started/finished/failed on the event stream is how a
  // caller finds out it's done; "update": true on the payload is the only
  // difference between the two verbs, rather than a parallel event
  // namespace. Keyed by {source, ref} rather than a game id: the whole
  // point is installing something Mira doesn't track yet.
  auto library_install_or_update = [this](const Request& req, Response& res, bool is_update) {
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("source") || !body["source"].is_string() ||
        !body.contains("ref") || !body["ref"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"source": "epic"|"steam"|"gog"|"itch"|"amazon", "ref": "..."})");
    }
    const std::string source = body["source"];
    const std::string ref = body["ref"];

    library::ILibrarySource* src = library::FindSource(source);
    if (src == nullptr) {
      return SendError(res, 400, "unknown_source", std::format("no installable source named \"{}\"", source));
    }

    events_.Publish("library.install.started", {{"source", source}, {"ref", ref}, {"update", is_update}});
    std::thread([this, src, source, ref, is_update] {
      const Result<void> result = is_update ? src->Update(config_, games_, events_, ref)
                                            : src->Install(config_, games_, events_, ref);
      if (!result) {
        log::Error("{} {} failed ({}): {}", source, is_update ? "update" : "install", ref, result.error().message);
        events_.Publish("library.install.failed",
                       {{"source", source}, {"ref", ref}, {"update", is_update}, {"error", result.error().message}});
      } else {
        log::Info("{} {} finished: {}", source, is_update ? "update" : "install", ref);
        SyncDesktopEntries(config_, games_);
        if (const auto game = games_.Find(source + "-" + ref)) metadata_fetches_.Enqueue(config_, events_, *game);
        events_.Publish("library.install.finished", {{"source", source}, {"ref", ref}, {"update", is_update}});
      }
    }).detach();

    SendJson(res, {{"status", is_update ? "updating" : "installing"}, {"ref", ref}}, 202);
  };
  http_->Post("/v1/library/install", [library_install_or_update](const Request& req, Response& res) {
    library_install_or_update(req, res, false);
  });
  http_->Post("/v1/library/update", [library_install_or_update](const Request& req, Response& res) {
    library_install_or_update(req, res, true);
  });

  // --- desktop entries (importing someone else's, not writing ours) -----

  http_->Get("/v1/desktop-entries/candidates", [this](const Request&, Response& res) {
    desktop::DesktopEntryScanner scanner(config_, games_, events_);
    auto candidates = scanner.ListCandidates();
    if (!candidates) return SendError(res, 404, candidates.error().code, candidates.error().message);
    json out = json::array();
    for (const auto& c : *candidates) out.push_back({{"id", c.id}, {"name", c.name}, {"icon", c.icon}});
    SendJson(res, std::move(out));
  });

  http_->Post("/v1/desktop-entries/import", [this](const Request& req, Response& res) {
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("ids") || !body["ids"].is_array()) {
      return SendError(res, 400, "invalid_body", R"(expected {"ids": ["..."]})");
    }
    const std::vector<std::string> ids = body["ids"];

    desktop::DesktopEntryScanner scanner(config_, games_, events_);
    auto summary = scanner.Import(ids);
    if (!summary) return SendError(res, 404, summary.error().code, summary.error().message);
    SyncDesktopEntries(config_, games_);
    for (const model::Game& game : summary->added_games) metadata_fetches_.Enqueue(config_, events_, game);
    SendJson(res, {{"added", summary->added}, {"updated", summary->updated}});
  });

  http_->Post("/v1/desktop-entries/sync", [this](const Request&, Response& res) {
    SyncDesktopEntries(config_, games_);
    SendJson(res, {{"ok", true}});
  });

  // --- manual add -----------------------------------------------------------

  // The primitive every other "add a game" path (a library scan,
  // SteamScanner, LutrisImporter, DesktopEntryScanner) only ever exercises
  // as a side effect of discovering something Mira already knew to look
  // for. This is the one place a game record can be created directly, for
  // a path outside every configured library root -- e.g. pointing Mira at
  // an installer or a folder it wouldn't otherwise scan.
  http_->Post("/v1/games/manual", [this](const Request& req, Response& res) {
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("install_path") || !body["install_path"].is_string() ||
        !body.contains("exe_path") || !body["exe_path"].is_string()) {
      return SendError(res, 400, "invalid_body",
                       R"(expected {"install_path": "...", "exe_path": "...", "name"?, "platform"?, "is_installer"?})");
    }
    const std::filesystem::path install_path = body["install_path"].get<std::string>();
    const std::string exe_path = body["exe_path"];
    const bool is_installer = body.value("is_installer", false);

    model::Platform platform;
    if (body.contains("platform") && body["platform"].is_string()) {
      platform = model::PlatformFromString(body["platform"].get<std::string>());
    } else {
      const std::string ext = strings::ToLower(std::filesystem::path(exe_path).extension().string());
      platform = ext == ".exe" ? model::Platform::Windows : model::Platform::Native;
    }

    model::Game game;
    const auto existing = games_.FindByInstallPath(install_path.string());
    if (existing) game = *existing;
    game.name = body.value("name", strings::CleanGameName(install_path.filename().string()));
    game.id = existing ? game.id : games_.NextId(game.name);
    game.source = "manual";
    game.install_path = install_path.string();
    game.exe_path = exe_path;
    game.args = body.value("args", std::string());
    game.platform = platform;
    game.updated_at = model::NowSeconds();
    if (!existing) game.created_at = game.updated_at;

    if (is_installer) {
      // Matches AutoSetup's own installer flagging -- running it isn't
      // running the game, and no prefix is provisioned yet for something
      // that can't be launched.
      game.status = model::GameStatus::NeedsInstall;
      game.last_error = "This is an installer, not the game itself — run it first, then point Mira at the "
                        "installed game.";
    } else {
      if (platform != model::Platform::Windows) {
        game.data_dir.clear();
      } else if (game.data_dir.empty()) {
        game.data_dir = library::PrefixDir(config_, game).string();
      }
      game.status = model::GameStatus::Ready;
      game.last_error.clear();
    }

    auto result = games_.Upsert(game);
    if (!result) return SendError(res, 500, result.error().code, result.error().message);

    if (game.status == model::GameStatus::Ready && game.platform == model::Platform::Windows) {
      const runner::RunnerRegistry provisioner(config_);
      const model::Game provisioned = provisioner.ProvisionGame(game);
      auto saved = games_.Update(game.id, [&](model::Game& g) {
        g.runner_ref = provisioned.runner_ref;
        g.status = provisioned.status;
        g.last_error = provisioned.last_error;
      });
      if (saved) game = *saved;
    }

    SyncDesktopEntries(config_, games_);
    if (!existing) metadata_fetches_.Enqueue(config_, events_, game);
    events_.Publish(existing ? "game.updated" : "game.added", GameJson(game, supervisor_));
    SendJson(res, GameJson(game, supervisor_));
  });

  // --- launching ------------------------------------------------------------

  http_->Post(R"(/v1/games/([^/]+)/launch)", [this](const Request& req, Response& res) {
    auto game = games_.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    if (game->status == model::GameStatus::NeedsInstall) {
      return SendError(res, 409, "needs_install", game->last_error);
    }
    if (game->status != model::GameStatus::Ready) {
      return SendError(res, 409, "not_ready",
                       std::format("\"{}\" is {}, not ready to launch", game->id,
                                  model::ToString(game->status)));
    }

    const config::Resolver resolver(config_, game->overrides);
    const std::string pre_script = resolver.GetString("launch.pre_script");
    const std::string post_script = resolver.GetString("launch.post_script");

    // A store launcher game (Battle.net, Ubisoft, EA) is started by its
    // launcher, which keeps running after the game exits; the game's own
    // processes are what's tracked.
    if (launchers::ForGame(*game)) {
      if (supervisor_.IsRunning(game->id)) {
        return SendError(res, 409, "already_running", std::format("\"{}\" is already running", game->id));
      }
      auto command = launchers::BuildCommand(config_, games_, *game);
      if (!command) return SendError(res, 409, command.error().code, command.error().message);
      if (auto ran = RunPreScriptInline(pre_script); !ran) {
        return SendError(res, 409, ran.error().code, ran.error().message);
      }
      ApplyLaunchEnv(*command, resolver.GetStringArray("launch.env"));
      if (auto spawned = runner::SpawnDetached(*command); !spawned) {
        return SendError(res, 500, spawned.error().code, spawned.error().message);
      }
      [[maybe_unused]] auto _ =
          games_.Update(game->id, [](model::Game& g) { g.last_played_at = model::NowSeconds(); });
      events_.Publish("game.launched", {{"id", game->id}, {"via", "launcher"}, {"tracked", true}});
      if (auto started = supervisor_.TrackLauncherLaunch(*game, launchers::WindowsDir(*game),
                                                         config_.GetInt("launchers.detect_timeout_s"), post_script);
          !started) {
        log::Warn("couldn't start tracking {}: {}", game->id, started.error().message);
      }
      return SendJson(res, {{"status", "launched_via_launcher"}, {"tracked", true}});
    }

    // A Steam-sourced game defaults to asking the Steam client to launch it
    // (steam://rungameid/<appid>) rather than Mira execing it directly: full
    // achievements/overlay support, and Steam's own accounting is what
    // GET /v1/games/{id} reads back for it (see steam.launch_mode's doc).
    // Mira didn't spawn this process, so it can't waitpid() it — that's what
    // steam.track_process (a /proc scan, see ProcessSupervisor) is for. Not
    // wrapped by mira-run either way: there's no process here for it to own.
    if (game->runner_ref.starts_with("steam:")) {
      if (resolver.GetString("steam.launch_mode") == "steam") {
        if (auto ran = RunPreScriptInline(pre_script); !ran) {
          return SendError(res, 409, ran.error().code, ran.error().message);
        }
        const std::string appid = game->runner_ref.substr(std::string_view("steam:").size());
        Command command;
        command.argv = {"steam", std::format("steam://rungameid/{}", appid)};
        if (auto spawned = runner::SpawnDetached(command); !spawned) {
          return SendError(res, 500, spawned.error().code, spawned.error().message);
        }
        [[maybe_unused]] auto _ =
            games_.Update(game->id, [](model::Game& g) { g.last_played_at = model::NowSeconds(); });
        // Whether game.state events are coming for this launch — on the
        // event too, for a client that launched from elsewhere (the CLI).
        const bool track = resolver.GetBool("steam.track_process");
        events_.Publish("game.launched",
                        {{"id", game->id}, {"via", "steam"}, {"tracked", track}});
        if (track) {
          if (auto started = supervisor_.TrackSteamLaunch(*game, appid, post_script); !started) {
            log::Warn("couldn't start tracking {}: {}", game->id, started.error().message);
          }
        }
        return SendJson(res, {{"status", "launched_via_steam"}, {"tracked", track}});
      }
    }

    const runner::RunnerRegistry registry(config_);
    auto resolved = registry.Resolve(registry.ResolveRef(*game));
    if (!resolved) return SendError(res, 400, resolved.error().code, resolved.error().message);

    if (game->runner_ref.empty() && resolved->build && config_.GetBool("launch.pin_runner")) {
      const std::string pinned = std::format("{}:{}", resolved->runner->kind(), resolved->build->name);
      game->runner_ref = pinned;
      [[maybe_unused]] auto _ = games_.Update(game->id, [&](model::Game& g) { g.runner_ref = pinned; });
    }

    auto command = resolved->runner->BuildCommand(*game, resolved->build);
    if (!command) return SendError(res, 400, command.error().code, command.error().message);

    const std::vector<std::string> wrappers = resolver.GetStringArray("command_wrappers");
    if (auto checked = CheckCommandWrappers(wrappers); !checked) {
      return SendError(res, 400, checked.error().code, checked.error().message);
    }
    ApplyLaunchEnv(*command, resolver.GetStringArray("launch.env"));
    ApplyCommandWrappers(*command, wrappers);

    // mira-run owns the whole session end to end (pre/post script, the
    // session record) so it survives mirad dying mid-launch — see
    // proc/Session.h, docs/architecture.md. The wrapper is never a hard
    // dependency: if it can't be found or spawned, mirad falls back to
    // exactly today's behavior (pre_script run inline, no session record)
    // rather than failing the launch.
    const auto mira_run = runner::ResolveSiblingBinary(OwnBinaryDir(), "mira-run");
    if (!mira_run) {
      log::Warn("mira-run not found; launching {} directly with no session recording", game->id);
      if (auto ran = RunPreScriptInline(pre_script); !ran) {
        return SendError(res, 409, ran.error().code, ran.error().message);
      }
      if (auto launched = supervisor_.Launch(*game, *command, post_script); !launched) {
        return SendError(res, 409, launched.error().code, launched.error().message);
      }
      return SendJson(res, {{"status", "running"}, {"tracked", true}});
    }

    const int pre_timeout_s = static_cast<int>(resolver.GetInt("launch.pre_timeout_s"));
    const std::filesystem::path sessions_dir = games_.Dir() / "sessions";
    const std::filesystem::path log_file = games_.Dir() / "logs" / std::format("{}.log", game->id);
    Command wrapped;
    wrapped.env = command->env;
    wrapped.cwd = command->cwd;
    wrapped.argv = {*mira_run,         "--game-id",       game->id,
                    "--session-dir",  sessions_dir.string(), "--log-file", log_file.string(),
                    "--log-max-mb",   std::to_string(resolver.GetInt("launch.log_max_mb")),
                    "--status-fd",    "3",
                    "--pre-timeout",  std::to_string(pre_timeout_s),
                    "--post-timeout", std::to_string(resolver.GetInt("launch.post_timeout_s"))};
    if (!pre_script.empty()) {
      wrapped.argv.push_back("--pre");
      wrapped.argv.push_back(pre_script);
    }
    if (!post_script.empty()) {
      wrapped.argv.push_back("--post");
      wrapped.argv.push_back(post_script);
    }
    if (resolver.GetBool("launch.gamemode")) wrapped.argv.push_back("--gamemode");
    wrapped.argv.push_back("--");
    wrapped.argv.insert(wrapped.argv.end(), command->argv.begin(), command->argv.end());

    int status_fd = -1;
    auto wrapper_pid = runner::SpawnDetachedWithStatus(wrapped, status_fd);
    if (!wrapper_pid) return SendError(res, 500, wrapper_pid.error().code, wrapper_pid.error().message);

    const WrapperStatus status = ReadWrapperStatus(status_fd, pre_timeout_s + 10);
    ::close(status_fd);
    // Always WNOHANG, never a blocking wait: on every other outcome
    // mira-run has already exited by now, so this reaps it immediately with
    // no delay -- but on read_timed_out it may still be genuinely hung, and
    // this HTTP worker thread must never block on that indefinitely. A
    // WNOHANG that doesn't reap here just leaves it for LaunchWrapped's own
    // watcher (success) or an unreaped zombie mirad doesn't yet clean up
    // (the timeout case -- rare enough not to chase further right now).
    int wait_status = 0;
    ::waitpid(*wrapper_pid, &wait_status, WNOHANG);

    if (status.read_timed_out) {
      return SendError(res, 500, "wrapper_unresponsive", "mira-run did not respond in time");
    }
    if (status.code == "pre_failed") {
      return SendError(res, 409, "pre_launch_failed", status.detail);
    }
    if (status.code == "pre_timeout") {
      return SendError(res, 409, "pre_launch_timeout",
                       std::format("launch.pre_script did not finish within {}s", pre_timeout_s));
    }
    if (!status.ok) {
      return SendError(res, 500, "wrapper_failed", std::format("unexpected mira-run status: {}", status.code));
    }

    if (auto launched = supervisor_.LaunchWrapped(*game, *wrapper_pid, std::filesystem::path(status.detail));
        !launched) {
      return SendError(res, 409, launched.error().code, launched.error().message);
    }
    SendJson(res, {{"status", "running"}, {"tracked", true}});  // always true: Mira spawned it
  });

  http_->Post(R"(/v1/games/([^/]+)/stop)", [this](const Request& req, Response& res) {
    if (auto stopped = supervisor_.Stop(req.matches[1]); !stopped) {
      // A client that still shows it running (it missed the exit, e.g. across
      // a mirad restart) gets told it's stopped instead of an error it can't
      // get out of.
      if (stopped.error().code == "not_running") {
        if (const auto game = games_.Find(req.matches[1])) {
          json event = GameJson(*game, supervisor_);
          event["state"] = "idle";
          events_.Publish("game.state", std::move(event));
          return SendJson(res, {{"status", "not_running"}});
        }
      }
      return SendError(res, 409, stopped.error().code, stopped.error().message);
    }
    SendJson(res, {{"status", "stopping"}});
  });

  // Runs an arbitrary exe inside this game's own prefix — normal
  // ProcessSupervisor tracking, same as /launch, but the exe/args come from
  // the request instead of the stored game. This is what actually runs a
  // needs_install game's installer (provisioning on demand, since Scanner
  // never provisions one), and it's the general "run something in this
  // prefix" escape hatch (winetricks-equivalent work, one-off tools) short
  // of a full custom-tricks implementation, which stays out of scope.
  http_->Post(R"(/v1/games/([^/]+)/run)", [this](const Request& req, Response& res) {
    auto game = games_.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");

    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("exe_path") || !body["exe_path"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"exe_path": "...", "args": "..."})");
    }
    const std::string exe_path = body["exe_path"];
    const std::string args = body.value("args", std::string());

    // A needs_install/setting_up game (or one whose prefix vanished) has no
    // usable prefix yet — provision one now rather than requiring a
    // separate call first.
    std::error_code ec;
    const bool needs_provisioning = game->platform == model::Platform::Windows &&
        (game->runner_ref.empty() || !std::filesystem::exists(std::filesystem::path(game->data_dir) / "drive_c", ec));
    if (needs_provisioning) {
      const runner::RunnerRegistry provisioner(config_);
      const model::Game provisioned = provisioner.ProvisionGame(*game);
      auto saved = games_.Update(game->id, [&](model::Game& g) {
        g.runner_ref = provisioned.runner_ref;
        if (provisioned.status == model::GameStatus::Broken) {
          g.status = model::GameStatus::Broken;
          g.last_error = provisioned.last_error;
        } else {
          g.last_error.clear();
        }
      });
      if (!saved) return SendError(res, 404, saved.error().code, saved.error().message);
      game = *saved;
      if (game->status == model::GameStatus::Broken) {
        return SendError(res, 409, "provision_failed", game->last_error);
      }
    }

    const runner::RunnerRegistry registry(config_);
    auto resolved = registry.Resolve(registry.ResolveRef(*game));
    if (!resolved) return SendError(res, 400, resolved.error().code, resolved.error().message);

    if (game->runner_ref.empty() && resolved->build && config_.GetBool("launch.pin_runner")) {
      const std::string pinned = std::format("{}:{}", resolved->runner->kind(), resolved->build->name);
      game->runner_ref = pinned;
      [[maybe_unused]] auto _ = games_.Update(game->id, [&](model::Game& g) { g.runner_ref = pinned; });
    }

    model::Game run_as = *game;
    run_as.exe_path = exe_path;
    run_as.args = args;

    auto command = resolved->runner->BuildCommand(run_as, resolved->build);
    if (!command) return SendError(res, 400, command.error().code, command.error().message);
    const config::Resolver resolver(config_, game->overrides);
    const std::vector<std::string> wrappers = resolver.GetStringArray("command_wrappers");
    if (auto checked = CheckCommandWrappers(wrappers); !checked) {
      return SendError(res, 400, checked.error().code, checked.error().message);
    }
    ApplyLaunchEnv(*command, resolver.GetStringArray("launch.env"));
    ApplyCommandWrappers(*command, wrappers);

    if (auto launched = supervisor_.Launch(*game, *command); !launched) {
      return SendError(res, 409, launched.error().code, launched.error().message);
    }
    SendJson(res, {{"status", "running"}});
  });

  // The escape hatch out of needs_install: run the installer via /run
  // above, PATCH exe_path to whatever it actually installed, then call
  // this to make the game launchable through the normal /launch path.
  // Describes a game's installer, or the file at ?path= (absolute, or
  // relative to install_path) when choosing one by hand.
  http_->Get(R"(/v1/games/([^/]+)/installer)", [this](const Request& req, Response& res) {
    auto game = games_.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    if (req.has_param("path")) game->exe_path = req.get_param_value("path");
    const auto info = library::DescribeInstaller(config_, *game);
    if (!info) return SendError(res, 404, info.error().code, info.error().message);
    SendJson(res, {{"path", info->path.string()},
                   {"size_bytes", info->size_bytes},
                   {"format", library::ToString(info->format)},
                   {"silent", info->silent},
                   {"silent_args", info->silent_args}});
  });

  http_->Get(R"(/v1/games/([^/]+)/install/progress)", [this](const Request& req, Response& res) {
    const std::string id = req.matches[1];
    if (!games_.Find(id)) return SendError(res, 404, "game_not_found", "no such game");
    const auto progress = library::Progress(id);
    if (!progress) return SendJson(res, {{"state", "idle"}});
    SendJson(res, {{"state", progress->state},
                   {"mode", progress->mode},
                   {"started_at", progress->started_at},
                   {"finished_at", progress->finished_at},
                   {"error", progress->error},
                   {"bytes_written", progress->bytes_written}});
  });

  // Runs a game's installer: silent for Inno/NSIS, otherwise (or with
  // "interactive": true) shown to click through. "installer" picks the
  // file by hand. Detached; poll .../install/progress or watch
  // game.install.finished/failed.
  http_->Post(R"(/v1/games/([^/]+)/install)", [this](const Request& req, Response& res) {
    const auto game = games_.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    const json body = json::parse(req.body.empty() ? "{}" : req.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) return SendError(res, 400, "invalid_body", "expected a JSON object");
    const bool interactive = body.value("interactive", false);
    std::optional<std::filesystem::path> installer;
    if (body.contains("installer")) {
      if (!body["installer"].is_string()) return SendError(res, 400, "invalid_body", "installer must be a string");
      installer = std::filesystem::path(game->install_path) / body["installer"].get<std::string>();
      std::error_code ec;
      if (!std::filesystem::is_regular_file(*installer, ec)) {
        return SendError(res, 404, "installer_missing", std::format("no installer at {}", installer->string()));
      }
    }
    const bool installable = game->status == model::GameStatus::NeedsInstall ||
                             (installer && game->status == model::GameStatus::Broken);
    if (!installable) return SendError(res, 409, "not_needs_install", "game isn't waiting on an installer");
    if (!library::BeginInstall(game->id)) {
      return SendError(res, 409, "install_running", "an install is already running for this game");
    }

    events_.Publish("game.install.started", {{"id", game->id}});
    std::thread([this, id = game->id, interactive, installer] {
      const auto done = library::Install(config_, games_, id,
                                         interactive ? library::InstallMode::kInteractive : library::InstallMode::kAuto,
                                         installer);
      if (done) {
        SyncDesktopEntries(config_, games_);
        events_.Publish("game.updated", GameJson(*done, supervisor_));
        events_.Publish("game.install.finished", {{"id", id}});
      } else {
        if (const auto stored = games_.Find(id)) events_.Publish("game.updated", GameJson(*stored, supervisor_));
        events_.Publish("game.install.failed", {{"id", id}, {"error", done.error().message}});
      }
    }).detach();
    SendJson(res, {{"status", "installing"}, {"id", game->id}}, 202);
  });

  http_->Post(R"(/v1/games/([^/]+)/finish-install)", [this](const Request& req, Response& res) {
    auto game = games_.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    if (game->exe_path.empty()) {
      return SendError(res, 409, "no_executable",
                       "PATCH exe_path to the installed game's real executable first");
    }
    const bool still_installer = std::ranges::any_of(game->candidates, [&](const model::Candidate& c) {
      return c.is_installer && c.rel_path == game->exe_path;
    });
    if (still_installer) {
      return SendError(res, 409, "no_executable",
                       "exe_path still points at the installer — PATCH it to the installed game's real "
                       "executable first");
    }
    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::path(game->install_path) / game->exe_path, ec)) {
      return SendError(res, 409, "no_executable", "exe_path doesn't exist under install_path");
    }
    auto result = games_.Update(game->id, [](model::Game& g) {
      g.status = model::GameStatus::Ready;
      g.last_error.clear();
    });
    if (!result) return SendError(res, 404, result.error().code, result.error().message);
    SyncDesktopEntries(config_, games_);
    events_.Publish("game.updated", GameJson(*result, supervisor_));
    SendJson(res, GameJson(*result, supervisor_));
  });

  // Moves this game's install_path/data_dir -- to the given target(s), or,
  // with no body (or a body omitting a field), into Mira's own canonical
  // layout for whichever field is omitted (see library::Relocate). The
  // escape hatch for a Lutris import or any game whose files sit outside
  // where prefix_root/prefix_naming now say they should — neither setting
  // moves anything by itself; this is the only thing that does.
  http_->Post(R"(/v1/games/([^/]+)/relocate)", [this](const Request& req, Response& res) {
    auto game = games_.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");

    library::RelocateRequest request;
    if (!req.body.empty()) {
      const json body = json::parse(req.body, nullptr, false);
      if (body.is_discarded() || !body.is_object()) {
        return SendError(res, 400, "invalid_body", R"(expected {"install_path"?: "...", "data_dir"?: "..."})");
      }
      if (body.contains("install_path") && body["install_path"].is_string()) {
        request.install_path = std::filesystem::path(body["install_path"].get<std::string>());
      }
      if (body.contains("data_dir") && body["data_dir"].is_string()) {
        request.data_dir = std::filesystem::path(body["data_dir"].get<std::string>());
      }
    }

    auto relocated = library::Relocate(config_, *game, request);
    if (!relocated) return SendError(res, 400, relocated.error().code, relocated.error().message);

    auto saved = games_.Update(game->id, [&](model::Game& g) {
      g.install_path = relocated->install_path;
      g.data_dir = relocated->data_dir;
      g.updated_at = model::NowSeconds();
    });
    if (!saved) return SendError(res, 404, saved.error().code, saved.error().message);
    SyncDesktopEntries(config_, games_);
    events_.Publish("game.updated", GameJson(*saved, supervisor_));
    SendJson(res, GameJson(*saved, supervisor_));
  });

  // Runs one winetricks verb against this game's own prefix — see
  // runner/Winetricks.h for why this shells out to the real tool rather than
  // reimplementing it. Runs in the background (a verb can mean downloading
  // and installing a redistributable, real minutes, not a request-scale
  // wait) and returns 202 immediately; tricks.finished/.failed on the event
  // stream say when it's done.
  http_->Post(R"(/v1/games/([^/]+)/tricks)", [this](const Request& req, Response& res) {
    auto game = games_.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");

    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("verb") || !body["verb"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"verb": "..."})");
    }
    const std::string verb = body["verb"];
    const std::string id = game->id;

    events_.Publish("tricks.started", {{"id", id}, {"verb", verb}});
    tricks_queue_.Run([this, id, verb] {
      const runner::RunnerRegistry registry(config_);
      const auto game = games_.Find(id);
      if (!game) return;  // removed while queued
      if (auto ran = runner::RunTricksVerb(registry, *game, verb); !ran) {
        log::Error("winetricks {} failed for {}: {}", verb, id, ran.error().message);
        events_.Publish("tricks.failed", {{"id", id}, {"verb", verb}, {"error", ran.error().message}});
      } else {
        events_.Publish("tricks.finished", {{"id", id}, {"verb", verb}});
      }
    });
    SendJson(res, {{"status", "running"}, {"verb", verb}}, 202);
  });

  // --- metadata ---------------------------------------------------------

  // Cached cover art + store info (see metadata/MetadataFetcher.h) — fetched
  // automatically off a scan, never blocking one; these two endpoints only
  // ever read what's already on disk. 404 either means "never fetched" or
  // "fetched, but this source had nothing" — the caller can always retry via
  // the refresh endpoint below to find out which.
  http_->Get(R"(/v1/games/([^/]+)/metadata)", [this](const Request& req, Response& res) {
    if (!games_.Find(req.matches[1])) return SendError(res, 404, "game_not_found", "no such game");
    const std::filesystem::path file = metadata::MetadataFile(config_, req.matches[1]);
    std::ifstream in(file);
    if (!in) return SendError(res, 404, "metadata_not_found", "no metadata cached for this game yet");
    std::ostringstream buffer;
    buffer << in.rdbuf();
    res.set_content(buffer.str(), "application/json");
  });

  http_->Get(R"(/v1/games/([^/]+)/artwork)", [this](const Request& req, Response& res) {
    if (!games_.Find(req.matches[1])) return SendError(res, 404, "game_not_found", "no such game");
    // ?type= picks a non-default art slot; "cover" (stored as "artwork" for
    // wire compatibility) is the default. See MetadataFetcher for the full
    // set of slots each source writes.
    const std::string type = req.has_param("type") ? req.get_param_value("type") : "cover";
    const std::string key = type == "cover" ? "artwork" : type;
    const std::filesystem::path metadata_file = metadata::MetadataFile(config_, req.matches[1]);
    std::ifstream meta_in(metadata_file);
    if (!meta_in) return SendError(res, 404, "artwork_not_found", "no artwork cached for this game yet");
    const json info = json::parse(meta_in, nullptr, false);
    if (info.is_discarded() || !info.contains(key)) {
      return SendError(res, 404, "artwork_not_found", "no artwork cached for this game yet");
    }
    const std::filesystem::path file =
        metadata::ArtworkDir(config_, req.matches[1]) / info[key].value("file", std::string());
    std::ifstream in(file, std::ios::binary);
    if (!in) return SendError(res, 404, "artwork_not_found", "cached artwork file is missing");
    std::ostringstream buffer;
    buffer << in.rdbuf();
    res.set_content(buffer.str(), info[key].value("content_type", "image/jpeg"));
  });

  // Lets a caller pick a different cached SteamGridDB candidate for a slot
  // (see art_candidates in GET .../metadata) instead of the auto-picked
  // top result -- looked up by the id from that list, never a raw URL, so
  // this can't be used to make the daemon fetch an arbitrary address.
  // Runs on artwork_selects_ rather than the request thread, same reasoning
  // as metadata_fetches_ below: a curl round trip must never block an API
  // thread.
  http_->Post(R"(/v1/games/([^/]+)/artwork)", [this](const Request& req, Response& res) {
    const std::string id = req.matches[1];
    if (!games_.Find(id)) return SendError(res, 404, "game_not_found", "no such game");
    if (!req.has_param("type")) return SendError(res, 400, "missing_type", "?type= is required");
    const std::string slot = req.get_param_value("type");
    const json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.value("candidate_id", json()).is_number_integer()) {
      return SendError(res, 400, "invalid_json", "body must be {\"candidate_id\": <id>}");
    }
    const std::int64_t candidate_id = body["candidate_id"].get<std::int64_t>();
    artwork_selects_.Run([this, id, slot, candidate_id] {
      if (auto selected = metadata::SelectArtwork(config_, id, slot, candidate_id); !selected) {
        events_.Publish("game.artwork_select_failed",
                        {{"id", id}, {"type", slot}, {"error", selected.error().message}});
      } else {
        events_.Publish("game.artwork_selected", {{"id", id}, {"type", slot}});
      }
    });
    SendJson(res, {{"status", "selecting"}}, 202);
  });

  // Re-runs the fetch for one game on demand — a new SteamGridDB key was
  // just set, or the first automatic attempt failed transiently. Runs in the
  // background via metadata_fetches_ (force=true bypasses metadata.enabled:
  // an explicit refresh request should work even with automatic fetching
  // turned off), same as runner downloads: a slow or unreachable source must
  // never block the request.
  http_->Post(R"(/v1/games/([^/]+)/metadata/refresh)", [this](const Request& req, Response& res) {
    auto game = games_.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    // ?announce=1 marks this as user-initiated, so FetchQueue reports its
    // outcome as a notification — a bulk refresh (many games at once)
    // leaves it off and reports its own summary instead.
    const bool announce = req.get_param_value("announce") == "1";
    metadata_fetches_.Enqueue(config_, events_, *game, /*force=*/true, announce);
    SendJson(res, {{"status", "fetching"}}, 202);
  });

  // Bulk version of the above: enqueues a fetch for every game with no
  // cached cover art yet, in one request — a caller wanting to backfill the
  // whole library used to have to loop over it and fire one POST
  // .../metadata/refresh per game itself.
  http_->Post("/v1/games/metadata/refresh-missing", [this](const Request&, Response& res) {
    std::size_t count = 0;
    for (const model::Game& game : games_.All()) {
      if (HasCachedArtwork(config_, game.id)) continue;
      metadata_fetches_.Enqueue(config_, events_, game, /*force=*/true);
      ++count;
    }
    SendJson(res, {{"status", "fetching"}, {"count", count}}, 202);
  });

  // --- runners --------------------------------------------------------------

  http_->Get("/v1/runners", [this](const Request&, Response& res) {
    const runner::RunnerRegistry registry(config_);
    json out = json::array();
    for (const model::RunnerBuild& build : registry.DiscoverAll()) out.push_back(model::ToJson(build));
    SendJson(res, std::move(out));
  });

  // What's available to install, not what's installed (that's GET
  // /v1/runners above) — hits GitHub's API live, so it's the one endpoint
  // in this file with real network latency baked in.
  http_->Get("/v1/runners/catalog", [this](const Request& req, Response& res) {
    const std::string kind = req.has_param("kind") ? req.get_param_value("kind") : "proton";
    auto releases = runner::ListReleases(config_, kind);
    if (!releases) return SendError(res, 502, releases.error().code, releases.error().message);
    json out = json::array();
    for (const auto& r : *releases) {
      out.push_back({{"tag", r.tag}, {"asset_name", r.asset_name}, {"size_bytes", r.size_bytes},
                     {"published_at", r.published_at}, {"has_checksum", !r.checksum_url.empty()}});
    }
    SendJson(res, std::move(out));
  });

  // Downloads and installs a build from the catalog above. Runs detached —
  // a Proton-GE tarball is 500+ MB, minutes over a slow connection, and
  // there's no job queue yet (see docs/architecture.md) to track it
  // properly; runners.download.finished/failed on the event stream is how
  // a caller finds out it's done, the same pattern game launches already
  // use for "don't block the request thread on something slow."
  http_->Post("/v1/runners/download", [this](const Request& req, Response& res) {
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("kind") || !body.contains("tag")) {
      return SendError(res, 400, "invalid_body", R"(expected {"kind": "proton"|"wine", "tag": "..."})");
    }
    const std::string kind = body["kind"];
    const std::string tag = body["tag"];

    auto releases = runner::ListReleases(config_, kind);
    if (!releases) return SendError(res, 502, releases.error().code, releases.error().message);
    const auto match = std::ranges::find(*releases, tag, &runner::ReleaseAsset::tag);
    if (match == releases->end()) {
      return SendError(res, 404, "release_not_found", std::format("no {} release tagged \"{}\"", kind, tag));
    }

    const runner::ReleaseAsset asset = *match;
    events_.Publish("runners.download.started", {{"kind", kind}, {"tag", tag}});
    std::thread([this, kind, tag, asset] {
      if (auto installed = runner::DownloadAndInstall(config_, kind, asset); !installed) {
        log::Error("runner download failed ({} {}): {}", kind, tag, installed.error().message);
        events_.Publish("runners.download.failed",
                       {{"kind", kind}, {"tag", tag}, {"error", installed.error().message}});
      } else {
        log::Info("installed {} {}", kind, tag);
        events_.Publish("runners.download.finished", {{"kind", kind}, {"tag", tag}});
      }
    }).detach();

    SendJson(res, {{"status", "downloading"}, {"tag", tag}}, 202);
  });

  // What game.runner_config accepts for one kind — see IRunner::SettingsSchema
  // for why this exists (a frontend renders runner_config generically instead
  // of hardcoding per-runner knowledge; a custom runner with different knobs
  // needs no frontend change). Empty array for a kind with no fields, which
  // is every kind but proton today.
  http_->Get(R"(/v1/runners/([^/]+)/schema)", [this](const Request& req, Response& res) {
    const runner::RunnerRegistry registry(config_);
    const runner::IRunner* found = registry.FindByKind(req.matches[1]);
    if (!found) return SendError(res, 404, "unknown_runner_kind", "no runner of that kind");
    SendJson(res, found->SettingsSchema());
  });

  // Removes an installed build's directory (the other half of
  // GET /v1/runners/catalog + POST /v1/runners/download). Only deletes a
  // path that resolves to this exact build and sits inside a configured
  // search path — same containment check as DELETE /v1/games/{id} — so
  // "wine:system" (the real system wine, found on PATH) is rejected, not
  // deleted. A kind with no separate builds (native, steam) 400s.
  http_->Delete(R"(/v1/runners/([^:]+):(.+))", [this](const Request& req, Response& res) {
    const std::string kind = req.matches[1];
    const std::string name = req.matches[2];
    if (name == "auto" || name == "latest") {
      return SendError(res, 400, "invalid_reference", "name a concrete build, not \"auto\"/\"latest\"");
    }

    const runner::RunnerRegistry registry(config_);
    auto resolved = registry.Resolve(kind + ":" + name);
    if (!resolved) return SendError(res, 404, resolved.error().code, resolved.error().message);
    if (!resolved->build) {
      return SendError(res, 400, "not_a_build", std::format("\"{}\" has no separate installed builds", kind));
    }

    // Proton's build->path is already the build's own root directory; Wine's
    // is the wine binary inside it (<root>/bin/wine — see WineRunner.cpp),
    // so the actual directory to remove is two levels up.
    const std::filesystem::path build_path(resolved->build->path);
    const std::filesystem::path target = kind == "wine" ? build_path.parent_path().parent_path() : build_path;
    const std::vector<std::filesystem::path> roots = kind == "wine" ? config_.GetPathArray("wine_search_paths")
                                                                    : config_.GetPathArray("runner_search_paths");
    if (auto deleted = DeleteUnderRoot(target.string(), roots); !deleted) {
      return SendError(res, 400, deleted.error().code, deleted.error().message);
    }
    events_.Publish("runners.removed", {{"kind", kind}, {"name", name}});
    SendJson(res, json::object());
  });

  // --- events (SSE) -----------------------------------------------------

  http_->Get("/v1/events", [this](const Request& req, Response& res) {
    std::int64_t after_id = 0;
    if (auto it = req.headers.find("Last-Event-ID"); it != req.headers.end()) {
      after_id = std::atoll(it->second.c_str());
    }

    res.set_header("Cache-Control", "no-cache");
    // The 20s wait cadence exists only to detect a vanished client on this one
    // open connection; it is not a daemon-idle wakeup (see EventBus::WaitNext).
    res.set_chunked_content_provider(
        "text/event-stream",
        [this, after_id](size_t, httplib::DataSink& sink) mutable -> bool {
          if (stopping_.load(std::memory_order_relaxed)) return false;
          auto event = events_.WaitNext(after_id, stopping_, std::chrono::milliseconds(20000));
          if (!event) return sink.is_writable() && !stopping_.load(std::memory_order_relaxed);
          after_id = event->id;
          const std::string frame =
              std::format("id: {}\nevent: {}\ndata: {}\n\n", event->id, event->type,
                         event->payload.dump());
          return sink.write(frame.data(), frame.size());
        });
  });
}

}  // namespace mira::api
