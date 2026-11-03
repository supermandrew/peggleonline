#pragma once
// The online duel flow: Duel -> Local/Online -> Host/Join -> connect -> host picks a level -> both play.

#include <string>

namespace online {

void Install();
void Update();  // once per frame on the game thread

bool InSession();
bool IsHost();
std::string PlayerName(int player);  // an online duel's: player 0 is the host
void SetNameOverride(const std::string& name);  // testing

}  // namespace online
