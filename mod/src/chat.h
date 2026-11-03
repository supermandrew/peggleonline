#pragma once
// Chat during an online duel: a CHAT button at the top left opens a list of set phrases, and what both players
// say shows in a box at the bottom left that fades a few seconds after the last message. None of it touches the
// shared simulation: the button and box are widgets of their own, above the board.
#include <string>

#include "net.h"

namespace chat {

void Update(bool on_board);  // once per frame: the button is on screen while an online duel's board is
void SetNames(const std::string& me, const std::string& opponent);
void OnMessage(const net::Message& m);
void Note(const std::string& text);  // a line in this player's box only (the mod's news: "Replay saved")
void End();  // the session is over: take everything off the screen and forget what was said

std::string DebugState();  // testing

// For the controller: the CHAT button and the phrase list.
bool Available();  // the CHAT button is on screen
void* Button();
bool ListOpen();
void ToggleList();
int RowCount();
bool RowCenter(int row, int& x, int& y);  // where phrase `row` is in the open list

}  // namespace chat
