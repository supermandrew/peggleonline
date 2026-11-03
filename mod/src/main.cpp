// Peggle Nights online duel mod.
// Loaded as a proxy bass.dll from the game folder (the original is bass_orig.dll; every export is forwarded
// by bass_exports.cpp). It only activates inside the real game process, after checking the game build.
#include <windows.h>

#include <cstdint>

#include "arena.h"
#include "chaos.h"
#include "controls.h"
#include "debug.h"
#include "display.h"
#include "game.h"
#include "hook.h"
#include "log.h"
#include "online.h"
#include "padmenu.h"
#include "present.h"
#include "replays.h"

HMODULE g_module;

namespace {

bool IsSupportedGame()
{
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    if (base != 0x400000)
        return false;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    return nt->FileHeader.TimeDateStamp == game::kTimestamp && nt->OptionalHeader.SizeOfImage == game::kSizeOfImage;
}

// Logs the first few hardware exceptions with registers and likely return addresses, so a crash can be traced
// even when the game's own crash reporter fails. The exception is then handled exactly as before.
constexpr DWORD kCppException = 0xE06D7363;  // MSVC throw

// The type name of a thrown C++ object, from the throw information MSVC passes along.
const char* ThrownType(const EXCEPTION_RECORD* r)
{
    if (r->NumberParameters < 3)
        return "?";
    const auto* throw_info = reinterpret_cast<const uint32_t*>(r->ExceptionInformation[2]);  // ThrowInfo
    const auto* types = reinterpret_cast<const uint32_t*>(throw_info[3]);                     // CatchableTypeArray
    const auto* first = reinterpret_cast<const uint32_t*>(types[1]);                         // CatchableType
    return reinterpret_cast<const char*>(first[1] + 8);                                    // TypeDescriptor name
}

LONG CALLBACK CrashLogger(EXCEPTION_POINTERS* ep)
{
    static LONG logged;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_STACK_OVERFLOW && code != kCppException)
        return EXCEPTION_CONTINUE_SEARCH;
    static LONG cpp_logged;  // the game may throw and catch some of its own; they get a count of their own
    if (InterlockedIncrement(code == kCppException ? &cpp_logged : &logged) > (code == kCppException ? 8 : 3))
        return EXCEPTION_CONTINUE_SEARCH;
    const CONTEXT* c = ep->ContextRecord;
    Log("EXCEPTION %08lX at %08lX (mod %p) eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX "
        "esp=%08lX",
        code, c->Eip, g_module, c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    if (code == kCppException)
        Log("  C++ exception: %s", ThrownType(ep->ExceptionRecord));
    auto mod_base = reinterpret_cast<uintptr_t>(g_module);
    const auto* stack = reinterpret_cast<const uintptr_t*>(c->Esp);
    char line[512];
    int len = 0;
    for (int i = 0; i < 256 && len < 450; ++i) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(stack + i, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT)
            break;
        uintptr_t v = stack[i];
        bool in_game = v >= 0x401000 && v < 0x66F000;
        bool in_mod = v >= mod_base && v < mod_base + 0x100000;
        if (in_game || in_mod)
            len += wsprintfA(line + len, " %s%08lX", in_mod ? "mod:" : "", in_mod ? v - mod_base : v);
    }
    Log("  stack:%s", line);
    return EXCEPTION_CONTINUE_SEARCH;
}

using DrawDirtyFn = bool(__thiscall*)(void* app);
using RedrawFn = bool(__thiscall*)(void* app, void* rect);
DrawDirtyFn g_orig_draw_dirty;
RedrawFn g_orig_redraw;

// The mod's work for each frame. The game redraws only what changed, and when nothing did (the in-game Menu over
// a paused board) Redraw comes once a second, which would drop a quick tap on the controller; DrawDirtyStuff
// comes every frame. Input the mod delivers from here (the controller, the test pipe) can open one of the game's
// dialogs that runs its own loop until closed, and that loop comes back here every frame. So each part does its
// input last, after its own bookkeeping, and runs again inside such a loop like it would for a mouse click.
bool __fastcall HookDrawDirty(void* app, void*)
{
    online::Update();
    chaos::Update();
    replays::Update();
    padmenu::Update();
    display::Update();
    controls::Update();
    bool r = g_orig_draw_dirty(app);
    DebugPump();
    return r;
}

bool __fastcall HookRedraw(void* app, void*, void* rect)
{
    PresentFixUpdate();
    return g_orig_redraw(app, rect);
}

void Init()
{
    char tag[32];
    wsprintfA(tag, "%lu", GetCurrentProcessId());
    LogInit(tag);
    Log("netplay mod loaded into game process %lu", GetCurrentProcessId());
    AddVectoredExceptionHandler(1, CrashLogger);
    display::Install();  // before the game makes its window
    arena::Install();
    g_orig_draw_dirty =
        reinterpret_cast<DrawDirtyFn>(HookVtable(game::kAppVtable, game::kAppSlotDrawDirty, HookDrawDirty));
    g_orig_redraw = reinterpret_cast<RedrawFn>(HookVtable(game::kAppVtable, game::kAppSlotRedraw, HookRedraw));
    online::Install();
    chaos::Install();
    replays::Install();
    controls::Install();
    DebugStart();
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void*)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = inst;
        DisableThreadLibraryCalls(inst);
        // The forwarded exports need bass_orig.dll; load it from our own folder so the DLL search order
        // (the game exe lives elsewhere) can't pick up a different copy.
        LoadLibraryA((ModDir() + "bass_orig.dll").c_str());
        if (IsSupportedGame())
            Init();
    }
    return TRUE;
}
