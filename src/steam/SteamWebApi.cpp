#include "steam/SteamWebApi.h"

#include <format>

#include <json.hpp>

#include "core/Command.h"
#include "runner/Exec.h"

namespace mira::steam {
namespace {
using nlohmann::json;

// Same ceiling every other outbound call here uses: this runs off a user
// request, and an unreachable endpoint must not park a thread indefinitely.
constexpr std::string_view kMaxTime = "10";

}  // namespace

Result<std::vector<OwnedGame>> ListOwnedGames(const config::Config& config) {
  const std::string key = config.GetString("steam.web_api_key");
  if (key.empty()) {
    return Err("no_steam_web_api_key",
               "listing everything a Steam account owns needs a Steam Web API key — set "
               "steam.web_api_key (free, from steamcommunity.com/dev/apikey)");
  }
  const std::string steamid = config.GetString("steam.steamid64");
  if (steamid.empty()) {
    return Err("no_steamid64", "set steam.steamid64 to the account's 64-bit Steam ID");
  }

  Command command;
  command.argv = {"curl", "-sSL", "--max-time", std::string(kMaxTime),
                  std::format("https://api.steampowered.com/IPlayerService/GetOwnedGames/v1/"
                             "?key={}&steamid={}&include_appinfo=1&include_played_free_games=1&format=json",
                             key, steamid)};
  const Result<runner::ExecResult> result = runner::RunAndWait(command);
  if (!result) return std::unexpected(result.error());

  const json parsed = json::parse(result->output, nullptr, false);
  // A bad key/id comes back as an empty {"response":{}} rather than an HTTP
  // error, so "no games" and "wrong credentials" look alike from here --
  // reported as the former, since that's also what a legitimately empty
  // account looks like and there's nothing to distinguish them by.
  if (parsed.is_discarded() || !parsed.contains("response")) {
    return Err("steam_web_api_error", "Steam's Web API didn't return a usable response");
  }

  std::vector<OwnedGame> owned;
  const json& response = parsed["response"];
  if (!response.contains("games") || !response["games"].is_array()) return owned;
  for (const json& entry : response["games"]) {
    OwnedGame game;
    if (!entry.contains("appid")) continue;
    game.appid = std::to_string(entry["appid"].get<std::int64_t>());
    game.name = entry.value("name", std::string());
    // Steam reports this in minutes; model::Game::play_seconds is seconds.
    game.play_seconds = entry.value("playtime_forever", std::int64_t{0}) * 60;
    owned.push_back(std::move(game));
  }
  return owned;
}

}  // namespace mira::steam
