#include "Transport.h"

#include <httplib.h>

#include <cstdlib>
#include <filesystem>

#include "JsonMapping.h"

namespace mira_gui::transport {
namespace {

using nlohmann::json;

httplib::Client MakeClient(const Options& options) {
  httplib::Client client(SocketPath(), 80);
  client.set_address_family(AF_UNIX);
  client.set_connection_timeout(std::chrono::seconds(2));
  if (options.read_timeout) client.set_read_timeout(*options.read_timeout);
  return client;
}

ApiError Unreachable(const httplib::Result& res) {
  return ApiError("cannot reach mirad at " + SocketPath() + " (" + httplib::to_string(res.error()) + ")",
                  ApiError::kUnreachable);
}

// mirad's {"error": {...}} envelope, or the raw body if it isn't one.
ApiError FromBody(const std::string& body_text) {
  const json body = json::parse(body_text, nullptr, false);
  if (body.is_discarded() || !body.contains("error") || !body["error"].is_object()) return ApiError(body_text);
  ApiError error = mapping::ToApiError(body["error"]);
  if (error.message.empty()) error.message = body_text;
  return error;
}

// An httplib::Result carries its own error when the request never got a
// response at all; when it did, mirad's error body is the envelope above.
Reply Finish(const httplib::Result& res) {
  Reply reply;
  if (res) reply.status = res->status;
  if (res && res->status >= 200 && res->status < 300) {
    reply.ok = true;
    reply.body = json::parse(res->body, nullptr, false);
    if (reply.body.is_discarded()) reply.body = json();
    return reply;
  }

  reply.error = res ? FromBody(res->body) : Unreachable(res);
  return reply;
}

}  // namespace

std::string SocketPath() {
  const char* override_path = std::getenv("MIRA_SOCKET");
  if (override_path && *override_path) return override_path;

  const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
  const std::filesystem::path base = runtime_dir && *runtime_dir ? runtime_dir : "/tmp";
  return (base / "mira" / "mirad.sock").string();
}

Reply Get(const std::string& path, const Options& options) {
  return Finish(MakeClient(options).Get(path));
}

Blob GetBinary(const std::string& path, const Options& options) {
  const httplib::Result res = MakeClient(options).Get(path);
  Blob blob;
  if (!res) {
    blob.error = Unreachable(res);
    return blob;
  }

  blob.status = res->status;
  if (res->status < 200 || res->status >= 300) {
    blob.error = FromBody(res->body);
    return blob;
  }

  blob.ok = true;
  blob.bytes = res->body;
  blob.content_type = res->get_header_value("Content-Type");
  return blob;
}

Reply Post(const std::string& path, const Options& options) {
  return Finish(MakeClient(options).Post(path));
}

Reply PostJson(const std::string& path, const json& body, const Options& options) {
  return Finish(MakeClient(options).Post(path, body.dump(), "application/json"));
}

Reply Patch(const std::string& path, const json& body, const Options& options) {
  return Finish(MakeClient(options).Patch(path, body.dump(), "application/json"));
}

Reply Delete(const std::string& path, const Options& options) {
  return Finish(MakeClient(options).Delete(path));
}

std::string UnexpectedResponse(const std::string& endpoint) {
  return "mirad returned an unexpected response for " + endpoint;
}

}  // namespace mira_gui::transport
