#pragma once
#include <string>

// Developer channel for automated testing: a named pipe \\.\pipe\peggle-netplay-<pid> that accepts one-line
// commands and runs them on the game thread. Only started when mods\debug.txt exists.
void DebugStart();
void DebugPump();  // call on the game thread once per frame
bool DebugEnabled();  // mods\debug.txt exists: testing aids (pipe, extra diagnostics files) are on

// Save the game's current frame (the draw surface) as a 24-bit BMP.
bool SaveFrameBmp(const std::string& path);
// Same for any IDirectDrawSurface7* held by the DDInterface at the given offset.
bool SaveSurfaceBmp(int dd_offset, const std::string& path);
