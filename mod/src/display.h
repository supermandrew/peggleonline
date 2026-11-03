#pragma once
// How the game's 800x600 picture reaches the screen. In a window it is as big as Windows' text scaling makes other
// programs (the mod scales it itself, sharper than Windows would); borderless, the window covers its whole monitor
// and the picture fills its height, in shape, with black bars at the sides. Either way the game maps the mouse
// back to its 800x600 with its own remapping. BORDERLESS is a checkbox in the Options dialog, next to Fullscreen,
// which it excludes.
#include <windows.h>

#include <string>

namespace display {

void Install();  // at load, before the game makes its window
void Update();   // once per frame: the window's style and size, the mouse mapping, the Options checkbox
// Where the picture goes in the window's client area, and that area's size; false without a usable window.
bool PictureRect(HWND wnd, RECT& picture, SIZE& client);
std::string DebugState();

}  // namespace display
