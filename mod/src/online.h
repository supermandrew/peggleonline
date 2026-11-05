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
void HideLevelsFromStage(int stage);             // testing: as if the level packs from that stage on weren't installed

}  // namespace online
