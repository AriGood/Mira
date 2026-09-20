#pragma once

#include <sys/types.h>

namespace mira::gamemode {

// Whether gamemoded/gamemoderun is on PATH -- distinct from IsDaemonRunning,
// so a caller can tell "not installed" apart from "installed but not
// running".
bool IsInstalled();

// Whether com.feralinteractive.GameMode currently owns its name on the
// session bus, i.e. gamemoded is actually reachable -- via the
// desktop-bus-standard NameHasOwner call, not anything GameMode-specific.
bool IsDaemonRunning();

// RegisterGame(pid)/UnregisterGame(pid) over GameMode's D-Bus interface, not
// a linked libgamemode (see GameMode.cpp). Both are best-effort and never
// block or fail a launch -- a missing daemon just means nothing happens.
void RegisterGame(pid_t pid);
void UnregisterGame(pid_t pid);

}  // namespace mira::gamemode
