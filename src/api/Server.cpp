#include "api/Server.h"

#include <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <thread>

#include <httplib.h>

#include "config/Resolver.h"
#include "config/Schema.h"
#include "core/Log.h"
#include "desktop/DesktopEntries.h"
#include "library/Scanner.h"
#include "metadata/MetadataFetcher.h"
#include "runner/Downloader.h"
#include "runner/Exec.h"
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
// entry ends up outermost, so ["gamescope", "mangohud"] runs
// `gamescope mangohud <game>`.
void ApplyCommandWrappers(Command& command, const std::vector<std::string>& wrappers) {
  for (auto it = wrappers.rbegin(); it != wrappers.rend(); ++it) {
    if (it->empty()) continue;
    command.argv.insert(command.argv.begin(), *it);
  }
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

Server::~Server() = default;

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
          {"type", config::ToString(entry.type)},
          {"default", entry.default_value},
          {"tier", config::ToString(entry.tier)},
          {"doc", entry.doc},
      });
      // Present only when there's a shape to describe.
      if (!entry.constraint.one_of.empty()) entries.back()["one_of"] = entry.constraint.one_of;
      if (entry.constraint.minimum) entries.back()["minimum"] = *entry.constraint.minimum;
      if (entry.constraint.maximum) entries.back()["maximum"] = *entry.constraint.maximum;
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
      out.push_back(model::ToJson(game));
    }
    SendJson(res, std::move(out));
  });

  http_->Get(R"(/v1/games/([^/]+))", [this](const Request& req, Response& res) {
    auto game = games_.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    SendJson(res, model::ToJson(*game));
  });

  http_->Patch(R"(/v1/games/([^/]+))", [this](const Request& req, Response& res) {
    const std::string id = req.matches[1];
    json patch = json::parse(req.body, nullptr, false);
    if (patch.is_discarded()) return SendError(res, 400, "invalid_json", "body is not valid JSON");

    auto result = games_.Update(id, [&](model::Game& game) { game = ParseGamePatch(game, patch); });
    if (!result) return SendError(res, 404, result.error().code, result.error().message);
    SyncDesktopEntries(config_, games_);
    events_.Publish("game.updated", model::ToJson(*result));
    SendJson(res, model::ToJson(*result));
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
    SendJson(res, model::ToJson(*result));
  });

  http_->Delete(R"(/v1/games/([^/]+))", [this](const Request& req, Response& res) {
    auto game = games_.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");

    // Opt-in, and deliberately narrow: only ever deletes a path this game's
    // own record points at, and only if that path is really inside a
    // configured root — never wherever install_path/data_dir happen to say,
    // in case a hand-edited games.toml points somewhere it shouldn't.
    if (req.has_param("delete_files") && req.get_param_value("delete_files") == "true") {
      if (auto deleted = DeleteUnderRoot(game->install_path, config_.GetPathArray("library_roots")); !deleted) {
        return SendError(res, 400, deleted.error().code, deleted.error().message);
      }
    }
    if (req.has_param("delete_prefix") && req.get_param_value("delete_prefix") == "true") {
      if (auto deleted = DeleteUnderRoot(game->data_dir, {config_.GetPath("prefix_root")}); !deleted) {
        return SendError(res, 400, deleted.error().code, deleted.error().message);
      }
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
    if (!pre_script.empty()) {
      Command script;
      script.argv = {"sh", "-c", pre_script};
      const Result<runner::ExecResult> ran = runner::RunAndWait(script);
      if (!ran || ran->exit_code != 0) {
        return SendError(res, 409, "pre_launch_failed",
                         !ran ? ran.error().message
                             : std::format("launch.pre_script exited {}: {}", ran->exit_code, ran->output));
      }
    }

    // A Steam-sourced game defaults to asking the Steam client to launch it
    // (steam://rungameid/<appid>) rather than Mira execing it directly: full
    // achievements/overlay support, and Steam's own accounting is what
    // GET /v1/games/{id} reads back for it (see steam.launch_mode's doc).
    // Mira didn't spawn this process, so it can't waitpid() it — that's what
    // steam.track_process (a /proc scan, see ProcessSupervisor) is for.
    if (game->runner_ref.starts_with("steam:")) {
      if (resolver.GetString("steam.launch_mode") == "steam") {
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
        events_.PublishNotification(model::NotifyLevel::Info,
                                    "Handed to Steam. Mira does not track Steam-launched sessions — "
                                    "Steam keeps its own playtime.");
        if (track) {
          if (auto started = supervisor_.TrackSteamLaunch(*game, appid, post_script); !started) {
            log::Warn("couldn't start tracking {}: {}", game->id, started.error().message);
          }
        }
        return SendJson(res, {{"status", "launched_via_steam"}, {"tracked", track}});
      }
    }

    const runner::RunnerRegistry registry(config_);
    auto resolved = registry.Resolve(game->runner_ref.empty() ? "native:native" : game->runner_ref);
    if (!resolved) return SendError(res, 400, resolved.error().code, resolved.error().message);

    auto command = resolved->runner->BuildCommand(*game, resolved->build);
    if (!command) return SendError(res, 400, command.error().code, command.error().message);

    ApplyCommandWrappers(*command, config_.GetStringArray("command_wrappers"));

    if (auto launched = supervisor_.Launch(*game, *command, post_script); !launched) {
      return SendError(res, 409, launched.error().code, launched.error().message);
    }
    SendJson(res, {{"status", "running"}, {"tracked", true}});  // always true: Mira spawned it
  });

  http_->Post(R"(/v1/games/([^/]+)/stop)", [this](const Request& req, Response& res) {
    if (auto stopped = supervisor_.Stop(req.matches[1]); !stopped) {
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
    auto resolved = registry.Resolve(game->runner_ref.empty() ? "native:native" : game->runner_ref);
    if (!resolved) return SendError(res, 400, resolved.error().code, resolved.error().message);

    model::Game run_as = *game;
    run_as.exe_path = exe_path;
    run_as.args = args;

    auto command = resolved->runner->BuildCommand(run_as, resolved->build);
    if (!command) return SendError(res, 400, command.error().code, command.error().message);
    ApplyCommandWrappers(*command, config_.GetStringArray("command_wrappers"));

    if (auto launched = supervisor_.Launch(*game, *command); !launched) {
      return SendError(res, 409, launched.error().code, launched.error().message);
    }
    SendJson(res, {{"status", "running"}});
  });

  // The escape hatch out of needs_install: run the installer via /run
  // above, PATCH exe_path to whatever it actually installed, then call
  // this to make the game launchable through the normal /launch path.
  http_->Post(R"(/v1/games/([^/]+)/finish-install)", [this](const Request& req, Response& res) {
    auto game = games_.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    if (game->exe_path.empty()) {
      return SendError(res, 409, "no_executable",
                       "PATCH exe_path to the installed game's real executable first");
    }
    auto result = games_.Update(game->id, [](model::Game& g) {
      g.status = model::GameStatus::Ready;
      g.last_error.clear();
    });
    if (!result) return SendError(res, 404, result.error().code, result.error().message);
    SyncDesktopEntries(config_, games_);
    events_.Publish("game.updated", model::ToJson(*result));
    SendJson(res, model::ToJson(*result));
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
