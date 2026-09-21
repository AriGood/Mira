#pragma once

#include <string>

#include <json.hpp>

#include "config/Config.h"
#include "core/Result.h"

// Minimal JSON-RPC 2.0 client for butlerd (itch.io's own
// launcher-integration daemon --
// https://itch.io/docs/butler/launcher-integration.html), confirmed live
// against a real `butler daemon --json --dbpath ...` run: it prints a mix
// of its own JSON log lines and one
// {"type":"butlerd/listen-notification","secret":"...",
// "tcp":{"address":"host:port"}} line to stdout, then serves JSON-RPC 2.0
// over that TCP address, one message per line, newline-delimited.
//
// Unlike every other source here, this is a long-lived daemon connection,
// not a one-shot subprocess per call -- Call() below starts butler once,
// lazily, on the first call any code makes, and keeps that connection for
// the rest of this mirad's run: there's no cheap "is it already running"
// check the way Legendary/gogdl's own detection has, so tearing the
// daemon down between calls would mean paying its startup cost (a real
// subprocess spawn plus a database open) on every single itch.io API
// call. The exact request/response shapes for Fetch.ProfileOwnedKeys and
// Fetch.Caves are per itch's own documentation, not yet confirmed against
// a real logged-in account -- parsed defensively (see ItchImporter.cpp)
// so a field-name surprise degrades gracefully instead of crashing.
namespace mira::itch {

// One request/response round trip. `method`/`params` are butlerd's own
// (Meta.Authenticate, Profile.LoginWithAPIKey, Fetch.ProfileOwnedKeys,
// Fetch.Caves, Install.Queue/PlanUpload/Perform, ...). Connects (and
// authenticates the *transport* itself, via Meta.Authenticate -- not the
// itch.io account; see itch::Login for that) on first use if not already
// connected. Any butlerd notification received while waiting for this
// call's own response (e.g. install progress pushed during
// Install.Perform) is skipped, not surfaced here.
Result<nlohmann::json> Call(const config::Config& config, const std::string& method,
                           const nlohmann::json& params);

}  // namespace mira::itch
