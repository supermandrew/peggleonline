#pragma once
#include <cstddef>
#include <cstdint>

// Overwrite bytes in the game image (handles page protection).
void PatchBytes(uintptr_t addr, const void* bytes, size_t len);

// Replace a vtable slot; returns the previous function.
void* HookVtable(uintptr_t vtable, int slot, void* fn);

// Detour a function: the first `stolen` bytes (whole instructions, no relative jumps or calls)
// are moved to a trampoline that then jumps back. Returns the trampoline, which calls the original.
void* HookInline(uintptr_t addr, size_t stolen, void* fn);

// Point an E8 call instruction at a new target; returns the old target.
uintptr_t HookCall(uintptr_t call_addr, void* fn);

// Suspend (or resume) every other thread of the game, for patching code that other threads may be running.
void SuspendOtherThreads(bool suspend);
