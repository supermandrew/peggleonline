#include "hook.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cstring>
#include <vector>

#include "log.h"

void PatchBytes(uintptr_t addr, const void* bytes, size_t len)
{
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(addr), len, PAGE_EXECUTE_READWRITE, &old);
    memcpy(reinterpret_cast<void*>(addr), bytes, len);
    VirtualProtect(reinterpret_cast<void*>(addr), len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), len);
}

void* HookVtable(uintptr_t vtable, int slot, void* fn)
{
    uintptr_t entry = vtable + slot * 4;
    void* old = *reinterpret_cast<void**>(entry);
    PatchBytes(entry, &fn, 4);
    return old;
}

static void WriteJump(uint8_t* at, const void* target)
{
    at[0] = 0xE9;
    *reinterpret_cast<int32_t*>(at + 1) = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) -
                                                               reinterpret_cast<uintptr_t>(at) - 5);
}

void* HookInline(uintptr_t addr, size_t stolen, void* fn)
{
    auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    memcpy(tramp, reinterpret_cast<void*>(addr), stolen);
    WriteJump(tramp + stolen, reinterpret_cast<void*>(addr + stolen));

    uint8_t patch[16];
    memset(patch, 0x90, sizeof patch);
    WriteJump(patch, fn);
    // WriteJump computes the displacement from the patch buffer, so fix it for the real address.
    *reinterpret_cast<int32_t*>(patch + 1) =
        static_cast<int32_t>(reinterpret_cast<uintptr_t>(fn) - addr - 5);
    PatchBytes(addr, patch, stolen);
    return tramp;
}

void SuspendOtherThreads(bool suspend)
{
    static std::vector<HANDLE> suspended;
    if (!suspend) {
        for (HANDLE t : suspended) {
            ResumeThread(t);
            CloseHandle(t);
        }
        suspended.clear();
        return;
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = {sizeof te};
    DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me)
            continue;
        if (HANDLE t = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID)) {
            if (SuspendThread(t) != static_cast<DWORD>(-1))
                suspended.push_back(t);
            else
                CloseHandle(t);
        }
    }
    CloseHandle(snap);
}

uintptr_t HookCall(uintptr_t call_addr, void* fn)
{
    auto* p = reinterpret_cast<uint8_t*>(call_addr);
    if (p[0] != 0xE8) {
        Log("HookCall: no call at %08X", static_cast<unsigned>(call_addr));
        return 0;
    }
    uintptr_t old = call_addr + 5 + *reinterpret_cast<int32_t*>(p + 1);
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(fn) - call_addr - 5);
    PatchBytes(call_addr + 1, &rel, 4);
    return old;
}
