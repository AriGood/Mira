#pragma once

#include <sys/types.h>

namespace mira::gamemode {

// Whether gamemoded/gamemoderun is on PATH at all -- distinct from whether
// the daemon is actually reachable right now (IsDaemonRunning below), so a
// caller can tell "not installed" apart from "installed but not running",
// which are different problems needing different messages.
bool IsInstalled();

// Whether com.feralinteractive.GameMode currently owns its well-known name
// on the session bus, i.e. gamemoded is actually up and reachable -- checked
// via the desktop-bus-standard NameHasOwner call, not gamemode-specific, so
// this works the same way regardless of which D-Bus implementation owns the
// session bus.
bool IsDaemonRunning();

// RegisterGame(pid)/UnregisterGame(pid) over GameMode's own D-Bus interface
// (com.feralinteractive.GameMode at /com/feralinteractive/GameMode) --
// shelled out to via gdbus rather than linking libgamemode, matching this
// project's established shell-out-rather-than-link convention (curl,
// sqlite3, flatpak, ...). Both are best-effort and never block or fail a
// launch: a missing daemon just means nothing happens, exactly as if the
// game had requested GameMode itself and gotten the same answer.
void RegisterGame(pid_t pid);
void UnregisterGame(pid_t pid);

}  // namespace mira::gamemode
