#pragma once

#include <string>

#include "core/Result.h"
#include "model/Types.h"
#include "runner/RunnerRegistry.h"

namespace mira::runner {

// Runs `winetricks --unattended <verb>` against a game's own Wine/Proton
// prefix — the maintenance escape hatch this project deliberately shells
// out to rather than reimplementing: winetricks' value is its hundreds of
// crowdsourced, constantly-updated verb definitions for known-broken
// redistributables, not the script itself (same call already made for
// wine/umu-run — see docs/architecture.md, Replaceability). Requires the
// real `winetricks` on PATH, a soft dependency like wine/umu-run: missing
// is reported clearly, never a startup failure.
//
// Every runner kind resolves to a WINE binary differently — WineRunner's
// build path IS the binary; ProtonRunner/SteamRunner bundle one inside
// their own tree at files/bin/wine — that translation lives here once,
// rather than teaching winetricks-awareness to every IRunner.
Result<void> RunTricksVerb(const RunnerRegistry& runners, const model::Game& game, const std::string& verb);

}  // namespace mira::runner
