#include "MiradClient.h"

#include <json.hpp>

#include <utility>

#include "Async.h"
#include "JsonMapping.h"
#include "Transport.h"

namespace mira_gui {
namespace {

using nlohmann::json;

// Each of these is the blocking half of one endpoint, run on a worker thread
// by async::Run below. They are written as "ask transport, shape the reply"
// and nothing else — no socket, no timeouts, no error unwrapping.

HealthStatus GetHealthSync() {
  HealthStatus status;
  const transport::Reply reply = transport::Get("/v1/health");
  status.reachable = reply.ok;
  status.detail = reply.ok ? reply.body.value("status", std::string("ok")) : reply.error;
  return status;
}

GamesResult GetGamesSync(const std::string& status_filter) {
  GamesResult result;
  const transport::Reply reply = transport::Get(
      status_filter.empty() ? "/v1/games" : "/v1/games?status=" + status_filter);
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_array()) {
    result.error = transport::UnexpectedResponse("GET /v1/games");
    return result;
  }

  result.ok = true;
  for (const json& entry : reply.body) result.games.push_back(mapping::ToGameSummary(entry));
  return result;
}

DeleteResult DeleteGameSync(const std::string& id, bool delete_files, bool delete_prefix) {
  // Both flags are opt-in server-side too (docs/api.md): the bare DELETE
  // never touches disk, so an omitted param and "false" mean the same thing.
  std::string path = "/v1/games/" + id;
  std::string separator = "?";
  if (delete_files) {
    path += separator + "delete_files=true";
    separator = "&";
  }
  if (delete_prefix) path += separator + "delete_prefix=true";

  const transport::Reply reply = transport::Delete(path);
  return {reply.ok, reply.error};
}

LaunchResult LaunchGameSync(const std::string& id) {
  const transport::Reply reply = transport::Post("/v1/games/" + id + "/launch");
  return {reply.ok, reply.error};
}

StopResult StopGameSync(const std::string& id) {
  const transport::Reply reply = transport::Post("/v1/games/" + id + "/stop");
  return {reply.ok, reply.error};
}

ScanResult ScanLibrarySync() {
  ScanResult result;
  const transport::Reply reply =
      transport::Post("/v1/library/scan", {.read_timeout = std::chrono::seconds(30)});
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }

  result.ok = true;
  result.added = reply.body.value("added", 0);
  result.missing = reply.body.value("missing", 0);
  result.restored = reply.body.value("restored", 0);
  return result;
}

GameDetailResult GetGameSync(const std::string& id) {
  GameDetailResult result;
  const transport::Reply reply = transport::Get("/v1/games/" + id);
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_object()) {
    result.error = transport::UnexpectedResponse("GET /v1/games/" + id);
    return result;
  }

  result.ok = true;
  result.game = mapping::ToGameDetail(reply.body);
  return result;
}

PatchGameResult PatchGameSync(const std::string& id, const GamePatch& patch) {
  PatchGameResult result;
  json body = json::object();
  if (patch.name) body["name"] = *patch.name;
  if (patch.exe_path) body["exe_path"] = *patch.exe_path;
  if (patch.args) body["args"] = *patch.args;
  if (patch.working_dir) body["working_dir"] = *patch.working_dir;
  if (patch.runner_ref) body["runner_ref"] = *patch.runner_ref;
  if (patch.data_dir) body["data_dir"] = *patch.data_dir;

  // Parsed client-side rather than left for the server to reject: a bad
  // JSON object here is a typing mistake, not something worth a round trip
  // to discover.
  const auto parse_object = [&](const std::string& text, const char* field,
                                const char* message) -> bool {
    const json parsed = json::parse(text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
      result.error = message;
      return false;
    }
    body[field] = parsed;
    return true;
  };
  if (patch.runner_config_json &&
      !parse_object(*patch.runner_config_json, "runner_config",
                    "Runner config must be a JSON object, e.g. {}")) {
    return result;
  }
  if (patch.env_json && !parse_object(*patch.env_json, "env",
                                      "Environment must be a JSON object of strings, e.g. {}")) {
    return result;
  }

  const transport::Reply reply = transport::Patch("/v1/games/" + id, body);
  return {reply.ok, reply.error};
}

ConfigSchemaResult GetConfigSchemaSync() {
  ConfigSchemaResult result;
  const transport::Reply reply = transport::Get("/v1/config/schema");
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_array()) {
    result.error = transport::UnexpectedResponse("GET /v1/config/schema");
    return result;
  }

  result.ok = true;
  for (const json& entry : reply.body) {
    ConfigSchemaEntry e;
    e.key = entry.value("key", std::string());
    e.type = entry.value("type", std::string());
    e.tier = entry.value("tier", std::string());
    e.doc = entry.value("doc", std::string());
    if (entry.contains("default")) e.default_display = mapping::ToDisplayString(entry["default"]);
    result.entries.push_back(std::move(e));
  }
  return result;
}

ConfigResult GetConfigSync() {
  ConfigResult result;
  const transport::Reply reply = transport::Get("/v1/config");
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_object()) {
    result.error = transport::UnexpectedResponse("GET /v1/config");
    return result;
  }

  result.ok = true;
  mapping::FlattenConfig(reply.body, "", result.values);
  return result;
}

PatchConfigResult PatchConfigSync(const std::vector<ConfigEdit>& edits) {
  json body = json::object();
  for (const ConfigEdit& edit : edits) {
    mapping::AssignDottedKey(body, edit.key, mapping::TypedValueFromText(edit.type, edit.value));
  }
  const transport::Reply reply = transport::Patch("/v1/config", body);
  return {reply.ok, reply.error};
}

PatchConfigResult ResetConfigKeySync(const std::string& key) {
  const transport::Reply reply = transport::Post("/v1/config/reset?key=" + key);
  return {reply.ok, reply.error};
}

RunnersResult GetRunnersSync() {
  RunnersResult result;
  const transport::Reply reply = transport::Get("/v1/runners");
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_array()) {
    result.error = transport::UnexpectedResponse("GET /v1/runners");
    return result;
  }

  result.ok = true;
  for (const json& entry : reply.body) {
    RunnerInfo runner;
    runner.kind = entry.value("kind", std::string());
    runner.name = entry.value("name", std::string());
    runner.version = entry.value("version", std::string());
    runner.reference = entry.value("reference", std::string());
    result.runners.push_back(std::move(runner));
  }
  return result;
}

GameConfigResult GetGameConfigSync(const std::string& id) {
  GameConfigResult result;
  const transport::Reply reply = transport::Get("/v1/games/" + id + "/config");
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_object()) {
    result.error = transport::UnexpectedResponse("GET /v1/games/" + id + "/config");
    return result;
  }

  result.ok = true;
  // Schema::Entries() order (Server.cpp's EffectiveDocument iterates it
  // directly), so this comes out in the same stable order GET
  // /v1/config/schema does — useful for a caller joining the two by key.
  for (const auto& [key, entry] : reply.body.items()) {
    GameConfigEntry e;
    e.key = key;
    e.value_display = mapping::ToDisplayString(entry.value("value", json()));
    e.layer = entry.value("layer", std::string());
    e.overridable = entry.value("overridable", false);
    result.entries.push_back(std::move(e));
  }
  return result;
}

PatchGameConfigResult PatchGameConfigSync(const std::string& id,
                                          const std::vector<GameConfigEdit>& edits) {
  json body = json::object();
  for (const GameConfigEdit& edit : edits) {
    body[edit.key] =
        edit.clear ? json(nullptr) : mapping::TypedValueFromText(edit.type, edit.value);
  }
  const transport::Reply reply = transport::Patch("/v1/games/" + id + "/config", body);
  return {reply.ok, reply.error};
}

}  // namespace

std::string MiradClient::ResolveSocketPath() { return transport::SocketPath(); }

void MiradClient::CheckHealthAsync(QObject* context, std::function<void(HealthStatus)> callback) {
  async::Run(context, [] { return GetHealthSync(); }, std::move(callback));
}

void MiradClient::ListGamesAsync(QObject* context, std::function<void(GamesResult)> callback,
                                 const std::string& status_filter) {
  async::Run(context, [status_filter] { return GetGamesSync(status_filter); }, std::move(callback));
}

void MiradClient::DeleteGameAsync(QObject* context, const std::string& id, bool delete_files,
                                  bool delete_prefix,
                                  std::function<void(DeleteResult)> callback) {
  async::Run(
      context, [id, delete_files, delete_prefix] {
        return DeleteGameSync(id, delete_files, delete_prefix);
      },
      std::move(callback));
}

void MiradClient::LaunchGameAsync(QObject* context, const std::string& id,
                                  std::function<void(LaunchResult)> callback) {
  async::Run(context, [id] { return LaunchGameSync(id); }, std::move(callback));
}

void MiradClient::StopGameAsync(QObject* context, const std::string& id,
                                std::function<void(StopResult)> callback) {
  async::Run(context, [id] { return StopGameSync(id); }, std::move(callback));
}

void MiradClient::ScanLibraryAsync(QObject* context, std::function<void(ScanResult)> callback) {
  async::Run(context, [] { return ScanLibrarySync(); }, std::move(callback));
}

void MiradClient::GetGameAsync(QObject* context, const std::string& id,
                               std::function<void(GameDetailResult)> callback) {
  async::Run(context, [id] { return GetGameSync(id); }, std::move(callback));
}

void MiradClient::PatchGameAsync(QObject* context, const std::string& id, const GamePatch& patch,
                                 std::function<void(PatchGameResult)> callback) {
  async::Run(context, [id, patch] { return PatchGameSync(id, patch); }, std::move(callback));
}

void MiradClient::GetConfigSchemaAsync(QObject* context,
                                       std::function<void(ConfigSchemaResult)> callback) {
  async::Run(context, [] { return GetConfigSchemaSync(); }, std::move(callback));
}

void MiradClient::GetConfigAsync(QObject* context, std::function<void(ConfigResult)> callback) {
  async::Run(context, [] { return GetConfigSync(); }, std::move(callback));
}

void MiradClient::PatchConfigAsync(QObject* context, const std::vector<ConfigEdit>& edits,
                                   std::function<void(PatchConfigResult)> callback) {
  async::Run(context, [edits] { return PatchConfigSync(edits); }, std::move(callback));
}

void MiradClient::ResetConfigKeyAsync(QObject* context, const std::string& key,
                                      std::function<void(PatchConfigResult)> callback) {
  async::Run(context, [key] { return ResetConfigKeySync(key); }, std::move(callback));
}

void MiradClient::ListRunnersAsync(QObject* context, std::function<void(RunnersResult)> callback) {
  async::Run(context, [] { return GetRunnersSync(); }, std::move(callback));
}

void MiradClient::GetGameConfigAsync(QObject* context, const std::string& id,
                                     std::function<void(GameConfigResult)> callback) {
  async::Run(context, [id] { return GetGameConfigSync(id); }, std::move(callback));
}

void MiradClient::PatchGameConfigAsync(QObject* context, const std::string& id,
                                       const std::vector<GameConfigEdit>& edits,
                                       std::function<void(PatchGameConfigResult)> callback) {
  async::Run(context, [id, edits] { return PatchGameConfigSync(id, edits); }, std::move(callback));
}

bool MiradClient::ParseGameSummary(const std::string& data, GameSummary* out) {
  const json entry = json::parse(data, nullptr, false);
  if (entry.is_discarded() || !entry.is_object()) return false;
  *out = mapping::ToGameSummary(entry);
  return true;
}

bool MiradClient::ParseGameState(const std::string& data, GameStateEvent* out) {
  const json entry = json::parse(data, nullptr, false);
  if (entry.is_discarded() || !entry.is_object()) return false;
  out->id = entry.value("id", std::string());
  out->state = entry.value("state", std::string());
  return !out->id.empty();
}

std::string MiradClient::ParseRemovedId(const std::string& data) {
  const json entry = json::parse(data, nullptr, false);
  if (entry.is_discarded() || !entry.is_object()) return {};
  return entry.value("id", std::string());
}

}  // namespace mira_gui
