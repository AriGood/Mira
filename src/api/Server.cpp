#include "api/Server.h"

#include <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <format>

#include <httplib.h>

#include "config/Resolver.h"
#include "config/Schema.h"
#include "core/Log.h"
#include "desktop/DesktopEntries.h"
#include "library/Scanner.h"
#include "runner/RunnerRegistry.h"

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

  http_->Get("/v1/games", [this](const Request& req, Response& res) {
    std::vector<model::Game> all = games_.All();
    json out = json::array();
    const auto status_filter = req.params.find("status");
    for (const model::Game& game : all) {
      if (status_filter != req.params.end() &&
          status_filter->second != model::ToString(game.status)) {
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
    SendJson(res, {{"added", summary.added}, {"missing", summary.missing},
                   {"restored", summary.restored}});
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

    const runner::RunnerRegistry registry(config_);
    auto resolved = registry.Resolve(game->runner_ref.empty() ? "native:native" : game->runner_ref);
    if (!resolved) return SendError(res, 400, resolved.error().code, resolved.error().message);

    auto command = resolved->runner->BuildCommand(*game, resolved->build);
    if (!command) return SendError(res, 400, command.error().code, command.error().message);

    ApplyCommandWrappers(*command, config_.GetStringArray("command_wrappers"));

    if (auto launched = supervisor_.Launch(*game, *command); !launched) {
      return SendError(res, 409, launched.error().code, launched.error().message);
    }
    SendJson(res, {{"status", "running"}});
  });

  http_->Post(R"(/v1/games/([^/]+)/stop)", [this](const Request& req, Response& res) {
    if (auto stopped = supervisor_.Stop(req.matches[1]); !stopped) {
      return SendError(res, 409, stopped.error().code, stopped.error().message);
    }
    SendJson(res, {{"status", "stopping"}});
  });

  // --- runners --------------------------------------------------------------

  http_->Get("/v1/runners", [this](const Request&, Response& res) {
    const runner::RunnerRegistry registry(config_);
    json out = json::array();
    for (const model::RunnerBuild& build : registry.DiscoverAll()) out.push_back(model::ToJson(build));
    SendJson(res, std::move(out));
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
