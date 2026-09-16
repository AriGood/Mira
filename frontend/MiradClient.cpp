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
  const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
  const std::filesystem::path base = runtime_dir && *runtime_dir ? runtime_dir : "/tmp";
  return (base / "mira" / "mirad.sock").string();
}

namespace {

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
  if (res) {
    const json body = json::parse(res->body, nullptr, false);
    status.detail = body.is_discarded()
                         ? res->body
                         : body.value("error", json::object()).value("message", res->body);
  } else {
    status.detail = "cannot reach mirad at " + socket_path + " (" +
                     httplib::to_string(res.error()) + ") — is it running?";
  }
  return status;
}

GamesResult GetGamesSync() {
  GamesResult result;
  const std::string socket_path = MiradClient::ResolveSocketPath();

  httplib::Client client(socket_path, 80);
  client.set_address_family(AF_UNIX);
  client.set_connection_timeout(std::chrono::seconds(2));

  auto res = client.Get("/v1/games");
  if (!res || res->status < 200 || res->status >= 300) {
    result.ok = false;
    if (res) {
      const json body = json::parse(res->body, nullptr, false);
      result.error = body.is_discarded()
                          ? res->body
                          : body.value("error", json::object()).value("message", res->body);
    } else {
      result.error = "cannot reach mirad at " + socket_path + " (" +
                      httplib::to_string(res.error()) + ") — is it running?";
    }
    return result;
  }

  const json body = json::parse(res->body, nullptr, false);
  if (body.is_discarded() || !body.is_array()) {
    result.ok = false;
    result.error = "mirad returned an unexpected response for GET /v1/games";
    return result;
  }

  result.ok = true;
  for (const json& entry : body) {
    GameSummary game;
    game.id = entry.value("id", std::string());
    game.name = entry.value("name", std::string());
    game.status = entry.value("status", std::string());
    game.platform = entry.value("platform", std::string());
    game.reviewed = entry.value("reviewed", false);
    game.confidence = entry.value("confidence", 0.0);
    result.games.push_back(std::move(game));
  }
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
  if (res) {
    const json body = json::parse(res->body, nullptr, false);
    result.error = body.is_discarded()
                        ? res->body
                        : body.value("error", json::object()).value("message", res->body);
  } else {
    result.error = "cannot reach mirad at " + socket_path + " (" +
                    httplib::to_string(res.error()) + ") — is it running?";
  }
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
    if (res) {
      const json body = json::parse(res->body, nullptr, false);
      result.error = body.is_discarded()
                          ? res->body
                          : body.value("error", json::object()).value("message", res->body);
    } else {
      result.error = "cannot reach mirad at " + socket_path + " (" +
                      httplib::to_string(res.error()) + ") — is it running?";
    }
    return result;
  }

  const json body = json::parse(res->body, nullptr, false);
  result.ok = true;
  result.added = body.value("added", 0);
  result.missing = body.value("missing", 0);
  result.restored = body.value("restored", 0);
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

void MiradClient::ListGamesAsync(QObject* context, std::function<void(GamesResult)> callback) {
  std::thread([context, callback = std::move(callback)]() {
    GamesResult result = GetGamesSync();
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

void MiradClient::ScanLibraryAsync(QObject* context, std::function<void(ScanResult)> callback) {
  std::thread([context, callback = std::move(callback)]() {
    ScanResult result = ScanLibrarySync();
    QMetaObject::invokeMethod(
        context, [callback, result = std::move(result)]() mutable { callback(std::move(result)); },
        Qt::QueuedConnection);
  }).detach();
}

}  // namespace mira_gui
