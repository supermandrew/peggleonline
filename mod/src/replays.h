#pragma once
// Instant Replays with Chaos Mode and Sabotage Mode (local duels): each shot the game records gets the effects'
// state too, which a replay puts in place while it plays; saved replay files carry it after the game's own data
// (the game reads only what it wrote, so the files still open anywhere). Online, where a replay would change one
// computer's game, the Instant Replay button is SAVE: the last shot goes to a replay file under an automatic name,
// noted in that player's own chat box. See re/NOTES.md, section 9.
#include <string>

namespace replays {

void Install();
void Update();        // once per frame
void* SaveButton();   // online: the SAVE button while it's on screen (for the controller)
std::string DebugCommand(const std::string& args);  // testing

}  // namespace replays
