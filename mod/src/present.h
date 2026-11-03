#pragma once
#include <string>

// How each frame reaches the window. The game shows a frame by blitting its 800x600 draw surface to the DirectDraw
// primary surface; on some current Windows/driver combinations that Blt reports success but never reaches the
// screen (the window stays white). When the game is windowed the mod puts the frame in the window itself: copied
// as it is when the window is 800x600, otherwise scaled into display::PictureRect with black around it, on the
// graphics card (Direct3D 9, into a window of the mod's over the game's, since Direct3D's present to the game window
// itself goes missing the same way) or, failing that, with GDI.
void PresentFixUpdate();  // call once per frame on the game thread (installs the hook when the primary exists)
std::string PresentState();  // testing: how the last frame went out
