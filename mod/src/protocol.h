#pragma once
// Messages between the two copies of the game (little-endian structs, both sides run the same build).
#include <cstdint>

namespace proto {

constexpr uint32_t kVersion = 8;

enum Type : uint8_t {
    kHello = 1,    // Hello
    kBye = 2,      // (empty) the other player left on purpose
    kPing = 3,     // (empty) keep-alive
    kLevel = 10,   // Level: host picked a level, both create the board
    kStream = 11,  // lockstep items from the player in control (see lockstep.cpp)
    kSelect = 12,  // Select: a player picked their master in the selection dialog
    kNewLevel = 13,  // (empty) host left the results screen to pick another level
    kDesync = 14,    // u32 tick: the games were found to differ (diagnostics)
    kChat = 15,      // Chat: a player said one of the set phrases (chat.cpp)
    kDuelOptions = 16,  // DuelOptions: the host's boxes on the Two Player Selection, for the joiner to see
};

#pragma pack(push, 1)
struct Hello {
    uint32_t version;
    uint32_t game_timestamp;
    char name[32];
    uint32_t clock;  // sender's update counter when sent
};

struct Level {
    int32_t stage;
    int32_t level;
    uint32_t seed;
    uint16_t fpu_control;  // host's x87 control word, used by both sides inside the simulation
    uint32_t arena_start;  // where the host's deterministic allocations for this duel begin
    uint32_t clock;        // the game's update counter at tick 0, on both computers (above both counters)
};

struct Select {
    uint8_t player;  // 0 host (left column), 1 joiner (right column)
    uint8_t master;  // portrait index 0..10
};

struct Chat {
    uint8_t phrase;  // index into chat.cpp's list (both players run the same version)
};

struct DuelOptions {  // (what the duel gets is in the host's Play Now, in the stream)
    uint8_t chaos;     // Chaos Mode
    uint8_t red;       // Red Peg Mode
};
#pragma pack(pop)

}  // namespace proto
