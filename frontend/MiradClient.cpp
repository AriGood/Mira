#include "MiradClient.h"

#include <httplib.h>
#include <json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

namespace mira_gui {

using nlohmann::json;

std::string MiradClient::ResolveSocketPath() {
  const char* override_path = std::getenv("MIRA_SOCKET");
  if (override_path && *override_path) return override_path;

  const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
  const std::filesystem::path base = runtime_dir && *runtime_dir ? runtime_dir : "/tmp";
  return (base / "mira" / "mirad.sock").string();
}

namespace {

GameSummary ParseGameSummaryJson(const json& entry) {
  GameSummary game;
  game.id = entry.value("id", std::string());
  game.name = entry.value("name", std::string());
  game.status = entry.value("status", std::string());
  game.platform = entry.value("platform", std::string());
  game.runner_ref = entry.value("runner_ref", std::string());
  game.last_error = entry.value("last_error", std::string());
  game.reviewed = entry.value("reviewed", false);
  game.confidence = entry.value("confidence", 0.0);
  if (entry.contains("last_played_at") && entry["last_played_at"].is_number()) {
    game.last_played_at = entry["last_played_at"].get<std::int64_t>();
  }
  game.play_seconds = entry.value("play_seconds", std::int64_t{0});
  return game;
}

GameDetail ParseGameDetailJson(const json& entry) {
  GameDetail game;
  game.id = entry.value("id", std::string());
  game.name = entry.value("name", std::string());
  game.status = entry.value("status", std::string());
  game.platform = entry.value("platform", std::string());
  game.install_path = entry.value("install_path", std::string());
  game.exe_path = entry.value("exe_path", std::string());
  game.args = entry.value("args", std::string());
  game.working_dir = entry.value("working_dir", std::string());
  game.runner_ref = entry.value("runner_ref", std::string());
  game.data_dir = entry.value("data_dir", std::string());
  game.last_error = entry.value("last_error", std::string());
  game.reviewed = entry.value("reviewed", false);
  game.confidence = entry.value("confidence", 0.0);
  if (entry.contains("last_played_at") && entry["last_played_at"].is_number()) {
    game.last_played_at = entry["last_played_at"].get<std::int64_t>();
  }
  game.play_seconds = entry.value("play_seconds", std::int64_t{0});
  game.runner_config_json = entry.value("runner_config", json::object()).dump(2);
  game.env_json = entry.value("env", json::object()).dump(2);

  for (const json& candidate : entry.value("candidates", json::array())) {
    GameDetail::Candidate c;
    c.rel_path = candidate.value("rel_path", std::string());
    c.kind = candidate.value("kind", std::string());
    c.score = candidate.value("score", 0.0);
    c.chosen = candidate.value("chosen", false);
    c.is_installer = candidate.value("is_installer", false);
    game.candidates.push_back(std::move(c));
  }
  return game;
}

// The message an httplib::Result's own error carries when the request
// never got a response at all (socket unreachable), vs. the `message` field
// of the {"error": {...}} envelope every mirad error response uses
// (docs/api.md) when it did.
std::string DescribeError(const httplib::Result& res, const std::string& socket_path) {
  if (res) {
    const json body = json::parse(res->body, nullptr, false);
    return body.is_discarded() ? res->body
                                : body.value("error", json::object()).value("message", res->body);
  }
  return "cannot reach mirad at " + socket_path + " (" + httplib::to_string(res.error()) +
         ") — is it running?";
}

HealthStatus GetHealthSync() {
  HealthStatus status;
  const std::string socket_path = MiradClient::ResolveSocketPath();

  httplib::Client client(socket_path, 80);
  client.set_address_family(AF_UNIX);
  client.set_connection_timeout(std::chrono::seconds(2));

  auto res = client.Get("/v1/health");
  if (res && res->status >= 200 && res->status < 300) {
    const json body = json::parse(res->body, nullptr, false);
    status.reachable = true;
    status.detail = body.is_discarded() ? res->body : body.value("status", std::string("ok"));
    return status;
  }

  status.reachable = false;
  status.detail = DescribeError(res, socket_path);
  return status;
}

GamesResult GetGamesSync(const std::string& status_filter) {
  GamesResult result;
  const std::string socket_path = MiradClient::ResolveSocketPath();

  httplib::Client client(socket_path, 80);
  client.set_address_family(AF_UNIX);
  client.set_connection_timeout(std::chrono::seconds(2));

  const std::string path = status_filter.empty() ? "/v1/games" : "/v1/games?status=" + status_filter;
  auto res = client.Get(path);
  if (!res || res->status < 200 || res->status >= 300) {
    result.ok = false;
    result.error = DescribeError(res, socket_path);
    return result;
  }

  const json body = json::parse(res->body, nullptr, false);
  if (body.is_discarded() || !body.is_array()) {
    result.ok = false;
    result.error = "mirad returned an unexpected response for GET /v1/games";
    return result;
  }

  result.ok = true;
  for (const json& entry : body) result.games.push_back(ParseGameSummaryJson(entry));
  return result;
}

DeleteResult DeleteGameSync(const std::string& id) {
  DeleteResult result;
  const std::string socket_path = MiradClient::ResolveSocketPath();

  httplib::Client client(socket_path, 80);
  client.set_address_family(AF_UNIX);
  client.set_connection_timeout(std::chrono::seconds(2));

  auto res = client.Delete("/v1/games/" + id);
  if (res && res->status >= 200 && res->status < 300) {
    result.ok = true;
    return result;
  }

  result.ok = false;
  result.error = DescribeError(res, socket_path);
  return result;
}

LaunchResult LaunchGameSync(const std::string& id) {
  LaunchResult result;
  const std::string socket_path = MiradClient::ResolveSocketPath();

  httplib::Client client(socket_path, 80);
  client.set_address_family(AF_UNIX);
  client.set_connection_timeout(std::chrono::seconds(2));

  auto res = client.Post("/v1/games/" + id + "/launch");
  if (res && res->status >= 200 && res->status < 300) {
    result.ok = true;
    return result;
  }

  result.ok = false;
  result.error = DescribeError(res, socket_path);
  return result;
}

StopResult StopGameSync(const std::string& id) {
  StopResult result;
  const std::string socket_path = MiradClient::ResolveSocketPath();

  httplib::Client client(socket_path, 80);
  client.set_address_family(AF_UNIX);
  client.set_connection_timeout(std::chrono::seconds(2));

  auto res = client.Post("/v1/games/" + id + "/stop");
  if (res && res->status >= 200 && res->status < 300) {
    result.ok = true;
    return result;
  }

  result.ok = false;
  result.error = DescribeError(res, socket_path);
  return result;
}

ScanResult ScanLibrarySync() {
  ScanResult result;
  const std::string socket_path = MiradClient::ResolveSocketPath();

  httplib::Client client(socket_path, 80);
  client.set_address_family(AF_UNIX);
  client.set_connection_timeout(std::chrono::seconds(2));
  client.set_read_timeout(std::chrono::seconds(30));  // walks every library root

  auto res = client.Post("/v1/library/scan");
  if (!res || res->status < 200 || res->status >= 300) {
    result.ok = false;
    result.error = DescribeError(res, socket_path);
    return result;
  }

  const json body = json::parse(res->body, nullptr, false);
  result.ok = true;
  result.added = body.value("added", 0);
  result.missing = body.value("missing", 0);
  result.restored = body.value("restored", 0);
  return result;
}

GameDetailResult GetGameSync(const std::string& id) {
  GameDetailResult result;
  const std::string socket_path = MiradClient::ResolveSocketPath();

  httplib::Client client(socket_path, 80);
  client.set_address_family(AF_UNIX);
  client.set_connection_timeout(std::chrono::seconds(2));

  auto res = client.Get("/v1/games/" + id);
  if (!res || res->status < 200 || res->status >= 300) {
    result.ok = false;
    result.error = DescribeError(res, socket_path);
    return result;
  }

  const json body = json::parse(res->body, nullptr, false);
  if (body.is_discarded() || !body.is_object()) {
    result.ok = false;
    result.error = "mirad returned an unexpected response for GET /v1/games/" + id;
    return result;
  }

  result.ok = true;
  result.game = ParseGameDetailJson(body);
  return result;
}

PatchGameResult PatchGameSync(const std::string& id, const GamePatch& patch) {
  PatchGameResult result;
  const std::string socket_path = MiradClient::ResolveSocketPath();

  httplib::Client client(socket_path, 80);
  client.set_address_family(AF_UNIX);
  client.set_connection_timeout(std::chrono::seconds(2));

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
  if (patch.runner_config_json) {
    const json parsed = json::parse(*patch.runner_config_json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
      result.ok = false;
      result.error = "Runner config must be a JSON object, e.g. {}";
      return result;
    }
    body["runner_config"] = parsed;
  }
  if (patch.env_json) {
    const json parsed = json::parse(*patch.env_json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
      result.ok = false;
      result.error = "Environment must be a JSON object of strings, e.g. {}";
      return result;
    }
    body["env"] = parsed;
  }

  auto res = client.Patch("/v1/games/" + id, body.dump(), "application/json");
  if (!res || res->status < 200 || res->status >= 300) {
    result.ok = false;
    result.error = DescribeError(res, socket_path);
    return result;
  }

  result.ok = true;
  return result;
}

std::string JsonToDisplayString(const json& value) {
  if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
  if (value.is_number_integer()) return std::to_string(value.get<std::int64_t>());
  if (value.is_number()) return value.dump();
  if (value.is_string()) return value.get<std::string>();
  if (value.is_array()) {
    std::string joined;
    for (const json& item : value) {
      if (!joined.empty()) joined += ", ";
      joined += item.is_string() ? item.get<std::string>() : item.dump();
    }
    return joined;
  }
  return value.dump();
}

void FlattenConfig(const json& node, const std::string& prefix,
                    std::map<std::string, std::string>& out) {
  if (!node.is_object()) {
    out[prefix] = JsonToDisplayString(node);
    return;
  }
  for (const auto& [key, value] : node.items()) {
    if (prefix.empty() && key == "frontend") continue;  // opaque, not part of the schema
    const std::string dotted = prefix.empty() ? key : prefix + "." + key;
    if (value.is_object()) {
      FlattenConfig(value, dotted, out);
    } else {
      out[dotted] = JsonToDisplayString(value);
    }
  }
}

// A trimmed, non-empty comma split — turns a StringArray edit box's text
// ("a, b, c") back into entries. The inverse of JsonToDisplayString's ", "
// join, so round-tripping an unedited field is a no-op.
std::vector<std::string> SplitCommaSeparated(const std::string& text) {
  std::vector<std::string> items;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find(',', start);
    if (end == std::string::npos) end = text.size();
    std::string item = text.substr(start, end - start);
    const size_t first = item.find_first_not_of(" \t\r\n");
    const size_t last = item.find_last_not_of(" \t\r\n");
    if (first != std::string::npos) items.push_back(item.substr(first, last - first + 1));
    start = end + 1;
  }
  return items;
}

ConfigSchemaResult GetConfigSchemaSync() {
  ConfigSchemaResult result;
  const std::string socket_path = MiradClient::ResolveSocketPath();

  httplib::Client client(socket_path, 80);
  client.set_address_family(AF_UNIX);
  client.set_connection_timeout(std::chrono::seconds(2));

  auto res = client.Get("/v1/config/schema");
  if (!res || res->status < 200 || res->status >= 300) {
    result.ok = false;
    result.error = DescribeError(res, socket_path);
    return result;
  }

  const json body = json::parse(res->body, nullptr, false);
  if (body.is_discarded() || !body.is_array()) {
    result.ok = false;
    result.error = "mirad returned an unexpected response for GET /v1/config/schema";
    return result;
  }

  result.ok = true;
  for (const json& entry : body) {
    ConfigSchemaEntry e;
    e.key = entry.value("key", std::string());
    e.type = entry.value("type", std::string());
    e.tier = entry.value("tier", std::string());
    e.doc = entry.value("doc", std::string());
    if (entry.contains("default")) e.default_display = JsonToDisplayString(entry["default"]);
    result.entries.push_back(std::move(e));
  }
  return result;
}

ConfigResult GetConfigSync() {
  ConfigResult result;
  const std::string socket_path = MiradClient::ResolveSocketPath();

  httplib::Client client(socket_path, 80);
  client.set_address_family(AF_UNIX);
  client.set_connection_timeout(std::chrono::seconds(2));

  auto res = client.Get("/v1/config");
  if (!res || res->status < 200 || res->status >= 300) {
    result.ok = false;
    result.error = DescribeError(res, socket_path);
    return result;
  }

  const json body = json::parse(res->body, nullptr, false);
  if (body.is_discarded() || !body.is_object()) {
    result.ok = false;
    result.error = "mirad returned an unexpected response for GET /v1/config";
    return result;
  }

  result.ok = true;
  FlattenConfig(body, "", result.values);
  return result;
}

// Converts a field's edited display text back to the JSON kind its schema
// type calls for, so a value round-trips through the display strings both
// PatchConfigSync and PatchGameConfigSync work with. Falls back to sending
// the raw text for a malformed number — the server's own validator explains
// the bad input better than guessing client-side would.
json TypedValueFromText(const std::string& type, const std::string& value) {
  if (type == "a boolean") return value == "true";
  if (type == "an integer") {
    try {
      return static_cast<std::int64_t>(std::stoll(value));
    } catch (...) {
      return value;
    }
  }
  if (type == "a number") {
    try {
      return std::stod(value);
    } catch (...) {
      return value;
    }
  }
  if (type == "an array of strings") return SplitCommaSeparated(value);
  return value;
}

PatchConfigResult PatchConfigSync(const std::vector<ConfigEdit>& edits) {
  PatchConfigResult result;
  const std::string socket_path = MiradClient::ResolveSocketPath();

  httplib::Client client(socket_path, 80);
  client.set_address_family(AF_UNIX);
  client.set_connection_timeout(std::chrono::seconds(2));

  json body = json::object();
  for (const ConfigEdit& edit : edits) {
    const json value = TypedValueFromText(edit.type, edit.value);

    json* cursor = &body;
    size_t start = 0;
    while (true) {
      const size_t dot = edit.key.find('.', start);
      const std::string segment =
          edit.key.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
      if (dot == std::string::npos) {
        (*cursor)[segment] = value;
        break;
      }
      cursor = &(*cursor)[segment];
      start = dot + 1;
    }
  }

  auto res = client.Patch("/v1/config", body.dump(), "application/json");
  if (!res || res->status < 200 || res->status >= 300) {
    result.ok = false;
    result.error = DescribeError(res, socket_path);
    return result;
  }

  result.ok = true;
  return result;
}

PatchConfigResult ResetConfigKeySync(const std::string& key) {
  PatchConfigResult result;
  const std::string socket_path = MiradClient::ResolveSocketPath();

  httplib::Client client(socket_path, 80);
  client.set_address_family(AF_UNIX);
  client.set_connection_timeout(std::chrono::seconds(2));

  auto res = client.Post("/v1/config/reset?key=" + key);
  if (!res || res->status < 200 || res->status >= 300) {
    result.ok = false;
    result.error = DescribeError(res, socket_path);
    return result;
  }

  result.ok = true;
  return result;
}

RunnersResult GetRunnersSync() {
  RunnersResult result;
  const std::string socket_path = MiradClient::ResolveSocketPath();

  httplib::Client client(socket_path, 80);
  client.set_address_family(AF_UNIX);
  client.set_connection_timeout(std::chrono::seconds(2));

  auto res = client.Get("/v1/runners");
  if (!res || res->status < 200 || res->status >= 300) {
    result.ok = false;
    result.error = DescribeError(res, socket_path);
    return result;
  }

  const json body = json::parse(res->body, nullptr, false);
  if (body.is_discarded() || !body.is_array()) {
    result.ok = false;
    result.error = "mirad returned an unexpected response for GET /v1/runners";
    return result;
  }

  result.ok = true;
  for (const json& entry : body) {
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
  const std::string socket_path = MiradClient::ResolveSocketPath();

  httplib::Client client(socket_path, 80);
  client.set_address_family(AF_UNIX);
  client.set_connection_timeout(std::chrono::seconds(2));

  auto res = client.Get("/v1/games/" + id + "/config");
  if (!res || res->status < 200 || res->status >= 300) {
    result.ok = false;
    result.error = DescribeError(res, socket_path);
    return result;
  }

  const json body = json::parse(res->body, nullptr, false);
  if (body.is_discarded() || !body.is_object()) {
    result.ok = false;
    result.error = "mirad returned an unexpected response for GET /v1/games/" + id + "/config";
    return result;
  }

  result.ok = true;
  // Schema::Entries() order (Server.cpp's EffectiveDocument iterates it
  // directly), so this comes out in the same stable order GET
  // /v1/config/schema does — useful for a caller joining the two by key.
  for (const auto& [key, entry] : body.items()) {
    GameConfigEntry e;
    e.key = key;
    e.value_display = JsonToDisplayString(entry.value("value", json()));
    e.layer = entry.value("layer", std::string());
    e.overridable = entry.value("overridable", false);
    result.entries.push_back(std::move(e));
  }
  return result;
}

PatchGameConfigResult PatchGameConfigSync(const std::string& id,
                                          const std::vector<GameConfigEdit>& edits) {
  PatchGameConfigResult result;
  const std::string socket_path = MiradClient::ResolveSocketPath();

  httplib::Client client(socket_path, 80);
  client.set_address_family(AF_UNIX);
  client.set_connection_timeout(std::chrono::seconds(2));

  json body = json::object();
  for (const GameConfigEdit& edit : edits) {
    body[edit.key] = edit.clear ? json(nullptr) : TypedValueFromText(edit.type, edit.value);
  }

  auto res = client.Patch("/v1/games/" + id + "/config", body.dump(), "application/json");
  if (!res || res->status < 200 || res->status >= 300) {
    result.ok = false;
    result.error = DescribeError(res, socket_path);
    return result;
  }

  result.ok = true;
  return result;
}

}  // namespace

void MiradClient::CheckHealthAsync(QObject* context, std::function<void(HealthStatus)> callback) {
  std::thread([context, callback = std::move(callback)]() {
    HealthStatus status = GetHealthSync();
    QMetaObject::invokeMethod(
        context, [callback, status = std::move(status)]() mutable { callback(std::move(status)); },
        Qt::QueuedConnection);
  }).detach();
}

void MiradClient::ListGamesAsync(QObject* context, std::function<void(GamesResult)> callback,
                                 const std::string& status_filter) {
  std::thread([context, callback = std::move(callback), status_filter]() {
    GamesResult result = GetGamesSync(status_filter);
    QMetaObject::invokeMethod(
        context, [callback, result = std::move(result)]() mutable { callback(std::move(result)); },
        Qt::QueuedConnection);
  }).detach();
}

void MiradClient::DeleteGameAsync(QObject* context, const std::string& id,
                                  std::function<void(DeleteResult)> callback) {
  std::thread([context, id, callback = std::move(callback)]() {
    DeleteResult result = DeleteGameSync(id);
    QMetaObject::invokeMethod(
        context, [callback, result = std::move(result)]() mutable { callback(std::move(result)); },
        Qt::QueuedConnection);
  }).detach();
}

void MiradClient::LaunchGameAsync(QObject* context, const std::string& id,
                                  std::function<void(LaunchResult)> callback) {
  std::thread([context, id, callback = std::move(callback)]() {
    LaunchResult result = LaunchGameSync(id);
    QMetaObject::invokeMethod(
        context, [callback, result = std::move(result)]() mutable { callback(std::move(result)); },
        Qt::QueuedConnection);
  }).detach();
}

void MiradClient::StopGameAsync(QObject* context, const std::string& id,
                                std::function<void(StopResult)> callback) {
  std::thread([context, id, callback = std::move(callback)]() {
    StopResult result = StopGameSync(id);
    QMetaObject::invokeMethod(
        context, [callback, result = std::move(result)]() mutable { callback(std::move(result)); },
        Qt::QueuedConnection);
  }).detach();
}

bool MiradClient::ParseGameSummary(const std::string& data, GameSummary* out) {
  const json entry = json::parse(data, nullptr, false);
  if (entry.is_discarded() || !entry.is_object()) return false;
  *out = ParseGameSummaryJson(entry);
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

void MiradClient::ScanLibraryAsync(QObject* context, std::function<void(ScanResult)> callback) {
  std::thread([context, callback = std::move(callback)]() {
    ScanResult result = ScanLibrarySync();
    QMetaObject::invokeMethod(
        context, [callback, result = std::move(result)]() mutable { callback(std::move(result)); },
        Qt::QueuedConnection);
  }).detach();
}

void MiradClient::GetGameAsync(QObject* context, const std::string& id,
                               std::function<void(GameDetailResult)> callback) {
  std::thread([context, id, callback = std::move(callback)]() {
    GameDetailResult result = GetGameSync(id);
    QMetaObject::invokeMethod(
        context, [callback, result = std::move(result)]() mutable { callback(std::move(result)); },
        Qt::QueuedConnection);
  }).detach();
}

void MiradClient::PatchGameAsync(QObject* context, const std::string& id, const GamePatch& patch,
                                 std::function<void(PatchGameResult)> callback) {
  std::thread([context, id, patch, callback = std::move(callback)]() {
    PatchGameResult result = PatchGameSync(id, patch);
    QMetaObject::invokeMethod(
        context, [callback, result = std::move(result)]() mutable { callback(std::move(result)); },
        Qt::QueuedConnection);
  }).detach();
}

void MiradClient::GetConfigSchemaAsync(QObject* context,
                                       std::function<void(ConfigSchemaResult)> callback) {
  std::thread([context, callback = std::move(callback)]() {
    ConfigSchemaResult result = GetConfigSchemaSync();
    QMetaObject::invokeMethod(
        context, [callback, result = std::move(result)]() mutable { callback(std::move(result)); },
        Qt::QueuedConnection);
  }).detach();
}

void MiradClient::GetConfigAsync(QObject* context, std::function<void(ConfigResult)> callback) {
  std::thread([context, callback = std::move(callback)]() {
    ConfigResult result = GetConfigSync();
    QMetaObject::invokeMethod(
        context, [callback, result = std::move(result)]() mutable { callback(std::move(result)); },
        Qt::QueuedConnection);
  }).detach();
}

void MiradClient::PatchConfigAsync(QObject* context, const std::vector<ConfigEdit>& edits,
                                   std::function<void(PatchConfigResult)> callback) {
  std::thread([context, edits, callback = std::move(callback)]() {
    PatchConfigResult result = PatchConfigSync(edits);
    QMetaObject::invokeMethod(
        context, [callback, result = std::move(result)]() mutable { callback(std::move(result)); },
        Qt::QueuedConnection);
  }).detach();
}

void MiradClient::ResetConfigKeyAsync(QObject* context, const std::string& key,
                                      std::function<void(PatchConfigResult)> callback) {
  std::thread([context, key, callback = std::move(callback)]() {
    PatchConfigResult result = ResetConfigKeySync(key);
    QMetaObject::invokeMethod(
        context, [callback, result = std::move(result)]() mutable { callback(std::move(result)); },
        Qt::QueuedConnection);
  }).detach();
}

void MiradClient::ListRunnersAsync(QObject* context, std::function<void(RunnersResult)> callback) {
  std::thread([context, callback = std::move(callback)]() {
    RunnersResult result = GetRunnersSync();
    QMetaObject::invokeMethod(
        context, [callback, result = std::move(result)]() mutable { callback(std::move(result)); },
        Qt::QueuedConnection);
  }).detach();
}

void MiradClient::GetGameConfigAsync(QObject* context, const std::string& id,
                                     std::function<void(GameConfigResult)> callback) {
  std::thread([context, id, callback = std::move(callback)]() {
    GameConfigResult result = GetGameConfigSync(id);
    QMetaObject::invokeMethod(
        context, [callback, result = std::move(result)]() mutable { callback(std::move(result)); },
        Qt::QueuedConnection);
  }).detach();
}

void MiradClient::PatchGameConfigAsync(QObject* context, const std::string& id,
                                       const std::vector<GameConfigEdit>& edits,
                                       std::function<void(PatchGameConfigResult)> callback) {
  std::thread([context, id, edits, callback = std::move(callback)]() {
    PatchGameConfigResult result = PatchGameConfigSync(id, edits);
    QMetaObject::invokeMethod(
        context, [callback, result = std::move(result)]() mutable { callback(std::move(result)); },
        Qt::QueuedConnection);
  }).detach();
}

void EventStream::Start(QObject* context, std::function<void(std::string, std::string)> on_event) {
  std::thread([context, on_event = std::move(on_event)]() {
    std::string last_event_id;

    while (true) {
      httplib::Client client(MiradClient::ResolveSocketPath(), 80);
      client.set_address_family(AF_UNIX);
      client.set_connection_timeout(std::chrono::seconds(2));
      client.set_read_timeout(std::chrono::hours(24 * 365));  // events can be arbitrarily far apart

      httplib::Headers headers;
      if (!last_event_id.empty()) headers = {{"Last-Event-ID", last_event_id}};

      std::string buffer;
      client.Get(
          "/v1/events", headers,
          [](const httplib::Response& response) { return response.status == 200; },
          [&](const char* data, size_t length) {
            buffer.append(data, length);
            size_t block_end;
            while ((block_end = buffer.find("\n\n")) != std::string::npos) {
              const std::string block = buffer.substr(0, block_end);
              buffer.erase(0, block_end + 2);

              std::string type;
              std::string event_data;
              size_t line_start = 0;
              while (line_start < block.size()) {
                size_t line_end = block.find('\n', line_start);
                if (line_end == std::string::npos) line_end = block.size();
                const std::string line = block.substr(line_start, line_end - line_start);
                if (line.rfind("event: ", 0) == 0) {
                  type = line.substr(7);
                } else if (line.rfind("data: ", 0) == 0) {
                  event_data = line.substr(6);
                } else if (line.rfind("id: ", 0) == 0) {
                  last_event_id = line.substr(4);
                }
                line_start = line_end + 1;
              }

              if (!type.empty()) {
                QMetaObject::invokeMethod(
                    context, [on_event, type, event_data] { on_event(type, event_data); },
                    Qt::QueuedConnection);
              }
            }
            return true;
          });

      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
  }).detach();
}

}  // namespace mira_gui
