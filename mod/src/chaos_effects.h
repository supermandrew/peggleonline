#pragma once
// The effects of Chaos Mode and Sabotage Mode (chaos_effects.cpp), driven by chaos.cpp. A round's effect and a red
// peg's can be in play together. Begin and End run inside the board's ticks (the shared simulation online), and
// what the effects do as the game runs happens in each physics step. What the effects draw only reads that state,
// and moves with the steps too, so two online games show the same picture at the same tick.
#include <cstdint>
#include <string>

#include "chaos.h"

namespace chaos {

void InstallEffects();
void UpdateEffects();  // once per frame, outside the simulation: pictures made for a board that went are deleted
void BoardDrawBegins(void* board);  // around a board's drawing: what's drawn upside down set up for it
void BoardDrawEnds();

bool EffectPossible(void* board, Effect e);  // e.g. Purple Party needs three blue pegs
void BeginEffect(void* board, Effect e);
void EndEffect(void* board, Effect e);  // the board is still there
void DropEffects(void* keep_board);     // effects on any other board: forgotten (that board went away)
uint32_t EffectHash();
std::string EffectDebug();  // testing: what the hooks did
bool DebugBombAt(float x, float y);
bool DebugDropIn();
void DebugPlaceBall(float x, float y, float vx, float vy);  // testing: the next ball to move (screen)
void DebugSpooky();  // testing: the next shot is a Spooky Ball that comes back once
// Testing: the player in turn (or the one given) has a master's power (the game's number: 2 Flippers, 4 Pyramid,
// 6 Spooky Ball, 12 Fireball, 14 Chain Lightning...) for this many turns; for the player in turn its objects are on
// or off now, as when a turn begins.
bool DebugPower(void* board, int power, int turns, int player = -1);

// Sabotage Mode: the red peg (a blue peg drawn red, never made purple or shuffled), chosen inside the ticks.
void* ChooseRedPeg(void* board);  // a random unhit blue peg (round or a brick, still or moving), now the red one
void ForgetRedPeg(void* keep_board);  // unless it's on that board
bool RedPegHit();                       // the red peg is lit (or gone)
// Testing: the unhit blue peg (of a kind: 0 any, 1 round and still, 2 a brick, 3 moving) nearest a screen point is
// the red one.
bool DebugRedAt(void* board, float x, float y, int kind);

// Mirage's way in: as the turn passes the board goes black, the fakes appear in the dark, and it lights up again.
void StartBlackout(void* board);
void CancelBlackout();
struct BlackoutState {  // where it has got to, for a repair online (it's timed by the board's steps)
    int32_t phase;
    uint32_t steps;
};
BlackoutState GetBlackout();
void SetBlackout(void* board, const BlackoutState& b);

// Instant Replay. The game records each shot as it's fired (a copy of the whole board, then where the balls went)
// and a replay loads that copy and plays it again, the balls put back where they were each step. What the effects
// add lives in the mod, so each recorded shot gets their state as well (replays.cpp), put in place while it plays
// and the live state back afterwards. Loading a copy makes new objects, so pegs are kept by their place in the
// level's order and where they are. Saved with a replay file: the layout may only grow at the end.
constexpr int kShotMaxFakes = 14, kShotMaxPortals = 6, kShotMaxPurples = 2;
#pragma pack(push, 4)
struct ShotPeg {
    int index;  // -1: none
    float x, y;
};
struct ShotFake {
    float x, y;
    uint32_t orange, glitched;
};
constexpr int kShotMaxEffects = 32;  // (room for more effects without moving what follows)
static_assert(kEffectCount <= kShotMaxEffects, "ShotState::ticks");
struct ShotState {
    uint32_t size;  // sizeof(ShotState) of the build that wrote it
    uint32_t active;
    uint32_t ticks[kShotMaxEffects];
    uint32_t rng;
    int32_t shooter;  // the player who fired
    ShotPeg red;
    uint32_t red_clock, red_hit_at;
    float red_x, red_y;
    ShotPeg bomb;
    uint32_t bomb_blown;
    ShotPeg purples[kShotMaxPurples];
    int32_t purple_count;
    uint32_t next_teleport, teleported_at;
    float teleport_from;
    float portals[kShotMaxPortals][2];
    int32_t portal_count;
    ShotFake fakes[kShotMaxFakes];
    int32_t fake_count;
    uint32_t flipped;
    uint32_t trace, trace_steps;  // where the balls went in the shot (testing), once it's over
    ShotPeg fake_pegs[kShotMaxFakes];  // the gone peg each fake is in the place of (-1: a round one of its own)
};
#pragma pack(pop)
void CaptureShot(void* board, ShotState& s);
void ApplyShot(void* board, const ShotState& s);  // after the board was loaded from a copy
// Testing: a hash of where the balls are after each physics step of a shot, since it was fired.
uint32_t Trace();
uint32_t TraceSteps();
void ResetTrace();

// Random numbers for effects: the same sequence on both computers (only inside the simulation).
uint32_t Random(uint32_t below);

}  // namespace chaos
