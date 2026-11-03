#pragma once
// What the controller does in the game. On every screen the stick or D-pad moves between the buttons (the game's
// own hover look shows where you are) and A presses them; on the board the sticks aim, A shoots, and the other
// buttons do the rest. It all reaches the game as the input a mouse makes, so an online duel streams it like any
// other, and the mouse pointer hides while the controller is in use. In a local duel between two people with a
// controller each, each player's turns and master answer to their own controller.
#include <string>

namespace controls {

void Install();
void Update();             // once per frame, before the frame is drawn
void Focus(void* widget);  // move the highlight to this button

// One frame of the controller, as menu-like parts of the mod (the on-screen keyboard) need it.
struct MenuInput {
    int dir;  // -1, or 0 up, 1 down, 2 left, 3 right: on press, then repeating while held
    bool select, back, start, x, y;
};

std::string DebugState();

}  // namespace controls
