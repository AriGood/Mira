#include "itch/Butlerd.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Log.h"
#include "itch/Itch.h"

namespace mira::itch {
namespace {
namespace fs = std::filesystem;
using nlohmann::json;

struct Connection {
  std::mutex mutex;
  bool connected = false;
  int socket_fd = -1;
  int next_id = 1;
};

Connection& GlobalConnection() {
  static Connection connection;
  return connection;
}

// Forks `argv`, redirecting only the child's stdout to a pipe (stderr goes
// to mirad's own, same posture as runner::SpawnDetached) -- the caller
// gets that pipe's read end and owns closing it. The child is left
// running; nothing here waits on it. Not built on runner::SpawnDetached*,
// which don't offer a way to read a long-lived child's stdout while it
// keeps running -- this is the one place in this codebase that needs
// that shape (see Butlerd.h's class comment).
Result<int> SpawnCapturingStdout(const std::vector<std::string>& argv) {
  int pipe_fds[2];
  if (pipe(pipe_fds) != 0) return Err("pipe_failed", std::strerror(errno));

  const pid_t pid = fork();
  if (pid < 0) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return Err("fork_failed", std::strerror(errno));
  }
  if (pid == 0) {
    close(pipe_fds[0]);
    dup2(pipe_fds[1], STDOUT_FILENO);
    close(pipe_fds[1]);
    setsid();
    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const std::string& arg : argv) args.push_back(const_cast<char*>(arg.c_str()));
    args.push_back(nullptr);
    execvp(args[0], args.data());
    _exit(127);
  }
  close(pipe_fds[1]);
  return pipe_fds[0];
}

// Reads from `fd` until a full line is available or `deadline` passes.
// Simple blocking-with-timeout read, one byte at a time -- butlerd's
// startup output is a handful of short lines, not a hot path.
Result<std::string> ReadLine(int fd, std::chrono::steady_clock::time_point deadline) {
  std::string line;
  char c = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const ssize_t n = read(fd, &c, 1);
    if (n < 0) {
      if (errno == EINTR) continue;
      return Err("read_failed", std::strerror(errno));
    }
    if (n == 0) return Err("eof", "connection closed before a full line arrived");
    if (c == '\n') return line;
    line.push_back(c);
  }
  return Err("timeout", "no response within the deadline");
}

Result<void> ConnectSocket(int& out_fd, const std::string& host, int port) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* result = nullptr;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) != 0 || result == nullptr) {
    return Err("resolve_failed", "couldn't resolve butlerd's own listen address " + host);
  }
  const int fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
  if (fd < 0) {
    freeaddrinfo(result);
    return Err("socket_failed", std::strerror(errno));
  }
  if (connect(fd, result->ai_addr, result->ai_addrlen) != 0) {
    freeaddrinfo(result);
    close(fd);
    return Err("connect_failed", std::strerror(errno));
  }
  freeaddrinfo(result);
  out_fd = fd;
  return {};
}

Result<void> SendLine(int fd, const json& message) {
  const std::string line = message.dump() + "\n";
  size_t sent = 0;
  while (sent < line.size()) {
    const ssize_t n = write(fd, line.data() + sent, line.size() - sent);
    if (n < 0) {
      if (errno == EINTR) continue;
      return Err("write_failed", std::strerror(errno));
    }
    sent += static_cast<size_t>(n);
  }
  return {};
}

// Reads lines off the connection until one carries the response to
// `request_id` (a "result"/"error" field alongside a matching "id") --
// any notification (a "method" with no "id", e.g. install progress) seen
// along the way is skipped, not surfaced here.
Result<json> ReadResponse(int fd, int request_id, std::chrono::steady_clock::time_point deadline) {
  while (true) {
    const Result<std::string> line = ReadLine(fd, deadline);
    if (!line) return std::unexpected(line.error());
    const json parsed = json::parse(*line, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) continue;
    if (parsed.contains("id") && parsed["id"].is_number_integer() &&
        parsed["id"].get<int>() == request_id) {
      if (parsed.contains("error")) {
        return Err("butlerd_error", parsed["error"].value("message", std::string("butlerd call failed")));
      }
      return parsed.value("result", json::object());
    }
    // A notification for some other in-flight call, or one we don't act
    // on -- move on.
  }
}

// Starts butler (if not already connected) and performs Meta.Authenticate.
// Assumes `connection.mutex` is already held.
Result<void> EnsureConnectedLocked(const config::Config& config, Connection& connection) {
  if (connection.connected) return {};

  const ItchStatus status = DetectButler(config);
  if (!status.installed) {
    return Err("butler_missing", "butler isn't installed — run \"mira itch setup\" to download it");
  }

  const fs::path db_path = config.File().parent_path() / "tools" / "itch" / "butler.db";
  std::error_code ec;
  fs::create_directories(db_path.parent_path(), ec);

  const Result<int> stdout_fd =
    SpawnCapturingStdout({status.path, "daemon", "--json", "--dbpath", db_path.string()});
  if (!stdout_fd) return std::unexpected(stdout_fd.error());

  // butlerd mixes its own JSON log lines in with the one
  // butlerd/listen-notification line we actually need (confirmed live) --
  // read until we see it or run out of time.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  std::string secret;
  std::string address;
  while (secret.empty()) {
    const Result<std::string> line = ReadLine(*stdout_fd, deadline);
    if (!line) {
      close(*stdout_fd);
      return Err("butlerd_start_failed", "butlerd didn't print its listen address in time: " + line.error().message);
    }
    const json parsed = json::parse(*line, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) continue;
    if (parsed.value("type", std::string()) != "butlerd/listen-notification") continue;
    secret = parsed.value("secret", std::string());
    address = parsed.value("tcp", json::object()).value("address", std::string());
  }

  // butlerd keeps logging to its stdout for as long as it runs -- drain it
  // in the background forever so the child never blocks writing to a full
  // pipe once we stop reading it ourselves.
  const int drain_fd = *stdout_fd;
  std::thread([drain_fd] {
    char buffer[4096];
    while (read(drain_fd, buffer, sizeof(buffer)) > 0) {
    }
    close(drain_fd);
  }).detach();

  const size_t colon = address.rfind(':');
  if (colon == std::string::npos) return Err("butlerd_start_failed", "unexpected listen address: " + address);
  const std::string host = address.substr(0, colon);
  const int port = std::atoi(address.substr(colon + 1).c_str());

  int socket_fd = -1;
  if (auto connected = ConnectSocket(socket_fd, host, port); !connected) return std::unexpected(connected.error());

  const int auth_id = connection.next_id++;
  if (auto sent = SendLine(socket_fd, {{"jsonrpc", "2.0"}, {"id", auth_id}, {"method", "Meta.Authenticate"},
                                       {"params", {{"secret", secret}}}});
      !sent) {
    close(socket_fd);
    return std::unexpected(sent.error());
  }
  const Result<json> auth_result = ReadResponse(socket_fd, auth_id, deadline);
  if (!auth_result) {
    close(socket_fd);
    return std::unexpected(auth_result.error());
  }

  connection.socket_fd = socket_fd;
  connection.connected = true;
  log::Info("connected to butlerd at {}", address);
  return {};
}

}  // namespace

Result<json> Call(const config::Config& config, const std::string& method, const json& params) {
  Connection& connection = GlobalConnection();
  std::lock_guard<std::mutex> lock(connection.mutex);

  if (auto ready = EnsureConnectedLocked(config, connection); !ready) return std::unexpected(ready.error());

  const int id = connection.next_id++;
  if (auto sent = SendLine(connection.socket_fd,
                          {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});
      !sent) {
    connection.connected = false;
    return std::unexpected(sent.error());
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
  Result<json> response = ReadResponse(connection.socket_fd, id, deadline);
  if (!response && response.error().code != "butlerd_error") {
    // Transport-level failure (read/timeout/eof) -- the connection itself
    // is no longer trustworthy, next call starts fresh.
    connection.connected = false;
  }
  return response;
}

}  // namespace mira::itch
