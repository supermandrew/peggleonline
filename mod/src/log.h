#pragma once
#include <string>

// Directory of the mod DLL (the game folder), with trailing backslash.
const std::string& ModDir();

void LogInit(const char* tag);
void Log(const char* fmt, ...);
