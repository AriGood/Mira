#pragma once

#include <string>

#include "core/Result.h"
#include "model/Types.h"
#include "runner/RunnerRegistry.h"

namespace mira::runner {

// Runs `winetricks --unattended <verb>` against a game's own Wine/Proton
// prefix, rather than reimplementing its crowdsourced verb definitions.
// Soft dependency on `winetricks` being on PATH: missing is reported
// clearly, never a startup failure.
//
// Resolves each runner kind's WINE binary once here (WineRunner's build path
// IS the binary; Proton/SteamRunner bundle one at files/bin/wine) rather
// than teaching winetricks-awareness to every IRunner.
Result<void> RunTricksVerb(const RunnerRegistry& runners, const model::Game& game, const std::string& verb);

}  // namespace mira::runner
