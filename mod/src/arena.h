#pragma once
// Deterministic memory for the synchronized simulation.
//
// The game keeps pegs (and other objects) in sets ordered by their memory address and picks, say, the k-th
// candidate for the purple peg. With ordinary heap allocation the two computers place the objects at
// different addresses, so the same random number picks different pegs. While synchronized code runs on the
// game thread, every allocation under 64 KB comes from this arena instead, whose allocator hands out addresses
// that depend only on the sequence of synchronized allocations and frees, which is identical on both computers.
// Blocks over 1 KB are never reused: they are mostly images, which the game frees on each computer's own clock.
#include <cstddef>
#include <cstdint>
#include <string>

namespace arena {

void Install();  // hooks the game's malloc / free / calloc / realloc / _msize

// Start a duel's allocations at `start` (0: at this computer's next free position). Returns the start used,
// for the host to send to the joiner.
uintptr_t BeginDuel(uintptr_t start);

void SetRouting(bool on);  // on while synchronized code runs (lockstep's sync scope)

struct Stats {
    size_t allocated, reused, leaked_bytes;
    uintptr_t start, used_to;  // this duel's space
};
Stats GetStats();

// Debugging: record every arena allocation (size, callers) from now on, then write them to `path`.
void StartRecording();
void MarkRecording(uint32_t tag);  // a "-- tag" line between allocations (e.g. the tick)
bool Recording();
void StopRecording(const std::string& path);
// Debugging: record every arena free (tick, size, context, allocation number, callers).
void StartFreeRecording();
void SetTickTag(uint32_t tick);
void StopFreeRecording(const std::string& path);

}  // namespace arena
