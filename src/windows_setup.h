// ng2 - Ninja Gaiden II static recompilation
//
// Windows first-run setup (windows_setup.cpp): what the AppImage's launcher
// does on Linux.

#pragma once

#include <filesystem>

namespace rex {
struct PathConfig;
}

namespace ng2::windows_setup {

// Fills in what the command line left out: the game folder (remembered, or
// asked for once, with Title Update 3's default.xexp extracted into it from
// its package), and the settings, saves and log in %LOCALAPPDATA%. False if
// the player cancelled or the game can't start.
bool ConfigurePaths(rex::PathConfig& paths);

// Power throttling off and the system timer at 1 ms for this process (as SDL
// already sets it). Logged. NG2_TIMER_PERIOD=0 leaves the throttling as it is.
void TuneProcess();

}  // namespace ng2::windows_setup
