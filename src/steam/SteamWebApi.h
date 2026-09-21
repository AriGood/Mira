#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "config/Config.h"
#include "core/Result.h"

namespace mira::steam {

// One title the Steam account owns, per Steam's own Web API — including
// ones that aren't installed, which is the part Steam's on-disk files
// (libraryfolders.vdf/appmanifest_*.acf, see SteamDetector) can't tell you.
struct OwnedGame {
  std::string appid;
  std::string name;
  std::int64_t play_seconds = 0;  // playtime_forever, which Steam reports in minutes
};

// IPlayerService/GetOwnedGames. Needs steam.web_api_key + steam.steamid64,
// both unset by default: with either missing this returns
// Err("no_steam_web_api_key"/"no_steamid64", ...) rather than guessing, and
// callers treat that as "this source contributes nothing", not a failure.
//
// Shells out to curl rather than linking libcurl, same as everything else
// that talks to the network here (see metadata/MetadataFetcher.cpp,
// runner/Downloader.cpp) -- occasional, human-triggered traffic, not a hot
// path.
Result<std::vector<OwnedGame>> ListOwnedGames(const config::Config& config);

}  // namespace mira::steam
