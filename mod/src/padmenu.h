#pragma once
// Controller settings: a CONTROLLER button in the game's Options dialog opens a dialog to choose the button for
// each action, turn vibration on or off, and set how fast the left stick aims.
#include <string>

namespace padmenu {

void Update();     // once per frame: keeps the CONTROLLER button in the Options dialog
bool Capturing();  // an action is waiting for the controller button that will do it
void Capture();    // once per frame while capturing (instead of the controller's usual work)
std::string DebugState();

}  // namespace padmenu
