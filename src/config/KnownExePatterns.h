#pragma once

// Curated lists of executable-name glob patterns for the detector. Edit this
// file to add or fix an entry — it's just data, included by Schema.cpp,
// which only wires these in as the default for a normal, still user-editable
// setting (detect.deny_name_patterns / detect.installer_name_patterns in
// settings.toml — nobody needs to touch this file or rebuild just to
// override the list for themselves).
//
// Grounded in real prior art where possible, not guessed: *crashhandler*
// exists because UnityCrashHandler*.exe getting auto-picked over the real
// game exe is a documented, still-open Lutris bug
// (github.com/lutris/lutris/issues/6881).

#include <array>
#include <string_view>

namespace mira::config::known_exe_patterns {

// Prerequisite installers, plus engine/launcher helper processes that ship
// inside a real game's own folder and would otherwise be auto-picked over
// the actual game.
inline constexpr std::array kDeny = {
    // prerequisite installers — redistributables commonly dropped next to a
    // game's own exe by InstallShield/Inno Setup packaging
    "unins*", "setup*", "vcredist*", "vc_redist*", "dxsetup*", "dxwebsetup*", "dotnet*", "ndp*",
    "directx*", "*redist*", "touchup*", "oalinst*", "physxinstaller*", "*physx_setup*",
    "gfwlivesetup*", "xnafx*", "*prereqsetup*", "*prerequisites*",
    // Unreal Engine helpers
    "*crashreport*", "*cefsubprocess*", "*crashpad*",
    // Unity helpers — see the Lutris issue cited above
    "*crashhandler*",
    // other common crash reporters (BugTrap/BugSplat, and legacy Dr. Watson)
    "*bugsplat*", "bssndrpt*", "crashsender*", "drwtsn32*",
    // launcher / DRM / anti-cheat helpers bundled alongside a game — these
    // ship as their own subfolder+exe in nearly every game that uses them
    // (e.g. EasyAntiCheat/EasyAntiCheat.exe, BattlEye/BEService.exe)
    "*launcher_installer*", "*webhelper*", "upc.exe", "*anticheat*", "battleye*", "beservice*",
    "pnkbstr*",
};

// A candidate matching one of these, combined with a size signal (see
// detect.installer_min_size_mb), is flagged as an installer rather than the
// game itself.
inline constexpr std::array kInstaller = {
    "setup*", "*setup*", "install*", "*installer*", "*_setup_*",
};

}  // namespace mira::config::known_exe_patterns
