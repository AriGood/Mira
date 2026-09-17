#pragma once

#include <json.hpp>

#include <chrono>
#include <optional>
#include <string>

// One request/response round trip against mirad's Unix socket.
//
// Every endpoint in MiradClient used to open its own httplib::Client, repeat
// the same address family and timeouts, then repeat the same "2xx, or else
// unwrap the {"error": {...}} envelope" check. That boilerplate was the only
// thing those fifteen functions had in common, and also the only thing that
// ever had to change in all of them at once — the socket override, a timeout,
// the shape of an error body. So it lives here exactly once, and each
// endpoint is left with only the part that is actually about that endpoint.
namespace mira_gui::transport {

struct Reply {
  bool ok = false;
  std::string error;    // set when ok is false; already human-readable
  nlohmann::json body;  // parsed response body, null if empty or unparsable
};

struct Options {
  // Left unset, httplib's own default applies. Set it for an endpoint that
  // does real work while the request is open — POST /v1/library/scan walks
  // every library root synchronously, since there is no job queue yet
  // (docs/architecture.md).
  std::optional<std::chrono::seconds> read_timeout;
};

// $MIRA_SOCKET if set — the same override `mirad --socket` accepts on the
// daemon side, so `MIRA_SOCKET=/path/to.sock mirad --socket /path/to.sock`
// and `MIRA_SOCKET=/path/to.sock mira-gui` unambiguously talk to each other
// regardless of what else is running. Without it, mirrors core/Paths.h's
// DefaultSocket(): $XDG_RUNTIME_DIR/mira/mirad.sock, falling back to
// /tmp/mira if XDG_RUNTIME_DIR is unset. Does not yet honor a socket_path
// override from settings.toml — that would need a GET /v1/config round trip
// through a socket that hasn't been resolved yet.
std::string SocketPath();

Reply Get(const std::string& path, const Options& options = {});
Reply Post(const std::string& path, const Options& options = {});
Reply PostJson(const std::string& path, const nlohmann::json& body, const Options& options = {});
Reply Patch(const std::string& path, const nlohmann::json& body, const Options& options = {});
Reply Delete(const std::string& path, const Options& options = {});

// The message for a reply that arrived intact but isn't the JSON kind the
// endpoint promises — a protocol mismatch rather than a transport failure,
// so it names the endpoint instead of the socket.
std::string UnexpectedResponse(const std::string& endpoint);

}  // namespace mira_gui::transport
