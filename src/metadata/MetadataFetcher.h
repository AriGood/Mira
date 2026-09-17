#pragma once

#include <filesystem>
#include <string>

#include "config/Config.h"
#include "core/Result.h"
#include "model/Types.h"

namespace mira::metadata {

// Cover art + store metadata for a game, fetched from public web APIs and
// cached on disk under paths::UserDir() — never in games.toml, since none of
// it is user-editable state and it can always be re-fetched. Two sources,
// picked by whether `game` is Steam-owned (runner_ref "steam:<appid>"):
//
//   Steam-owned: Steam's own public store API (description, genres, release
//   date, developers/publishers, metacritic — no key needed), ProtonDB's
//   compatibility tier (no key needed), and cover art from Steam's CDN (no
//   key needed).
//
//   Everything else: SteamGridDB, matched by name search, for cover art
//   only — there is no equivalent free metadata source for a non-Steam
//   game. Needs steamgriddb.api_key configured; silently skipped without one.
//
// Shells out to curl, consistent with runner/Downloader.cpp, rather than
// linking libcurl for what's occasional, human-triggered-adjacent traffic.
// Synchronous — see metadata/FetchQueue.h for the background-safe wrapper
// every real caller should use instead of calling this directly off a
// short-lived thread.
Result<void> Fetch(const config::Config& config, const model::Game& game);

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
