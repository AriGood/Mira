#include "Transport.h"

#include <httplib.h>

#include <cstdlib>
#include <filesystem>

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

// An httplib::Result carries its own error when the request never got a
// response at all (socket unreachable); when it did, mirad's error body is
// the {"error": {"code", "message"}} envelope every failure uses
// (docs/api.md), and the message inside it is the useful half.
Reply Finish(const httplib::Result& res) {
  Reply reply;
  if (res && res->status >= 200 && res->status < 300) {
    reply.ok = true;
    reply.body = json::parse(res->body, nullptr, false);
    if (reply.body.is_discarded()) reply.body = json();
    return reply;
  }

  if (res) {
    const json body = json::parse(res->body, nullptr, false);
    reply.error = body.is_discarded()
                      ? res->body
                      : body.value("error", json::object()).value("message", res->body);
  } else {
    reply.error = "cannot reach mirad at " + SocketPath() + " (" +
                  httplib::to_string(res.error()) + ") — is it running?";
  }
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

Reply Post(const std::string& path, const Options& options) {
  return Finish(MakeClient(options).Post(path));
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
