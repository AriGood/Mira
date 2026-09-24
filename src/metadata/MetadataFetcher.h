#pragma once

#include <filesystem>
#include <string>

#include "config/Config.h"
#include "core/Result.h"
#include "model/Types.h"

namespace mira::metadata {

// Cover art + store metadata for a game, cached on disk under
// paths::UserDir() (never in games.toml, since it's always re-fetchable).
// Steam-owned games use Steam's store API + ProtonDB + Steam's CDN, all
// keyless; everything else uses SteamGridDB (needs steamgriddb.api_key, or
// fails with no_steamgriddb_key). Shells out to curl rather than linking
// libcurl, same as runner/Downloader.cpp. Synchronous — see
// metadata/FetchQueue.h for the background-safe wrapper.
Result<void> Fetch(const config::Config& config, const model::Game& game);

// SteamGridDB's matches for `name`, best first: [{id, name, release_date?}].
// Fetch uses the first unless the game sets metadata.steamgriddb_id.
Result<nlohmann::json> SearchSteamGridDb(const config::Config& config, const std::string& name);

// Re-downloads one SteamGridDB candidate (by the id it was listed with in
// info["art_candidates"][slot], from a prior Fetch()) and makes it the
// active image for `slot`, leaving every other cached slot untouched.
Result<void> SelectArtwork(const config::Config& config, const std::string& game_id, const std::string& slot,
                           std::int64_t candidate_id);

// Absolute paths to a game's cached files, whether or not they exist yet.
// Siblings of settings.toml (config.File().parent_path()/metadata,
// .../artwork) rather than a global XDG_CONFIG_HOME lookup of their own —
// same reasoning as frontend.toml living next to settings.toml: it follows
// wherever this particular Config was actually opened from, real daemon or
// an isolated test instance, instead of re-resolving the environment itself
// and risking a mismatch.
std::filesystem::path MetadataFile(const config::Config& config, const std::string& game_id);
std::filesystem::path ArtworkDir(const config::Config& config, const std::string& game_id);

}  // namespace mira::metadata
