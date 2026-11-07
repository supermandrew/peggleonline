#pragma once
// Deterministic lockstep of the duel board. Both games run the full simulation. Whoever is in control
// (the host while masters are chosen, then the player whose turn it is) runs the board freely and streams
// every input event and tick; the other game replays the stream tick for tick. All randomness inside the
// simulation comes from a private generator seeded identically on both sides. The game in control says when control
// passes, in the stream, and the other game goes by that whatever its own board says. As a shot ends and as a turn
// begins it says what its whole board came to; if the other game's differs, it sends its board and both load it, so
// the shot as its player saw it stands.
#include <cstdint>
#include <string>

#include "net.h"

namespace lockstep {

void Install();

// Run `fn` (board creation, etc.) as synchronized code: shared random numbers, fixed FPU mode, and the game's
// update counter held at `clock` + tick until End().
void SeedAndRunSynced(uint32_t seed, uint16_t fpu_control, uint32_t clock, void (*fn)(void*), void* arg);

void Begin(void* board, bool is_host, const char* host_name, const char* client_name);
void End();
bool Active();
uint32_t CurrentTick();
std::string DebugState();  // testing
void HoldAt(uint32_t tick);  // testing: pause the simulation at `tick` (0xFFFFFFFF: run)
bool DumpBoard(const std::string& path);  // testing: raw memory of the board, its LogicMgr and cannon
void SetNewLevelHandler(void (*fn)());  // host chose "Play New Level" on the results screen

void Update();                         // once per frame (bookkeeping, desync checks)
bool HandleMessage(const net::Message& m);  // returns true if it was a lockstep message

uint16_t CurrentFpuControl();

// The board's pointer input (enter, move, drag) is dropped while `filter(slot)` says so, before it reaches the
// stream: the controller parks the pointer without aiming the cannon at it.
void SetBoardInputFilter(bool (*filter)(int slot));
// Buttons that do nothing for this player online (the opponent's portraits, what only the host decides), which
// the controller's menu navigation leaves out.
bool ControllerSkips(void* widget);

}  // namespace lockstep
