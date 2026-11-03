#pragma once
// Chaos Mode and Red Peg Mode for duels, local and online.
// Chaos Mode: both players' first shots are normal, then every round (each player shooting once) brings a random
// effect, named in the middle of the screen, and the next round's replaces it.
// Red Peg Mode: one blue peg of the level is red. Whoever hits it gives the other player an effect for their next
// turn (one shot), named the same way; there is no red peg after that. With both on, the two effects stack (never
// the same one twice).
// Everything that changes the game happens inside the board's ticks, so two online games stay identical.
#include <cstdint>
#include <string>

#include "net.h"

namespace chaos {

enum Effect {
    kNone = -1,
    kBigBalls,
    kPinballMadness,
    kDoubleBucket,
    kTeleportingBucket,
    kPurpleParty,
    kPegShuffle,
    kInvisiblePegs,
    kBlindShot,
    kDoubleShot,
    kWormholes,
    kWrapAround,
    kBombPeg,
    kLightsOut,
    kUpsideDown,
    kPortalStorm,
    kBulletTime,
    kMirage,  // the red peg's own (it also gives Blind Shot and Lights Out)
    kSilhouette,
    kEffectCount
};
constexpr int kChaosEffects = kMirage;  // the ones Chaos Mode deals

void Install();
void Update();  // once per frame: the boxes on the Two Player Selection

// The boxes: this player's choices (remembered), and online the host's, which the joiner's boxes show.
bool Wanted();     // CHAOS MODE
bool RedWanted();  // RED PEG MODE
bool HandleMessage(const net::Message& m);  // returns true if it was the host's choice

// Play Now, online: whether this duel has each mode and the seed of its random choices, the same on both
// computers. (A local duel takes the boxes and a seed of its own.)
void SetOnlineDuel(bool chaos, bool red, uint32_t seed);
// After each tick of a board (inside the shared simulation, online).
void AfterTick(void* board);
// The board was loaded from a copy (an Instant Replay began or ended): no turn began or ended by it.
void BoardReloaded(void* board);

// The effects' random numbers (an Instant Replay plays a shot with the ones it had).
uint32_t RandomState();
void SetRandomState(uint32_t s);

uint32_t StateHash();  // what both computers' modes must agree on
std::string DebugState();
std::string DebugCommand(const std::string& args);  // testing: force effects

}  // namespace chaos
