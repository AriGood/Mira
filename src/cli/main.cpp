// mira — command-line client for mirad.
//
// A pure REST client over the same Unix socket the frontend uses: no direct
// access to settings.toml/games.toml, no special privilege. That is
// deliberate (see docs/architecture.md) — every gap this CLI hits is a gap
// the frontend would hit too, so it surfaces here first, while the API is
// still cheap to change.

#include <httplib.h>
#include <json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <format>
#include <iostream>
#include <string>
#include <vector>
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

int CmdRunnersList() {
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

int CmdRunnersCatalog(int argc, char** argv) {
  std::string kind = "proton";
  for (int i = 0; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--kind" && i + 1 < argc) kind = argv[++i];
  }
  auto client = Connect();
  auto res = client.Get(std::format("/v1/runners/catalog?kind={}", kind));
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  json releases = json::parse(res->body);
  if (releases.empty()) {
    std::puts("(no releases found — check runner_sources.*.repo / .asset_pattern)");
    return 0;
  }
  for (const json& r : releases) {
    std::printf("%-24s %-40s %8.1f MB%s\n", r.value("tag", "").c_str(), r.value("asset_name", "").c_str(),
               r.value("size_bytes", 0LL) / 1024.0 / 1024.0,
               r.value("has_checksum", false) ? "" : "  (no checksum)");
  }
  return 0;
}

int CmdRunnersDownload(int argc, char** argv) {
  std::string kind, tag;
  for (int i = 0; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--kind" && i + 1 < argc) kind = argv[++i];
    else if (arg == "--tag" && i + 1 < argc) tag = argv[++i];
  }
  if (kind.empty() || tag.empty()) {
    std::fprintf(stderr, "usage: mira runners download --kind proton|wine --tag TAG\n");
    return 2;
  }
  auto client = Connect();
  const json body = {{"kind", kind}, {"tag", tag}};
  auto res = client.Post("/v1/runners/download", body.dump(), "application/json");
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  std::printf("downloading %s %s — watch `mira watch` for runners.download.finished/failed\n",
             kind.c_str(), tag.c_str());
  return 0;
}

int CmdRunnersSchema(int argc, char** argv) {
  if (argc < 1) {
    std::fprintf(stderr, "usage: mira runners schema <kind>\n");
    return 2;
  }
  auto client = Connect();
  auto res = client.Get(std::format("/v1/runners/{}/schema", argv[0]));
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  std::cout << json::parse(res->body).dump(2) << '\n';
  return 0;
}

int CmdRunnersRemove(int argc, char** argv) {
  if (argc < 1) {
    std::fprintf(stderr,
                 "usage: mira runners remove <kind:name>\n"
                 "  uninstalls a build fetched via `runners download` (e.g. proton:GE-Proton11-7).\n"
                 "  Refuses anything not really inside runner_search_paths/wine_search_paths --\n"
                 "  the system wine, or a hand-edited path, can't be removed this way.\n");
    return 2;
  }
  auto client = Connect();
  auto res = client.Delete(std::format("/v1/runners/{}", argv[0]));
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  std::puts("removed");
  return 0;
}

int CmdRunners(int argc, char** argv) {
  if (argc == 0) return CmdRunnersList();
  const std::string_view sub = argv[0];
  if (sub == "catalog") return CmdRunnersCatalog(argc - 1, argv + 1);
  if (sub == "download") return CmdRunnersDownload(argc - 1, argv + 1);
  if (sub == "schema") return CmdRunnersSchema(argc - 1, argv + 1);
  if (sub == "remove") return CmdRunnersRemove(argc - 1, argv + 1);
  std::fprintf(stderr,
              "usage: mira runners [catalog [--kind K] | download --kind K --tag TAG |\n"
              "                     schema <kind> | remove <kind:name>]\n");
  return 2;
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

int CmdRun(int argc, char** argv) {
  std::string id, exe_path, args;
  for (int i = 0; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--exe" && i + 1 < argc) exe_path = argv[++i];
    else if (arg == "--args" && i + 1 < argc) args = argv[++i];
    else if (id.empty()) id = arg;
  }
  if (id.empty() || exe_path.empty()) {
    std::fprintf(stderr,
                 "usage: mira run <id> --exe PATH [--args ARGS]\n"
                 "  runs an arbitrary exe inside this game's own prefix (tracked like a normal\n"
                 "  launch) — provisions one first if it doesn't have one yet, which is how a\n"
                 "  needs_install game's installer actually gets run.\n");
    return 2;
  }
  auto client = Connect();
  json body = {{"exe_path", exe_path}};
  if (!args.empty()) body["args"] = args;
  auto res = client.Post(std::format("/v1/games/{}/run", id), body.dump(), "application/json");
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  std::puts("running");
  return 0;
}

int CmdAdd(int argc, char** argv) {
  std::string install_path, exe_path, name, platform;
  bool is_installer = false;
  std::vector<std::string> positional;
  for (int i = 0; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--name" && i + 1 < argc) name = argv[++i];
    else if (arg == "--platform" && i + 1 < argc) platform = argv[++i];
    else if (arg == "--installer") is_installer = true;
    else positional.push_back(std::string(arg));
  }
  if (positional.size() < 2) {
    std::fprintf(stderr,
                 "usage: mira add <install_path> <exe_path> [--name N] "
                 "[--platform windows|native] [--installer]\n"
                 "  creates a game record for a path outside anywhere Mira already scans --\n"
                 "  point it at an installer with --installer, run it with `mira run`, then\n"
                 "  `mira finish-install` once it's actually installed.\n");
    return 2;
  }
  json body = {{"install_path", positional[0]}, {"exe_path", positional[1]}};
  if (!name.empty()) body["name"] = name;
  if (!platform.empty()) body["platform"] = platform;
  if (is_installer) body["is_installer"] = true;

  auto client = Connect();
  auto res = client.Post("/v1/games/manual", body.dump(), "application/json");
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  json game = json::parse(res->body);
  std::printf("%s: %s\n", game.value("id", "").c_str(), game.value("status", "").c_str());
  return 0;
}

int CmdFinishInstall(int argc, char** argv) {
  if (argc < 1) {
    std::fprintf(stderr,
                 "usage: mira finish-install <id>\n"
                 "  after running an installer with `mira run` and pointing --exe at whatever\n"
                 "  it actually installed via `mira set <id> --exe ...`, this marks the game\n"
                 "  ready to launch normally.\n");
    return 2;
  }
  auto client = Connect();
  auto res = client.Post(std::format("/v1/games/{}/finish-install", argv[0]));
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  std::puts("ready");
  return 0;
}

int CmdRemove(int argc, char** argv) {
  if (argc < 1) {
    std::fprintf(stderr,
                 "usage: mira remove <id> [--delete-files] [--delete-prefix] [--delete-metadata] [--purge]\n"
                 "  forgets the game; nothing on disk is touched unless you ask for it\n"
                 "  explicitly, and only paths that are really inside a configured root.\n"
                 "  --delete-files     removes the game's own install folder\n"
                 "  --delete-prefix    removes its Wine/Proton prefix — use --delete-files\n"
                 "                     alone to remove the game but leave the prefix in place\n"
                 "  --delete-metadata  removes cached cover art / store info\n"
                 "  --purge            shorthand for all three of the above\n");
    return 2;
  }
  const std::string id = argv[0];
  bool delete_files = false, delete_prefix = false, delete_metadata = false, purge = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--delete-files") delete_files = true;
    else if (arg == "--delete-prefix") delete_prefix = true;
    else if (arg == "--delete-metadata") delete_metadata = true;
    else if (arg == "--purge") purge = true;
  }
  auto client = Connect();
  std::string path = std::format("/v1/games/{}", id);
  if (delete_files || delete_prefix || delete_metadata || purge) {
    path += "?";
    if (delete_files) path += "delete_files=true&";
    if (delete_prefix) path += "delete_prefix=true&";
    if (delete_metadata) path += "delete_metadata=true&";
    if (purge) path += "purge=true&";
    path.pop_back();  // trailing '&' or '?'
  }
  auto res = client.Delete(path);
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  std::puts("removed");
  return 0;
}

int CmdSteamScan() {
  auto client = Connect();
  auto res = client.Post("/v1/steam/scan");
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  json summary = json::parse(res->body);
  std::printf("added: %lld  updated: %lld\n", summary.value("added", 0LL), summary.value("updated", 0LL));
  return 0;
}

int CmdSteam(int argc, char** argv) {
  if (argc > 0 && std::string_view(argv[0]) == "scan") return CmdSteamScan();
  std::fprintf(stderr, "usage: mira steam scan\n");
  return 2;
}

int CmdLutrisImport() {
  auto client = Connect();
  auto res = client.Post("/v1/lutris/import");
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  json summary = json::parse(res->body);
  std::printf("added: %lld  updated: %lld  skipped: %lld\n", summary.value("added", 0LL),
             summary.value("updated", 0LL), summary.value("skipped", 0LL));
  return 0;
}

int CmdLutris(int argc, char** argv) {
  if (argc > 0 && std::string_view(argv[0]) == "import") return CmdLutrisImport();
  std::fprintf(stderr, "usage: mira lutris import\n");
  return 2;
}

int CmdEpicSetup() {
  auto client = Connect();
  auto res = client.Post("/v1/epic/legendary/install");
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  json body = json::parse(res->body);
  std::printf("downloading legendary %s — watch `mira watch` for epic.legendary.install.finished\n",
             body.value("tag", std::string()).c_str());
  return 0;
}

int CmdEpic(int argc, char** argv) {
  if (argc > 0 && std::string_view(argv[0]) == "setup") return CmdEpicSetup();
  std::fprintf(stderr, "usage: mira epic setup\n");
  return 2;
}

int CmdDesktopEntriesList() {
  auto client = Connect();
  auto res = client.Get("/v1/desktop-entries/candidates");
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  json candidates = json::parse(res->body);
  if (candidates.empty()) {
    std::puts("(no candidates — every already-installed .desktop entry is either already a game, "
              "Mira's own, or covered by another importer)");
    return 0;
  }
  for (const json& c : candidates) {
    std::printf("%-40s %s\n", c.value("id", "").c_str(), c.value("name", "").c_str());
  }
  return 0;
}

int CmdDesktopEntriesImport(int argc, char** argv) {
  if (argc < 1) {
    std::fprintf(stderr, "usage: mira desktop-entries import <id> [<id>...]\n");
    return 2;
  }
  json ids = json::array();
  for (int i = 0; i < argc; ++i) ids.push_back(argv[i]);

  auto client = Connect();
  auto res = client.Post("/v1/desktop-entries/import", json{{"ids", ids}}.dump(), "application/json");
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  json summary = json::parse(res->body);
  std::printf("added: %lld  updated: %lld\n", summary.value("added", 0LL), summary.value("updated", 0LL));
  return 0;
}

int CmdDesktopEntries(int argc, char** argv) {
  if (argc > 0 && std::string_view(argv[0]) == "list") return CmdDesktopEntriesList();
  if (argc > 0 && std::string_view(argv[0]) == "import") return CmdDesktopEntriesImport(argc - 1, argv + 1);
  std::fprintf(stderr, "usage: mira desktop-entries [list | import <id> [<id>...]]\n");
  return 2;
}

int CmdGameModeStatus() {
  auto client = Connect();
  auto res = client.Get("/v1/gamemode/status");
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  json status = json::parse(res->body);
  std::printf("installed: %s  daemon running: %s\n", status.value("installed", false) ? "yes" : "no",
             status.value("daemon_running", false) ? "yes" : "no");
  return 0;
}

int CmdGameMode(int argc, char** argv) {
  if (argc > 0 && std::string_view(argv[0]) == "status") return CmdGameModeStatus();
  std::fprintf(stderr, "usage: mira gamemode status\n");
  return 2;
}

int CmdTricks(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: mira tricks <id> <verb>\n"
                 "  runs a winetricks verb (e.g. corefonts, vcrun2019, win10) against this\n"
                 "  game's own prefix. Runs in the background; watch `mira watch` for\n"
                 "  tricks.finished/.failed.\n");
    return 2;
  }
  const std::string id = argv[0];
  const std::string verb = argv[1];

  auto client = Connect();
  const json body = {{"verb", verb}};
  auto res = client.Post(std::format("/v1/games/{}/tricks", id), body.dump(), "application/json");
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  std::puts("running — watch `mira watch` for tricks.finished/.failed");
  return 0;
}

int CmdMetadata(int argc, char** argv) {
  if (argc < 1) {
    std::fprintf(stderr,
                 "usage: mira metadata <id> [--refresh]\n"
                 "  prints cached cover-art/store-info JSON; --refresh re-fetches first\n"
                 "  (cover art itself is only reachable via GET /v1/games/{id}/artwork,\n"
                 "  not printable here).\n");
    return 2;
  }
  const std::string id = argv[0];
  const bool refresh = argc > 1 && std::string_view(argv[1]) == "--refresh";

  auto client = Connect();
  if (refresh) {
    auto posted = client.Post(std::format("/v1/games/{}/metadata/refresh", id));
    if (!Ok(posted)) {
      PrintError(posted);
      return 1;
    }
    std::puts("fetching — watch `mira watch` for game.metadata_ready/.metadata_failed");
    return 0;
  }

  auto res = client.Get(std::format("/v1/games/{}/metadata", id));
  if (!Ok(res)) {
    PrintError(res);
    return 1;
  }
  std::cout << json::parse(res->body).dump(2) << '\n';
  return 0;
}

int CmdList(int argc, char** argv) {
  std::string status_filter, tag_filter;
  for (int i = 0; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--status" && i + 1 < argc) status_filter = argv[++i];
    else if (arg == "--tag" && i + 1 < argc) tag_filter = argv[++i];
  }
  auto client = Connect();
  std::string path = "/v1/games";
  std::string query;
  if (!status_filter.empty()) query += "status=" + status_filter + "&";
  if (!tag_filter.empty()) query += "tag=" + tag_filter + "&";
  if (!query.empty()) {
    query.pop_back();  // trailing '&'
    path += "?" + query;
  }
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
                 "                     [--tag NAME]... [--untag NAME]...\n"
                 "                     [--override dotted.key=VALUE]... [--unset dotted.key]...\n"
                 "  --tag hidden leaves this game out of `mira list`/GET /v1/games by\n"
                 "  default (still reachable via --tag hidden or `mira show`).\n");
    return 2;
  }
  const std::string id = argv[0];
  json patch = json::object();     // -> PATCH /v1/games/{id}: this game's own fields
  json overrides = json::object(); // -> PATCH /v1/games/{id}/config: overrides of global settings
  json env = json::object();
  std::vector<std::string> add_tags, remove_tags;

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
    } else if (arg == "--tag") {
      add_tags.push_back(next());
    } else if (arg == "--untag") {
      remove_tags.push_back(next());
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

  // --tag/--untag add or remove from whatever this game's tags already are
  // -- PATCH itself replaces the array wholesale (see ParseGamePatch), so
  // the current set has to be fetched first to edit it rather than blow it
  // away.
  if (!add_tags.empty() || !remove_tags.empty()) {
    auto current = client.Get(std::format("/v1/games/{}", id));
    if (!Ok(current)) {
      PrintError(current);
      return 1;
    }
    json tags = json::parse(current->body).value("tags", json::array());
    std::vector<std::string> merged;
    for (const auto& t : tags) merged.push_back(t.get<std::string>());
    for (const std::string& t : remove_tags) std::erase(merged, t);
    for (const std::string& t : add_tags) {
      if (std::ranges::find(merged, t) == merged.end()) merged.push_back(t);
    }
    patch["tags"] = merged;
  }

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
      "  runners catalog [--kind proton|wine]     list downloadable versions\n"
      "  runners download --kind K --tag TAG      download and install one\n"
      "  launch <id>            launch a game (or fire steam://rungameid for a\n"
      "                         Steam game, depending on steam.launch_mode)\n"
      "  stop <id>              stop a running game\n"
      "  run <id> --exe PATH [--args ARGS]        run an arbitrary exe in this\n"
      "                         game's prefix — how you run a needs_install game's\n"
      "                         installer\n"
      "  finish-install <id>    mark a needs_install game ready after installing\n"
      "  list [--status S] [--tag T]        list games (hidden-tagged ones excluded\n"
      "                         by default; --tag hidden lists exactly those)\n"
      "  show <id> [--effective] show one game, or its resolved settings\n"
      "  set <id> [flags...]    correct a game's auto-detected configuration\n"
      "  remove <id> [--delete-files] [--delete-prefix] [--delete-metadata] [--purge]\n"
      "  add <install_path> <exe_path> [--name N] [--platform windows|native] [--installer]\n"
      "  steam scan             detect installed Steam games\n"
      "  lutris import          import games from Lutris's own database\n"
      "  desktop-entries list    list already-installed .desktop entries that could become games\n"
      "  desktop-entries import <id> [<id>...]   add the picked ones (covers Flatpak apps too)\n"
      "  gamemode status         check whether GameMode is installed/running\n"
      "  metadata <id> [--refresh]                cached cover-art/store info\n"
      "  tricks <id> <verb>     run a winetricks verb against this game's prefix\n"
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
  if (command == "runners") return CmdRunners(rest_argc, rest);
  if (command == "launch") return CmdLaunch(rest_argc, rest);
  if (command == "stop") return CmdStop(rest_argc, rest);
  if (command == "run") return CmdRun(rest_argc, rest);
  if (command == "finish-install") return CmdFinishInstall(rest_argc, rest);
  if (command == "remove") return CmdRemove(rest_argc, rest);
  if (command == "add") return CmdAdd(rest_argc, rest);
  if (command == "steam") return CmdSteam(rest_argc, rest);
  if (command == "lutris") return CmdLutris(rest_argc, rest);
  if (command == "epic") return CmdEpic(rest_argc, rest);
  if (command == "desktop-entries") return CmdDesktopEntries(rest_argc, rest);
  if (command == "gamemode") return CmdGameMode(rest_argc, rest);
  if (command == "metadata") return CmdMetadata(rest_argc, rest);
  if (command == "tricks") return CmdTricks(rest_argc, rest);
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
