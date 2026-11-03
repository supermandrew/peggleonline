#pragma once
// An on-screen keyboard for typing with the controller (the Join Game address, a new player's name). It types into
// the game's text box through the box's own key handling, so the box's limits still apply.
#include <string>

#include "controls.h"

namespace keyboard {

void Open(void* edit_widget);
bool IsOpen();
void Tick();                                   // every frame: closes when its text box's dialog went away
void Update(const controls::MenuInput& input);  // the controller, while open
void Close();
std::string DebugState();

}  // namespace keyboard
