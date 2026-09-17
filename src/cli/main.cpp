// mira — command-line client for mirad.
//
// A pure REST client over the same Unix socket the frontend uses: no direct
// access to settings.toml/games.toml, no special privilege. That is
// deliberate (see docs/architecture.md) — every gap this CLI hits is a gap
// the frontend would hit too, so it surfaces here first, while the API is
// still cheap to change.

#include <httplib.h>
#include <json.hpp>

#include <chrono>
#include <cstdio>
#include <format>
#include <iostream>
#include <string>
#include <unistd.h>

#include "config/Config.h"
#include "core/Paths.h"

namespace {
using nlohmann::json;

std::filesystem::path ResolveSocketPath() {
  mira::config::Config config(mira::paths::SettingsFile());
  config.Load();
  return mira::paths::Expand(config.GetString("socket_path"));
}

httplib::Client Connect() {
  httplib::Client client(ResolveSocketPath().string(), 80);
  client.set_address_family(AF_UNIX);
  client.set_connection_timeout(std::chrono::seconds(2));
  return client;
}

void PrintError(const httplib::Result& res) {
  if (res) {
    json body = json::parse(res->body, nullptr, false);
    const std::string message =
        body.is_discarded() ? res->body : body.value("error", json::object()).value("message", res->body);
    std::fprintf(stderr, "mira: %s (HTTP %d)\n", message.c_str(), res->status);
  } else {
    std::fprintf(stderr,
                 "mira: cannot reach mirad at %s (%s) — is it running? "
                 "Try `systemctl --user status mirad` or just running `mirad`.\n",
                 ResolveSocketPath().string().c_str(), httplib::to_string(res.error()).c_str());
  }
}

bool Ok(const httplib::Result& res) { return res && res->status >= 200 && res->status < 300; }

int CmdStatus() {
  auto client = Connect();
  auto res = client.Get("/v1/health");
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  std::puts("mirad is running");
  return 0;
}

int CmdScan() {
  auto client = Connect();
  auto res = client.Post("/v1/library/scan");
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  json summary = json::parse(res->body);
  std::printf("added: %lld  missing: %lld  restored: %lld\n",
             summary.value("added", 0LL), summary.value("missing", 0LL),
             summary.value("restored", 0LL));
  return 0;
}

int CmdRunners() {
  auto client = Connect();
  auto res = client.Get("/v1/runners");
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  json builds = json::parse(res->body);
  if (builds.empty()) {
    std::puts("(no runner builds found — check runner_search_paths / wine_search_paths)");
    return 0;
  }
  for (const json& build : builds) {
    std::printf("%-40s %s\n", build.value("reference", "").c_str(), build.value("path", "").c_str());
  }
  return 0;
}

int CmdLaunch(int argc, char** argv) {
  if (argc < 1) {
    std::fprintf(stderr, "usage: mira launch <id>\n");
    return 2;
  }
  auto client = Connect();
  auto res = client.Post(std::format("/v1/games/{}/launch", argv[0]));
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  std::puts("running");
  return 0;
}

int CmdStop(int argc, char** argv) {
  if (argc < 1) {
    std::fprintf(stderr, "usage: mira stop <id>\n");
    return 2;
  }
  auto client = Connect();
  auto res = client.Post(std::format("/v1/games/{}/stop", argv[0]));
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  std::puts("stopping");
  return 0;
}

int CmdList(int argc, char** argv) {
  std::string status_filter;
  for (int i = 0; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--status" && i + 1 < argc) status_filter = argv[++i];
  }
  auto client = Connect();
  std::string path = "/v1/games";
  if (!status_filter.empty()) path += "?status=" + status_filter;
  auto res = client.Get(path);
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  json games = json::parse(res->body);
  if (games.empty()) {
    std::puts("(no games)");
    return 0;
  }
  for (const json& game : games) {
    std::printf("%-24s %-10s %-8s %s%s\n", game.value("id", "").c_str(),
               game.value("status", "").c_str(),
               game.value("reviewed", false) ? "" : "[unreviewed]",
               game.value("name", "").c_str(),
               game.value("platform", "") == "windows" ? "  (windows)" : "");
  }
  return 0;
}

int CmdShow(int argc, char** argv) {
  if (argc < 1) {
    std::fprintf(stderr, "usage: mira show <id> [--effective]\n");
    return 2;
  }
  const std::string id = argv[0];
  const bool effective = argc > 1 && std::string_view(argv[1]) == "--effective";

  auto client = Connect();
  auto res = client.Get(effective ? std::format("/v1/games/{}/config", id)
                                  : std::format("/v1/games/{}", id));
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  std::cout << json::parse(res->body).dump(2) << '\n';
  return 0;
}

int CmdSet(int argc, char** argv) {
  if (argc < 1) {
    std::fprintf(stderr,
                 "usage: mira set <id> [--name N] [--exe PATH] [--args ARGS]\n"
                 "                     [--runner kind:name] [--data-dir PATH]\n"
                 "                     [--env KEY=VALUE]...\n"
                 "                     [--override dotted.key=VALUE]... [--unset dotted.key]...\n");
    return 2;
  }
  const std::string id = argv[0];
  json patch = json::object();     // -> PATCH /v1/games/{id}: this game's own fields
  json overrides = json::object(); // -> PATCH /v1/games/{id}/config: overrides of global settings
  json env = json::object();

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
    if (arg == "--name") {
      patch["name"] = next();
    } else if (arg == "--exe") {
      patch["exe_path"] = next();
    } else if (arg == "--args") {
      patch["args"] = next();
    } else if (arg == "--runner") {
      patch["runner_ref"] = next();
    } else if (arg == "--data-dir") {
      patch["data_dir"] = next();
    } else if (arg == "--env") {
      const std::string kv = next();
      if (const auto eq = kv.find('='); eq != std::string::npos) {
        env[kv.substr(0, eq)] = kv.substr(eq + 1);
      }
    } else if (arg == "--override") {
      const std::string kv = next();
      if (const auto eq = kv.find('='); eq != std::string::npos) {
        json value = json::parse(kv.substr(eq + 1), nullptr, false);
        overrides[kv.substr(0, eq)] = value.is_discarded() ? json(kv.substr(eq + 1)) : value;
      }
    } else if (arg == "--unset") {
      overrides[next()] = nullptr;
    }
  }
  if (!env.empty()) patch["env"] = env;

  auto client = Connect();
  bool changed = false;

  if (!patch.empty()) {
    auto res = client.Patch(std::format("/v1/games/{}", id), patch.dump(), "application/json");
    if (!Ok(res)) {
      PrintError(res);
      return 1;
    }
    changed = true;
  }
  if (!overrides.empty()) {
    auto res =
        client.Patch(std::format("/v1/games/{}/config", id), overrides.dump(), "application/json");
    if (!Ok(res)) {
      PrintError(res);
      return 1;
    }
    changed = true;
  }

  std::puts(changed ? "updated" : "nothing to do");
  return 0;
}

int CmdConfig(int argc, char** argv) {
  if (argc < 1) {
    std::fprintf(stderr, "usage: mira config get|set|list|reset [args...]\n");
    return 2;
  }
  const std::string_view sub = argv[0];
  auto client = Connect();

  if (sub == "list") {
    auto res = client.Get("/v1/config/schema");
    if (!Ok(res)) {
      PrintError(res);
      return 1;
    }
    for (const json& entry : json::parse(res->body)) {
      std::printf("%-32s %-8s %-9s %s\n", entry.value("key", "").c_str(),
                 entry.value("type", "").c_str(), entry.value("tier", "").c_str(),
                 entry.value("doc", "").c_str());
    }
    return 0;
  }

  if (sub == "get") {
    if (argc < 2) {
      std::fprintf(stderr, "usage: mira config get <key>\n");
      return 2;
    }
    auto res = client.Get("/v1/config");
    if (!Ok(res)) {
      PrintError(res);
      return 1;
    }
    json document = json::parse(res->body);
    json::json_pointer pointer;
    std::string flat(argv[1]);
    for (char& c : flat) {
      if (c == '.') c = '/';
    }
    pointer = json::json_pointer("/" + flat);
    if (!document.contains(pointer)) {
      std::fprintf(stderr, "mira: unknown setting \"%s\"\n", argv[1]);
      return 1;
    }
    std::cout << document[pointer].dump() << '\n';
    return 0;
  }

  if (sub == "set") {
    if (argc < 3) {
      std::fprintf(stderr, "usage: mira config set <key> <value>\n");
      return 2;
    }
    json value = json::parse(argv[2], nullptr, false);
    if (value.is_discarded()) value = std::string(argv[2]);  // bare strings need no quoting

    std::string flat(argv[1]);
    json patch = json::object();
    json* cursor = &patch;
    size_t start = 0;
    while (true) {
      const size_t dot = flat.find('.', start);
      const std::string segment = flat.substr(start, dot == std::string::npos ? dot : dot - start);
      if (dot == std::string::npos) {
        (*cursor)[segment] = value;
        break;
      }
      cursor = &(*cursor)[segment];
      start = dot + 1;
    }
    auto res = client.Patch("/v1/config", patch.dump(), "application/json");
    if (!Ok(res)) {
      PrintError(res);
      return 1;
    }
    std::puts("ok");
    return 0;
  }

  if (sub == "reset") {
    const std::string path = argc > 1 ? std::format("/v1/config/reset?key={}", argv[1])
                                       : "/v1/config/reset";
    auto res = client.Post(path);
    if (!Ok(res)) {
      PrintError(res);
      return 1;
    }
    std::puts("ok");
    return 0;
  }

  std::fprintf(stderr, "mira: unknown config subcommand \"%.*s\"\n", static_cast<int>(sub.size()),
              sub.data());
  return 2;
}

int CmdWatch() {
  auto client = Connect();
  client.set_read_timeout(std::chrono::hours(24 * 365));  // events can be arbitrarily far apart
  std::string buffer;
  auto res = client.Get("/v1/events", [&](const char* data, size_t length) {
    buffer.append(data, length);
    size_t pos;
    while ((pos = buffer.find("\n\n")) != std::string::npos) {
      std::cout << buffer.substr(0, pos) << "\n---\n";
      std::cout.flush();
      buffer.erase(0, pos + 2);
    }
    return true;
  });
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  return 0;
}

int CmdDaemon(int argc, char** argv, const char* self) {
  // `mira daemon` is a convenience that execs mirad alongside this binary (or
  // on PATH), forwarding any remaining arguments — it does not talk to the
  // API, since there is nothing listening yet.
  std::filesystem::path candidate = std::filesystem::path(self).parent_path() / "mirad";
  std::vector<char*> exec_argv;
  const std::string exe = std::filesystem::exists(candidate) ? candidate.string() : "mirad";
  exec_argv.push_back(const_cast<char*>(exe.c_str()));
  for (int i = 0; i < argc; ++i) exec_argv.push_back(argv[i]);
  exec_argv.push_back(nullptr);
  execvp(exe.c_str(), exec_argv.data());
  std::perror("mira: failed to start mirad");
  return 1;
}

void PrintUsage() {
  std::puts(
      "usage: mira <command> [args...]\n"
      "\n"
      "commands:\n"
      "  status                 check whether mirad is reachable\n"
      "  daemon [args...]       exec mirad in the foreground\n"
      "  scan                   scan all library roots now\n"
      "  runners                list installed Proton/Wine builds\n"
      "  launch <id>            launch a game\n"
      "  stop <id>              stop a running game\n"
      "  list [--status S]      list games\n"
      "  show <id> [--effective] show one game, or its resolved settings\n"
      "  set <id> [flags...]    correct a game's auto-detected configuration\n"
      "  config get|set|list|reset [args...]\n"
      "  watch                  tail the event stream\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    PrintUsage();
    return 2;
  }
  const std::string_view command = argv[1];
  char** rest = argv + 2;
  const int rest_argc = argc - 2;

  if (command == "--help" || command == "-h" || command == "help") {
    PrintUsage();
    return 0;
  }
  if (command == "status") return CmdStatus();
  if (command == "scan") return CmdScan();
  if (command == "runners") return CmdRunners();
  if (command == "launch") return CmdLaunch(rest_argc, rest);
  if (command == "stop") return CmdStop(rest_argc, rest);
  if (command == "daemon") return CmdDaemon(rest_argc, rest, argv[0]);
  if (command == "list") return CmdList(rest_argc, rest);
  if (command == "show") return CmdShow(rest_argc, rest);
  if (command == "set") return CmdSet(rest_argc, rest);
  if (command == "config") return CmdConfig(rest_argc, rest);
  if (command == "watch") return CmdWatch();

  std::fprintf(stderr, "mira: unknown command \"%.*s\"\n", static_cast<int>(command.size()),
              command.data());
  PrintUsage();
  return 2;
}
