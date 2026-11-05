#include "lockstep.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <vector>

#include "arena.h"
#include "chaos.h"
#include "chat.h"
#include "debug.h"
#include "dialogs.h"
#include "game.h"
#include "hook.h"
#include "log.h"
#include "pad.h"
#include "protocol.h"
#include "ui.h"

namespace lockstep {

namespace {

// ---- Game addresses ----
constexpr uintptr_t kRandStub = 0x56B510;   // `jmp` every game caller of Sexy::Rand goes through
constexpr uintptr_t kIs3DGetter = 0x54FD60;  // AL = app->mDDInterface->mIs3D (EAX = app)
constexpr uintptr_t kCrtRand = 0x636203;     // C runtime rand()
constexpr uintptr_t kBoardVtable = 0x6864A4;
constexpr int kSlotUpdate = 22;
constexpr int kAppOffWidgetManager = 0x320;
constexpr int kBoardOffLogic = 0x720;
constexpr int kLogicOffState = 0x04;
constexpr int kLogicOffStateSteps = 0x08;  // logic steps run in the current state
constexpr int kLogicOffCurrentPlayer = 0x1AC;
constexpr int kLogicOffBalls = 0x208;  // int[2]
constexpr int kStateOver = 5, kStateIntro = 8, kStateSelection = 9;  // also 2/3 ball in play
constexpr int kStateAiming = 1, kStateTurnPassing = 6;  // every shot comes through 6, on to the next turn or the results
// The turn passes 150 logic steps into state 6 at the earliest (0x4763D0), and fast-forward runs 8 of them a tick.
constexpr int kTurnPassingWaitSteps = 150 - 2 * 8;
constexpr uintptr_t kDuelBookkeepingCall = 0x46BEC7;  // the duel ends: 0x40A440, ECX board
constexpr uintptr_t kCopyBoard = 0x405D00;  // stdcall(board) -> a new DataWriter holding all of the board
constexpr uintptr_t kLoadBoard = 0x405DB0;  // ECX DataWriter, EDX board, stdcall(bool with the shot's inputs) -> AL
constexpr int kWriterSlotWrite = 1;         // thiscall(const void*, size)
constexpr int kBoardOffPowerDisplay = 0x704;  // the master's display: its flash as a power's count changes runs on
constexpr int kDisplayOffFlash = 0x30;        // each computer's own drawing, so it's no part of the comparison
constexpr int kWriterOffData = 0x08, kWriterOffSize = 0x0C;
constexpr int kAppOffDialogCount = 0x32C;   // how many dialogs are open (the size of the app's std::map of them)
constexpr int kDialogLeaveGame = 4, kDialogGameMenu = 19;
constexpr int kLogicOffAim = 0xEC;                 // float: angle the cannon turns towards
constexpr int kLogicOffAimDirty = 0x20;            // bool: the mouse moved, re-aim at the next update
constexpr int kBoardOffCannon = 0xD4, kCannonOffAngle = 0x150;
constexpr int kWmOffMouseX = 0xE0, kWmOffMouseY = 0xE4, kWmOffActualButtons = 0xE8, kWmOffButtons = 0xEC;
constexpr int kWmOffKeys = 0xF5;            // bool[256], polled by the board (arrow keys)
constexpr int kBoardOffWidgetFlags = 0x57;  // focus / down / mouse-over, polled by the board
constexpr int kBoardOffReplayButton = 0x738;  // Instant Replay button: holding the right button over it doesn't
constexpr int kWidgetOffMouseOver = 0x59;     // fast-forward the game
constexpr int kWidgetOffVisible = 0x54;
constexpr int kWidgetOffParent = 0x14;
constexpr int kButtonOffId = 0x88;

// Selection dialog ("Two Player Selection", dialog id 1)
constexpr uintptr_t kCharDlgButtonVtable = 0x68E630;    // 0: ButtonPress(id, clicks)  2: ButtonDepress(id)
constexpr uintptr_t kCharDlgCheckboxVtable = 0x68E650;  // 0: CheckboxChecked(id, checked)
constexpr int kCharDlgId = 1;
constexpr int kDialogListenerOffset = 0x88;  // ButtonListener sub-object inside a Dialog
constexpr int kCharDlgOffMasters = 0x22C;    // per player: std::vector of 12-byte entries, `selected` at +4
constexpr int kMastersPerColumn = 12;
constexpr int kMasterRandom = 11;
constexpr int kButtonPlayNow = 1000;
constexpr int kButtonEditName1 = 100, kButtonEditName2 = 101;

constexpr int kCharDlgOffName1 = 0x1C0, kCharDlgOffName2 = 0x1DC;  // std::string: names above the columns
// Widgets that only make sense offline: the two name EDIT buttons, Play a Friend / Play Computer, and the
// computer's difficulty (Easy, Normal, Hard, Master), one pointer after the other.
constexpr int kCharDlgOffOfflineControls = 0x190, kCharDlgOfflineControls = 8;

// Pausing the board (menu, lost focus, Space, the Lucky Spin dialog): ESI = board, stack: bool pause.
constexpr uintptr_t kBoardSetPaused = 0x407010;
// Coin flip for the first turn (LogicMgr::SetupTurnOrder): globals that avoid streaks across duels.
constexpr uintptr_t kCoinFlipLastFirst = 0x6C8068, kCoinFlipStreak = 0x7051C0;
// Saving an unfinished game when a board is closed (userdata/duel%d.sav): ESI = board.
constexpr uintptr_t kBoardSaveGame = 0x405940;
// Presentation the simulation calls, with session-wide caches: run outside it (see SyncPause).
// stdcall(board): start the next track of the level music. The first call in a session fills a static
// playlist and shuffles it with rand().
constexpr uintptr_t kPlayLevelMusic = 0x4082B0;
// fastcall(curve, text): set an animation curve from its text form. Each text is parsed once per session into a
// global cache (32 KB table per curve).
constexpr uintptr_t kSetCurveFromText = 0x434630;
// stdcall(frame, flag): rebuild the board frame's images for the level's theme. The frame outlives boards, so
// it only rebuilds when the theme differs from the last level played.
constexpr uintptr_t kBuildFrameSkin = 0x4634A0;
// EBX = board: render the intro's title card ("Stage %d-%d") into a new image.
constexpr uintptr_t kBuildTitleCard = 0x40C0A0;
// Sounds: SoundManager::GetSoundInstance(id) creates an instance that releases itself once played, when the
// sound manager notices (on its own clock, or during the next sound request).
constexpr uintptr_t kSoundManagerVtable = 0x696DAC;
constexpr int kSlotGetSoundInstance = 8;
// stdcall(board): (re)start the board's intro curves, including the fade-in that holds Board::Update back.
constexpr uintptr_t kStartIntroCurves = 0x407F70;
constexpr int kBoardOffFadeIn = 0x138, kCurveOffStart = 0x48;
constexpr uintptr_t kFadeProgressFn = 0x435410;       // ESI = curve: progress 0..1 on the FPU stack
constexpr int kAppOffTransitionList = 0x920;
constexpr uintptr_t kTransitionListHasFn = 0x4338C0;  // EAX = list, stdcall(widget): is it in the list
constexpr uintptr_t kBoardInTransitionCall = 0x40E499;  // Board::Update's only use of it

// Lucky Spin slot machine: a widget owned by the board, shown while spinning.
constexpr uintptr_t kSlotMachineVtable = 0x68E24C;  // 22: Update   54: MouseDown(x, y, clicks)
constexpr int kBoardOffSlotMachine = 0x730;
constexpr int kWidgetOffManager = 0x10;  // non-null while the widget is on screen

// End-of-duel results: an EndLevelDialog owned by the board, shown when the duel is over.
constexpr uintptr_t kEndLevelButtonVtable = 0x68E0D0;  // 2: ButtonDepress(id)
constexpr int kBoardOffResults = 0x72C;
constexpr int kResultsNewLevel = 0, kResultsMainMenu = 1000;  // 1001 Replay Level, 1 Change Character
// Other buttons that must not change one game only.
constexpr uintptr_t kBoardButtonVtable = 0x6865C0;     // 2: ButtonDepress(id): 0 Menu, 1 Instant Replay
constexpr int kBoardButtonInstantReplay = 1;
constexpr uintptr_t kOptionsButtonVtable = 0x68F0E0;   // 2: ButtonDepress(id): 9 Main Menu, 10 New Level, ...
constexpr int kOptionsNewLevel = 10, kOptionsNewChallenge = 11, kOptionsSkip = 12;

enum Target : uint8_t { kTargetBoard = 0, kTargetSlotMachine = 1, kTargetEndLevel = 2 };

// Board input methods hooked (vtable slot -> number of int arguments).
struct InputSlot {
    int slot, args;
};
constexpr InputSlot kInputSlots[] = {{47, 1}, {48, 1}, {49, 1}, {50, 0}, {51, 0}, {52, 2},
                                     {54, 3}, {56, 3}, {58, 2}, {59, 1}};

// ---- Shared random numbers (MT19937, same output range as the game's: 31 bits) ----
struct MersenneTwister {
    uint32_t mt[624];
    int index = 625;
    uint32_t calls = 0;

    void Seed(uint32_t s)
    {
        mt[0] = s;
        for (int i = 1; i < 624; ++i)
            mt[i] = 1812433253u * (mt[i - 1] ^ (mt[i - 1] >> 30)) + i;
        index = 624;
        calls = 0;
    }

    uint32_t Next()
    {
        static const uint32_t mag01[2] = {0, 0x9908B0DF};
        if (index >= 624) {
            int k = 0;
            for (; k < 624 - 397; ++k) {
                uint32_t y = (mt[k] & 0x80000000) | (mt[k + 1] & 0x7FFFFFFF);
                mt[k] = mt[k + 397] ^ (y >> 1) ^ mag01[y & 1];
            }
            for (; k < 623; ++k) {
                uint32_t y = (mt[k] & 0x80000000) | (mt[k + 1] & 0x7FFFFFFF);
                mt[k] = mt[k + (397 - 624)] ^ (y >> 1) ^ mag01[y & 1];
            }
            uint32_t y = (mt[623] & 0x80000000) | (mt[0] & 0x7FFFFFFF);
            mt[623] = mt[396] ^ (y >> 1) ^ mag01[y & 1];
            index = 0;
        }
        uint32_t y = mt[index++];
        y ^= y >> 11;
        y ^= (y << 7) & 0x9D2C5680;
        y ^= (y << 15) & 0xEFC60000;
        y ^= y >> 18;
        ++calls;
        return y & 0x7FFFFFFF;
    }
};

MersenneTwister g_mt;

// ---- Synchronized-code scope ----
DWORD g_game_thread;            // only the game thread's calls are part of the simulation
int g_depth;                    // > 0 while simulation code runs that both games must run identically
uint16_t g_fpu_control = 0x027F;
uint16_t g_saved_fpu;
uint32_t g_saved_mxcsr;
uint32_t g_fixed_mxcsr = 0x1F80;
uint32_t g_tick;
uint32_t g_level_seed;  // the level's shared seed (Chaos Mode's effects start from it)

// The game's update counter is its clock: animation curves (including the board's fade-in, which holds the
// simulation back until it is far enough along) measure time with it, and a few things use its value directly.
// Each computer advances it every frame whether or not the duel moved on, so during a duel it follows the
// duel's own ticks instead, from a start both computers share.
bool g_clock_held;
uint32_t g_clock_start;

void HoldClock()
{
    if (g_clock_held)
        game::At<uint32_t>(game::App(), game::kAppOffUpdateCount) = g_clock_start + g_tick;
}

void EnterSync()
{
    if (g_depth++ == 0) {
        HoldClock();
        arena::SetRouting(true);
        __asm {
            fnstcw g_saved_fpu
            fldcw g_fpu_control
            stmxcsr g_saved_mxcsr
            ldmxcsr g_fixed_mxcsr
        }
    }
}

void LeaveSync()
{
    if (--g_depth == 0) {
        __asm {
            fldcw g_saved_fpu
            ldmxcsr g_saved_mxcsr
        }
        arena::SetRouting(false);
    }
}

struct SyncScope {
    SyncScope() { EnterSync(); }
    ~SyncScope() { LeaveSync(); }
};

// Presentation code that the simulation calls runs as ordinary local code: its own random numbers and heap.
int g_paused_depths[16];
int g_pause_level;

void __cdecl PauseSync()
{
    int saved = g_depth;
    g_paused_depths[g_pause_level++ & 15] = saved;
    if (saved) {
        g_depth = 0;
        arena::SetRouting(false);
    }
}

void __cdecl ResumeSync()
{
    int saved = g_paused_depths[--g_pause_level & 15];
    if (saved) {
        arena::SetRouting(true);
        g_depth = saved;
    }
}

struct SyncPause {
    SyncPause() { PauseSync(); }
    ~SyncPause() { ResumeSync(); }
};

uint32_t __cdecl SyncedRand() { return g_mt.Next(); }

uintptr_t g_original_rand = 0x566C50;  // MTRand_Next on the game's global generator

// Diagnostics: callers that draw random numbers outside the simulation while a duel is running.
bool g_watch_unsynced;
uintptr_t g_unsynced_seen[64];
int g_unsynced_count;

void __cdecl NoteUnsynced(uintptr_t ret, int which)
{
    for (int i = 0; i < g_unsynced_count; ++i)
        if (g_unsynced_seen[i] == ret)
            return;
    if (g_unsynced_count < 64) {
        g_unsynced_seen[g_unsynced_count++] = ret;
        Log("lockstep: %s called outside the simulation from %08X", which ? "rand()" : "Rand()", ret);
    }
}

__declspec(naked) void RandHook()
{
    __asm {
        cmp g_depth, 0
        je unsynced
        push eax
        mov eax, fs:[0x24]
        cmp eax, g_game_thread
        pop eax
        je synced
    unsynced:
        cmp g_watch_unsynced, 0
        je original
        push 0
        push dword ptr [esp + 4]
        call NoteUnsynced
        add esp, 8
    original:
        jmp dword ptr [g_original_rand]
    synced:
        jmp SyncedRand
    }
}

// The C runtime's rand() is used too (purple and green pegs, among others). It keeps its state per thread;
// inside the simulation it uses a shared state with the same formula instead.
uint32_t g_crt_state;
uint32_t g_crt_calls;
uintptr_t g_getptd = 0x6419A6;  // CRT _getptd: per-thread data, rand state at +0x14

// Diagnostics: who draws C-runtime random numbers while the board is built (logged once setup is done).
bool g_setup_running;
uintptr_t g_setup_callers[16];
int g_setup_caller_count;

void __cdecl NoteSetupCaller(uintptr_t ret)
{
    for (int i = 0; i < g_setup_caller_count; ++i)
        if (g_setup_callers[i] == ret)
            return;
    if (g_setup_caller_count < 16)
        g_setup_callers[g_setup_caller_count++] = ret;
}

__declspec(naked) void CrtRandHook()
{
    __asm {
        cmp g_depth, 0
        je unsynced
        push eax
        mov eax, fs:[0x24]
        cmp eax, g_game_thread
        pop eax
        je synced
    unsynced:
        cmp g_watch_unsynced, 0
        je original
        push 1
        push dword ptr [esp + 4]
        call NoteUnsynced
        add esp, 8
    original:
        call dword ptr [g_getptd]
        mov ecx, [eax + 0x14]
        imul ecx, ecx, 0x343FD
        add ecx, 0x269EC3
        mov [eax + 0x14], ecx
        mov eax, ecx
        shr eax, 16
        and eax, 0x7FFF
        ret
    synced:
        cmp g_setup_running, 0
        je draw
        push dword ptr [esp]
        call NoteSetupCaller
        add esp, 4
    draw:
        mov eax, g_crt_state
        imul eax, eax, 0x343FD
        add eax, 0x269EC3
        mov g_crt_state, eax
        inc g_crt_calls
        shr eax, 16
        and eax, 0x7FFF
        ret
    }
}

// The board asks how far the player has got in Adventure (stage * 5 + level, from the profile): purple pegs need
// 2, green ones 1, special pegs stay on the player's newest level, and the opening has hints for new players.
// The two players' profiles differ, so inside the simulation both games report the same: far along, and not on
// the level being played.
constexpr uintptr_t kProfileProgress = 0x42DC90;  // EAX app -> EAX (6-byte prologue)
constexpr uintptr_t kOnNewestLevel = 0x42DCC0;    // EDX app -> AL (6-byte prologue)
constexpr int kSharedProgress = 1000;
void* g_progress_tramp;
void* g_newest_level_tramp;

__declspec(naked) void ProgressHook()
{
    __asm {
        cmp g_depth, 0
        je original
        push eax
        mov eax, fs:[0x24]
        cmp eax, g_game_thread
        pop eax
        jne original
        mov eax, kSharedProgress
        ret
    original:
        jmp dword ptr [g_progress_tramp]
    }
}

__declspec(naked) void NewestLevelHook()
{
    __asm {
        cmp g_depth, 0
        je original
        push eax
        mov eax, fs:[0x24]
        cmp eax, g_game_thread
        pop eax
        jne original
        xor al, al
        ret
    original:
        jmp dword ptr [g_newest_level_tramp]
    }
}

// The particle count of several effects depends on whether 3D acceleration is on, which would make the
// two games draw different numbers of random values. Inside the simulation both report "on".
__declspec(naked) void Is3DHook()
{
    __asm {
        cmp g_depth, 0
        je unsynced
        push ecx
        mov ecx, fs:[0x24]
        cmp ecx, g_game_thread
        pop ecx
        je synced
    unsynced:
        mov ecx, [eax + 0x36C]
        mov al, [ecx + 0x38]
        ret
    synced:
        mov al, 1
        ret
    }
}

void WriteJump(uintptr_t at, void* target)
{
    uint8_t jmp[5] = {0xE9};
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - at - 5);
    memcpy(jmp + 1, &rel, 4);
    PatchBytes(at, jmp, 5);
}

// ---- Polled input (what the board reads directly instead of through events) ----
#pragma pack(push, 1)
struct Polled {
    int16_t x, y;
    uint8_t actual_buttons, buttons;
    uint8_t widget[3];
    uint8_t replay_over;
    uint8_t keys[32];  // bitmask of the 256 key-down flags
    bool operator==(const Polled& o) const { return memcmp(this, &o, sizeof *this) == 0; }
};
#pragma pack(pop)

void* Wm() { return game::At<void*>(game::App(), kAppOffWidgetManager); }

uint8_t* ReplayOver(void* board)
{
    void* button = board ? game::At<void*>(board, kBoardOffReplayButton) : nullptr;
    return button ? &game::At<uint8_t>(button, kWidgetOffMouseOver) : nullptr;
}

Polled CapturePolled(void* board)
{
    void* wm = Wm();
    Polled p = {};
    p.x = static_cast<int16_t>(game::At<int>(wm, kWmOffMouseX));
    p.y = static_cast<int16_t>(game::At<int>(wm, kWmOffMouseY));
    p.actual_buttons = static_cast<uint8_t>(game::At<int>(wm, kWmOffActualButtons));
    p.buttons = static_cast<uint8_t>(game::At<int>(wm, kWmOffButtons));
    memcpy(p.widget, static_cast<char*>(board) + kBoardOffWidgetFlags, 3);
    if (uint8_t* over = ReplayOver(board))
        p.replay_over = *over;
    auto* keys = static_cast<uint8_t*>(wm) + kWmOffKeys;
    for (int i = 0; i < 256; ++i)
        if (keys[i])
            p.keys[i >> 3] |= 1 << (i & 7);
    return p;
}

struct SavedLocal {
    int x, y, actual_buttons, buttons;
    uint8_t widget[3];
    uint8_t replay_over;
    uint8_t keys[256];
};

void OverridePolled(void* board, const Polled& p, SavedLocal& saved)
{
    void* wm = Wm();
    saved.x = game::At<int>(wm, kWmOffMouseX);
    saved.y = game::At<int>(wm, kWmOffMouseY);
    saved.actual_buttons = game::At<int>(wm, kWmOffActualButtons);
    saved.buttons = game::At<int>(wm, kWmOffButtons);
    if (board)
        memcpy(saved.widget, static_cast<char*>(board) + kBoardOffWidgetFlags, 3);
    if (uint8_t* over = ReplayOver(board)) {
        saved.replay_over = *over;
        *over = p.replay_over;
    }
    memcpy(saved.keys, static_cast<uint8_t*>(wm) + kWmOffKeys, 256);
    game::At<int>(wm, kWmOffMouseX) = p.x;
    game::At<int>(wm, kWmOffMouseY) = p.y;
    game::At<int>(wm, kWmOffActualButtons) = p.actual_buttons;
    game::At<int>(wm, kWmOffButtons) = p.buttons;
    if (board)
        memcpy(static_cast<char*>(board) + kBoardOffWidgetFlags, p.widget, 3);
    auto* keys = static_cast<uint8_t*>(wm) + kWmOffKeys;
    for (int i = 0; i < 256; ++i)
        keys[i] = (p.keys[i >> 3] >> (i & 7)) & 1;
}

void RestorePolled(void* board, const SavedLocal& saved)
{
    void* wm = Wm();
    game::At<int>(wm, kWmOffMouseX) = saved.x;
    game::At<int>(wm, kWmOffMouseY) = saved.y;
    game::At<int>(wm, kWmOffActualButtons) = saved.actual_buttons;
    game::At<int>(wm, kWmOffButtons) = saved.buttons;
    if (board)
        memcpy(static_cast<char*>(board) + kBoardOffWidgetFlags, saved.widget, 3);
    if (uint8_t* over = ReplayOver(board))
        *over = saved.replay_over;
    memcpy(static_cast<uint8_t*>(wm) + kWmOffKeys, saved.keys, 256);
}

// ---- The stream ----
enum ItemKind : uint8_t {
    kItemPolled = 1,   // Polled: input state for the following events and ticks
    kItemEvent = 2,    // u8 target, u8 slot, i32 a, i32 b, i32 c: an input call on a synchronized widget
    kItemTick = 3,     // u32 tick: run one Board::Update
    kItemPlayNow = 4,  // u8 master1, u8 master2, u8 options (1: Chaos Mode, 2: Red Peg Mode): Play Now pressed in
                       // the selection dialog
    kItemCheck = 5,    // u32 tick, u32 hash: state fingerprint after that tick
    kItemBoard = 6,    // u32 size, BoardHeader, the board, the modes' state: both games take this board, right here
    kItemTurn = 7,     // TurnCheck: what the whole board came to as a shot ended or a turn began
    kItemControl = 8,  // u32 tick: the one reading this is in control from here on
};

bool g_active;
bool g_is_host;
bool g_local_control;  // this game drives the board right now
bool g_duel_started;   // Play Now has been applied
bool g_replaying;      // a remote input event is being applied
void* g_board;
std::vector<uint8_t> g_out;
std::vector<uint8_t> g_in;
size_t g_in_pos;
int g_in_ticks;  // complete ticks buffered in g_in
Polled g_last_sent;
bool g_sent_any;
Polled g_remote;
std::string g_host_name, g_client_name;
bool g_desync_reported;
uint32_t g_mismatches;  // ticks found to differ, since the last status line

// Per-tick trace, dumped to the log on both computers when they are found to differ.
struct TraceEntry {
    uint32_t tick, mt_calls, crt_calls;
    int state, player;
    uint32_t allocs, reused, arena_used;
    float aim, cannon;  // LogicMgr's aim angle, and the cannon's
    uint8_t aim_dirty;
    uint32_t fade_start;
    float fade;          // the fade-in's progress, as Board::Update measures it (logic waits for 0.2)
    uint8_t transition;  // the board is in the app's transition list (skips the intro curves' steps)
};
TraceEntry g_trace[256];
constexpr uint32_t kRecordTicks = 1000;  // debug mode: how long allocations and frees are recorded after setup

void RecordTrace();
void DumpTrace(const char* why);
bool g_desync_notice;  // shown to the player once per duel
DWORD g_stat_time;
int g_stat_ticks;
uint32_t g_hold_at = 0xFFFFFFFF;  // testing: stop the simulation at this tick on both computers

using UpdateFn = void(__thiscall*)(void*);
UpdateFn g_orig_update;
void* g_orig_input[64];

template <typename T>
void Put(std::vector<uint8_t>& v, const T& value)
{
    const auto* p = reinterpret_cast<const uint8_t*>(&value);
    v.insert(v.end(), p, p + sizeof value);
}

void Flush()
{
    if (g_out.empty())
        return;
    net::Send(proto::kStream, g_out.data(), g_out.size());
    g_out.clear();
}

void EmitPolled(void* board)
{
    Polled p = CapturePolled(board);
    if (g_sent_any && p == g_last_sent)
        return;
    g_last_sent = p;
    g_sent_any = true;
    g_out.push_back(kItemPolled);
    Put(g_out, p);
}

void* LogicMgr(void* board) { return game::At<void*>(board, kBoardOffLogic); }
int CurrentPlayer(void* board) { return game::At<int>(LogicMgr(board), kLogicOffCurrentPlayer); }
int LogicState(void* board) { return game::At<int>(LogicMgr(board), kLogicOffState); }

// A load leaves the vectors of what outlives it as big as they had grown (LogicMgr+0xC0, its ball records at +0xE0, the
// physics manager's lists), and those aren't the same once a shot has gone differently on the two computers. So after a
// repair the two games ask for memory at different moments, for good, and their counters are no longer compared. It
// changes nothing: addresses only matter in the pegs' sets, and a load builds those in the same order on both.
bool g_allocators_apart;

uint32_t StateHash(void* board)
{
    void* lm = LogicMgr(board);
    uint32_t h = 2166136261u;
    auto mix = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            h ^= (v >> (i * 8)) & 0xFF;
            h *= 16777619u;
        }
    };
    mix(g_mt.calls);
    mix(static_cast<uint32_t>(g_mt.index));
    mix(g_crt_calls);
    mix(g_crt_state);
    mix(game::At<uint32_t>(lm, kLogicOffState));
    mix(game::At<uint32_t>(lm, kLogicOffCurrentPlayer));
    mix(game::At<uint32_t>(lm, kLogicOffBalls));
    mix(game::At<uint32_t>(lm, kLogicOffBalls + 4));
    if (!g_allocators_apart) {
        arena::Stats a = arena::GetStats();  // a different allocation sequence soon means different peg order
        mix(static_cast<uint32_t>(a.allocated));
        mix(static_cast<uint32_t>(a.reused));
        mix(static_cast<uint32_t>(a.used_to - a.start));
    }
    mix(chaos::StateHash());
    return h;
}

// Diagnostics: turn-state changes and the selection dialog, with the tick they happen on.
int g_last_state = -1;
bool g_last_sel;

void TraceState()
{
    int st = game::At<int>(LogicMgr(g_board), kLogicOffState);
    bool sel = game::GetDialog(kCharDlgId) != nullptr;
    if (st != g_last_state || sel != g_last_sel)
        Log("lockstep: tick %u state %d->%d selection %d->%d (rng %u+%u, depth %d)", g_tick, g_last_state, st,
            g_last_sel, sel, g_mt.calls, g_crt_calls, g_depth);
    g_last_state = st;
    g_last_sel = sel;
}

// Only the game in control decides, from synchronized state, and it says so in the stream: the other game takes over
// at that very point, whatever its own board says. Were each to go by its own board, a board that had gone another way
// would leave one waiting for the other for good, or both running. (Not from dialogs: they open inside the simulation
// but close on each computer's own clock, while they slide away.)
void UpdateControl()
{
    TraceState();
    if (!g_local_control)
        return;
    int st = LogicState(g_board);  // the host runs everything between duels: intro, selection, results
    bool host_controls = !g_duel_started || st == kStateIntro || st == kStateSelection || st == kStateOver ||
                         CurrentPlayer(g_board) == 0;
    if (host_controls == g_is_host)
        return;
    g_local_control = false;
    g_out.push_back(kItemControl);
    Put(g_out, g_tick);
    Flush();
    Log("lockstep: tick %u, opponent is in control (player %d's turn)", g_tick, CurrentPlayer(g_board) + 1);
}

void TakeControl()  // (a tick runs before this game can hand back, so boards that disagree still move on)
{
    g_local_control = true;
    Log("lockstep: tick %u, we are in control (player %d's turn)", g_tick, CurrentPlayer(g_board) + 1);
}

// ---- Calling the board's original methods ----
void CallInput(void* board, int slot, int a, int b, int c)
{
    void* fn = g_orig_input[slot];
    for (const InputSlot& s : kInputSlots) {
        if (s.slot != slot)
            continue;
        if (s.args == 0)
            reinterpret_cast<void(__thiscall*)(void*)>(fn)(board);
        else if (s.args == 1)
            reinterpret_cast<void(__thiscall*)(void*, int)>(fn)(board, a);
        else if (s.args == 2)
            reinterpret_cast<void(__thiscall*)(void*, int, int)>(fn)(board, a, b);
        else
            reinterpret_cast<void(__thiscall*)(void*, int, int, int)>(fn)(board, a, b, c);
    }
}

void* SlotMachine() { return g_board ? game::At<void*>(g_board, kBoardOffSlotMachine) : nullptr; }
void* Results() { return g_board ? game::At<void*>(g_board, kBoardOffResults) : nullptr; }
bool ResultsShowing()
{
    void* r = Results();
    return r && game::At<void*>(r, 0x10);  // on screen (has a widget manager)
}

UpdateFn g_orig_slot_update;
using Input3Fn = void(__thiscall*)(void*, int, int, int);
Input3Fn g_orig_slot_mouse_down;
using ButtonDepressFn = void(__thiscall*)(void*, int);
ButtonDepressFn g_orig_endlevel_depress;

void CallTarget(int target, int slot, int a, int b, int c)
{
    switch (target) {
    case kTargetBoard:
        CallInput(g_board, slot, a, b, c);
        break;
    case kTargetSlotMachine:
        if (void* w = SlotMachine())
            g_orig_slot_mouse_down(w, a, b, c);
        break;
    case kTargetEndLevel:
        if (void* dlg = Results())
            g_orig_endlevel_depress(static_cast<char*>(dlg) + kDialogListenerOffset, a);
        break;
    }
}

// Input on a synchronized widget. Returns false when the caller should run the game's own handler directly
// (not online, or the call comes from inside the simulation itself).
int g_pointer_in_sync;  // diagnostics: pointer events ignored this duel

void NoteEvent(int target, int slot, int a, int b, bool ours)  // debug mode: the opening's input events
{
    if (DebugEnabled() && g_tick < kRecordTicks)
        Log("lockstep: tick %u event %d/%d (%d, %d) %s", g_tick, target, slot, a, b, ours ? "ours" : "theirs");
}

bool OnInput(int target, int slot, int a, int b, int c)
{
    if (!g_active)
        return false;
    if (g_depth > 0 || g_replaying) {
        // When widgets come and go during a tick, the window manager re-checks what is under the mouse and tells
        // the board (enter, leave, move). That depends on each computer's own screen, such as a dialog that is
        // still sliding away, so the board ignores it; real mouse movement reaches it through the stream.
        bool pointer = target == kTargetBoard && (slot == 50 || slot == 51 || slot == 52 || slot == 58);
        if (pointer && g_pointer_in_sync++ < 5)
            Log("lockstep: tick %u, ignored a pointer event (slot %d) from inside the simulation", g_tick, slot);
        return pointer;
    }
    if (!g_local_control)
        return true;  // the opponent is in control: our own mouse and keys don't touch the game
    NoteEvent(target, slot, a, b, true);
    EmitPolled(g_board);
    g_out.push_back(kItemEvent);
    g_out.push_back(static_cast<uint8_t>(target));
    g_out.push_back(static_cast<uint8_t>(slot));
    Put(g_out, a);
    Put(g_out, b);
    Put(g_out, c);
    {
        SyncScope sync;
        CallTarget(target, slot, a, b, c);
    }
    if (g_active)
        UpdateControl();
    return true;
}

bool (*g_board_input_filter)(int slot);

void BoardInput(void* board, int slot, int a, int b, int c)
{
    if (g_board_input_filter && g_board_input_filter(slot))
        return;  // the controller has the pointer parked; the board never saw it move (controls.cpp)
    if (board != g_board || !OnInput(kTargetBoard, slot, a, b, c))
        CallInput(board, slot, a, b, c);
}

template <int Slot>
void __fastcall Input0(void* board, void*)
{
    BoardInput(board, Slot, 0, 0, 0);
}
template <int Slot>
void __fastcall Input1(void* board, void*, int a)
{
    BoardInput(board, Slot, a, 0, 0);
}
template <int Slot>
void __fastcall Input2(void* board, void*, int a, int b)
{
    BoardInput(board, Slot, a, b, 0);
}
template <int Slot>
void __fastcall Input3(void* board, void*, int a, int b, int c)
{
    BoardInput(board, Slot, a, b, c);
}

// ---- Selection dialog ----
void* SelectionDialog() { return game::GetDialog(kCharDlgId); }

int SelectedMaster(void* dlg, int player)
{
    char* vec = static_cast<char*>(dlg) + kCharDlgOffMasters + player * 0x10;
    char* begin = game::At<char*>(vec, 4);
    char* end = game::At<char*>(vec, 8);
    for (int i = 0; begin && begin + i * 0xC < end; ++i)
        if (begin[i * 0xC + 4])
            return i;
    return 0;
}

// Select directly (the dialog's own path refuses masters the local player hasn't unlocked).
void SetSelectedMaster(void* dlg, int player, int master)
{
    char* vec = static_cast<char*>(dlg) + kCharDlgOffMasters + player * 0x10;
    char* begin = game::At<char*>(vec, 4);
    char* end = game::At<char*>(vec, 8);
    for (int i = 0; begin && begin + i * 0xC < end; ++i)
        begin[i * 0xC + 4] = i == master;
    game::At<uint8_t>(dlg, 0x21B) = 1;
}

using ButtonPressFn = void(__thiscall*)(void*, int, int);
using CheckboxFn = void(__thiscall*)(void*, int, int);
ButtonPressFn g_orig_sel_press;
ButtonDepressFn g_orig_sel_depress;
CheckboxFn g_orig_sel_checkbox;

void ApplyPlayNow(int master1, int master2, int options)
{
    void* dlg = SelectionDialog();
    if (!dlg) {
        Log("lockstep: Play Now but no selection dialog");
        return;
    }
    SetSelectedMaster(dlg, 0, master1);
    SetSelectedMaster(dlg, 1, master2);
    chaos::SetOnlineDuel((options & 1) != 0, (options & 2) != 0, g_level_seed ^ 0x9E3779B9);
    {
        SyncScope sync;
        g_orig_sel_depress(static_cast<char*>(dlg) + kDialogListenerOffset, kButtonPlayNow);
    }
    g_duel_started = true;
    Log("lockstep: duel started at tick %u, masters %d vs %d, options %d (rng calls %u+%u)", g_tick, master1, master2,
        options, g_mt.calls, g_crt_calls);
    UpdateControl();
}

void __fastcall HookSelPress(void* listener, void*, int id, int clicks)
{
    if (!g_active) {
        g_orig_sel_press(listener, id, clicks);
        return;
    }
    int player = id < kMastersPerColumn ? 0 : 1;
    int master = id % kMastersPerColumn;
    if ((player == 0) != g_is_host || master == kMasterRandom)
        return;  // each player picks only their own master; Random would resolve differently per PC
    g_orig_sel_press(listener, id, 1);  // never a double-click: that would start the duel
    proto::Select s = {static_cast<uint8_t>(player),
                       static_cast<uint8_t>(SelectedMaster(static_cast<char*>(listener) - kDialogListenerOffset,
                                                           player))};
    net::Send(proto::kSelect, &s, sizeof s);
}

void __fastcall HookSelDepress(void* listener, void*, int id)
{
    if (!g_active || g_depth > 0) {
        g_orig_sel_depress(listener, id);
        return;
    }
    if (id == kButtonEditName1 || id == kButtonEditName2)
        return;  // names come from each player's profile
    if (id != kButtonPlayNow) {
        g_orig_sel_depress(listener, id);
        return;
    }
    if (!g_is_host)
        return;  // the host starts the duel
    void* dlg = static_cast<char*>(listener) - kDialogListenerOffset;
    int m1 = SelectedMaster(dlg, 0), m2 = SelectedMaster(dlg, 1);
    int options = (chaos::Wanted() ? 1 : 0) | (chaos::RedWanted() ? 2 : 0);
    g_out.push_back(kItemPlayNow);
    g_out.push_back(static_cast<uint8_t>(m1));
    g_out.push_back(static_cast<uint8_t>(m2));
    g_out.push_back(static_cast<uint8_t>(options));
    Flush();
    ApplyPlayNow(m1, m2, options);
}

void __fastcall HookSelCheckbox(void* listener, void*, int id, int checked)
{
    if (g_active)
        return;  // "Play Computer" is off in online duels
    g_orig_sel_checkbox(listener, id, checked);
}

// ---- Ticks ----
bool g_ticking_slot;

// The simulation asks how many dialogs are open: the intro waits for there to be none before the master selection
// (0x4737A9) and Board::Update holds two things back. One computer's own dialogs (its Menu, the mod's) don't exist
// on the other, so they don't count: else the Menu or a message left open stalls that computer's duel alone.
struct LocalDialogsHidden {
    int hidden = 0;
    LocalDialogsHidden()
    {
        int& open = game::At<int>(game::App(), kAppOffDialogCount);
        if (open == 0)
            return;
        hidden = dialogs::OpenCount() + (game::GetDialog(kDialogGameMenu) ? 1 : 0) +
                 (game::GetDialog(kDialogLeaveGame) ? 1 : 0);
        open -= hidden;
    }
    ~LocalDialogsHidden() { game::At<int>(game::App(), kAppOffDialogCount) += hidden; }
};

void RunTickSynced(void* board)
{
    LocalDialogsHidden hidden;
    SyncScope sync;
    g_orig_update(board);
    void* slot = game::At<void*>(board, kBoardOffSlotMachine);
    if (slot && game::At<void*>(slot, kWidgetOffManager)) {
        g_ticking_slot = true;
        g_orig_slot_update(slot);
        g_ticking_slot = false;
    }
    chaos::AfterTick(board);
}

void __fastcall HookSlotUpdate(void* w, void*)
{
    if (g_active && w == SlotMachine() && !g_ticking_slot)
        return;  // runs inside the synchronized board tick instead
    g_orig_slot_update(w);
}

void __fastcall HookSlotMouseDown(void* w, void*, int x, int y, int clicks)
{
    if (w != SlotMachine() || !OnInput(kTargetSlotMachine, 54, x, y, clicks))
        g_orig_slot_mouse_down(w, x, y, clicks);
}

void (*g_on_new_level)();

void __fastcall HookEndLevelDepress(void* listener, void*, int id)
{
    if (!g_active || g_depth > 0 || g_replaying) {
        g_orig_endlevel_depress(listener, id);
        return;
    }
    bool decides = g_is_host && g_local_control && LogicState(g_board) == kStateOver;
    Log("lockstep: results button %d pressed (%s)", id, decides ? "host decides" : "ignored");
    if (id == kResultsMainMenu) {
        g_orig_endlevel_depress(listener, id);  // either player may leave
        return;
    }
    if (!decides)
        return;  // the host decides what happens next, once (the dialog stays clickable while it slides away)
    if (id == kResultsNewLevel) {
        if (g_on_new_level)
            g_on_new_level();  // the joiner waits on the main menu while the host picks
        g_orig_endlevel_depress(listener, id);
        return;
    }
    OnInput(kTargetEndLevel, 0, id, 0, 0);  // replay level / change masters: both games
}

ButtonDepressFn g_orig_board_button, g_orig_options_button;

void __fastcall HookBoardButton(void* listener, void*, int id)
{
    if (id == kBoardButtonInstantReplay && g_active)
        return;  // replaying a shot on one computer only would change its board (online it's SAVE: replays.cpp)
    g_orig_board_button(listener, id);
}

void __fastcall HookOptionsButton(void* listener, void*, int id)
{
    if (g_active && (id == kOptionsNewLevel || id == kOptionsNewChallenge || id == kOptionsSkip))
        return;  // only the host changes levels, from the end-of-duel screen
    g_orig_options_button(listener, id);
}

// Music, sounds, animation curves and images are local presentation. They keep caches for the whole session or
// free their memory on their own clock (a sound releases itself once played), so inside the simulation they
// would draw random numbers or allocate and free memory differently on the two computers. They never run as
// part of the shared simulation.
using PlayLevelMusicFn = void(__stdcall*)(void* board);
using SetCurveFn = void(__fastcall*)(void* curve, const void* text);
using BuildFrameSkinFn = void(__stdcall*)(void* frame, int flag);
PlayLevelMusicFn g_orig_play_level_music;
SetCurveFn g_orig_set_curve;
BuildFrameSkinFn g_orig_build_frame_skin;

void __stdcall HookPlayLevelMusic(void* board)
{
    SyncPause pause;
    g_orig_play_level_music(board);
}

void __fastcall HookSetCurve(void* curve, const void* text)
{
    SyncPause pause;
    g_orig_set_curve(curve, text);
}

void __stdcall HookBuildFrameSkin(void* frame, int flag)
{
    SyncPause pause;
    g_orig_build_frame_skin(frame, flag);
}

void* g_orig_build_title_card;

__declspec(naked) void HookBuildTitleCard()  // register arguments: keep them all
{
    __asm {
        pushad
        call PauseSync
        popad
        call dword ptr [g_orig_build_title_card]
        pushad
        call ResumeSync
        popad
        ret
    }
}

using GetSoundInstanceFn = void*(__thiscall*)(void* manager, unsigned id);
GetSoundInstanceFn g_orig_get_sound_instance;

void* __fastcall HookGetSoundInstance(void* manager, void*, unsigned id)
{
    SyncPause pause;
    pad::OnSound(id);  // some sounds shake the controller
    return g_orig_get_sound_instance(manager, id);
}

// A new board holds its intro back while it is in the app's screen-transition list (the wipe from the previous
// screen), and that list is advanced on each computer's own frames. The online board waits a fixed number of
// ticks instead, about as long as the wipe.
uintptr_t g_in_transition_fn;
uint32_t g_transition_ticks = 20;

__declspec(naked) void HookBoardInTransition()  // EAX = transition list, stdcall(widget): AL = in the list
{
    __asm {
        cmp g_active, 0
        je original
        mov ecx, [esp + 4]
        cmp ecx, g_board
        jne original
        mov ecx, g_tick
        cmp ecx, g_transition_ticks
        setb al
        ret 4
    original:
        jmp dword ptr [g_in_transition_fn]
    }
}

using StartIntroCurvesFn = void(__stdcall*)(void* board);
StartIntroCurvesFn g_orig_start_intro_curves;

void __stdcall HookStartIntroCurves(void* board)
{
    Log("lockstep: intro curves started (board %p, %s, tick %u, clock %+d, from %p)", board,
        g_depth ? "synchronized" : "local", g_tick,
        static_cast<int>(game::At<uint32_t>(game::App(), game::kAppOffUpdateCount) - g_clock_start), _ReturnAddress());
    g_orig_start_intro_curves(board);
}

// Pausing from the menu or when the window loses focus is local to one computer; the shared board keeps
// running. Pauses made by the game itself (inside the simulation) still apply on both.
void* g_pause_trampoline;

__declspec(naked) void PauseHook()
{
    __asm {
        cmp g_active, 0
        je original
        cmp esi, g_board
        jne original
        cmp g_depth, 0
        jne original
        ret 4
    original:
        jmp dword ptr [g_pause_trampoline]
    }
}

// An online duel is not saved as the player's unfinished local duel.
// As a duel ends the game updates and saves the player's own records and builds the results: the allocations differ
// with what each player's profile holds, and nothing in them matters to the simulation.
using DuelBookkeepingFn = void(__fastcall*)(void* board);
DuelBookkeepingFn g_orig_duel_bookkeeping;

void __fastcall HookDuelBookkeeping(void* board)
{
    SyncPause pause;
    g_orig_duel_bookkeeping(board);
}

void* g_save_trampoline;
void* g_last_online_board;

__declspec(naked) void SaveHook()
{
    __asm {
        cmp esi, g_last_online_board
        je skip
        jmp dword ptr [g_save_trampoline]
    skip:
        ret
    }
}

void ApplyNames()
{
    void* dlg = SelectionDialog();
    if (!dlg)
        return;
    char* n1 = static_cast<char*>(dlg) + kCharDlgOffName1;
    char* n2 = static_cast<char*>(dlg) + kCharDlgOffName2;
    if (game::ReadString(n1) != g_host_name)
        game::AssignString(n1, g_host_name.c_str());
    if (game::ReadString(n2) != g_client_name)
        game::AssignString(n2, g_client_name.c_str());
}

// An online duel is always against the other player, under the names of the two profiles.
void HideOfflineControls()
{
    void* dlg = SelectionDialog();
    if (!dlg)
        return;
    for (int i = 0; i < kCharDlgOfflineControls; ++i) {
        void* w = game::At<void*>(dlg, kCharDlgOffOfflineControls + 4 * i);
        if (w && game::At<uint8_t>(w, kWidgetOffVisible))
            ui::SetVisible(w, false);
    }
}

// ---- Every shot and turn: the whole board, as the game copies it for Instant Replay and saved games ----
GAME_CALL void* CopyBoard(void* board)
{
    void* copy;
    __asm {
        push board
        mov eax, kCopyBoard
        call eax
        mov copy, eax
    }
    return copy;
}

GAME_CALL bool LoadBoardCopy(void* copy, void* board)
{
    uint8_t loaded;
    __asm {
        push 0
        mov ecx, copy
        mov edx, board
        mov eax, kLoadBoard
        call eax
        mov loaded, al
    }
    return loaded != 0;
}

void FreeCopy(void* copy)
{
    reinterpret_cast<void(__thiscall*)(void*, int)>((*reinterpret_cast<void***>(copy))[0])(copy, 1);
}

// As a shot ends and as the next turn begins the game in control says what its board came to (kItemTurn), and the other
// game compares at that same point of the stream. Every shot comes through the turn passing, so that game can't get
// past the end of a shot of its own: if its ball went another way and its shot is over first, it stops there until the
// stream gets that far, and never runs on into a turn of its own or the results.
struct TurnCheck {
    uint32_t tick, hash, size;
};
TurnCheck g_our_check;  // what this game's board came to at its latest check
int g_turn_check_state = -1;
uint32_t g_turn_mismatches, g_turn_matches;
bool g_at_checkpoint;       // following, and here before the stream: what it brings until then doesn't apply
bool g_unanswered;          // in control, and the other game hasn't answered this check yet
uint32_t g_unanswered_tick;

// When they differ the other game asks for the board of the game in control, whose player took the shot, so the shot
// as they saw it stands, score and all. Both games load that board at the same point of the stream, with the same
// random numbers, tick and clock.
#pragma pack(push, 1)
struct BoardHeader {
    uint32_t arena_start, seed, board_size, tick;
};
#pragma pack(pop)
constexpr int kMaxRepairsInARow = 3;  // still different after this many: something a board can't put right
bool g_board_wanted;                  // the other game has asked for this game's board
bool g_waiting_for_board;             // and the other way round: nothing in the stream applies until it comes
bool g_control_after_board;           // ... and that game has handed over already
int g_repairs_in_a_row;

void CompareTurn(const TurnCheck& theirs)
{
    g_at_checkpoint = false;
    if (g_waiting_for_board)
        return;  // (its board is on the way)
    proto::TurnReply reply = {theirs.tick, proto::kTurnSame};
    if (g_tick == theirs.tick && memcmp(&g_our_check, &theirs, sizeof theirs) == 0) {
        ++g_turn_matches;
        g_repairs_in_a_row = 0;
    } else {
        ++g_turn_mismatches;
        Log("lockstep: BOARDS DIFFER at tick %u: ours %08X (%u bytes, tick %u, we are at %u), theirs %08X (%u bytes)",
            theirs.tick, g_our_check.hash, g_our_check.size, g_our_check.tick, g_tick, theirs.hash, theirs.size);
        if (g_repairs_in_a_row < kMaxRepairsInARow) {
            g_waiting_for_board = true;
            reply.answer = proto::kTurnSendBoard;
        } else {
            g_tick = theirs.tick;
            g_desync_notice = true;
            reply.answer = proto::kTurnGivenUp;
        }
    }
    net::Send(proto::kTurnReply, &reply, sizeof reply);
}

void OnTurnReply(const proto::TurnReply& r)
{
    if (r.tick == g_unanswered_tick)
        g_unanswered = false;
    if (r.answer == proto::kTurnSame) {
        ++g_turn_matches;
        g_repairs_in_a_row = 0;
    } else if (r.answer == proto::kTurnSendBoard) {
        ++g_turn_mismatches;
        Log("lockstep: BOARDS DIFFER at tick %u: the other game asks for ours", r.tick);
        g_board_wanted = true;
    } else {
        g_desync_notice = true;
    }
}

void AdoptBoard(void* board, void* copy, const std::string& modes, uint32_t seed, uint32_t tick)
{
    g_board_wanted = g_waiting_for_board = g_at_checkpoint = g_unanswered = false;
    g_allocators_apart = true;
    ++g_repairs_in_a_row;
    g_tick = tick;
    g_mt.Seed(seed);
    g_crt_state = seed ^ 0x9E3779B9;
    g_crt_calls = 0;
    Polled neutral = {};  // (as when a board is built)
    neutral.x = 400;
    neutral.y = 300;
    SavedLocal saved;
    OverridePolled(nullptr, neutral, saved);
    bool loaded;
    {
        SyncScope sync;
        loaded = LoadBoardCopy(copy, board);
        chaos::LoadState(board, modes);
    }
    RestorePolled(nullptr, saved);
    g_desync_reported = false;
    g_mismatches = 0;
    g_turn_check_state = -1;  // the next tick checks them again
    Log("lockstep: tick %u, took one board (%u bytes, loaded %d, repair %d in a row)", g_tick,
        game::At<uint32_t>(copy, kWriterOffSize), loaded ? 1 : 0, g_repairs_in_a_row);
    chat::Note("The two games had drifted apart. Fixed!");
}

void SendBoard(void* board)
{
    void* copy = CopyBoard(board);
    std::string modes = chaos::SaveState(board);
    BoardHeader h;
    h.arena_start = static_cast<uint32_t>(arena::BeginDuel(0));
    h.seed = g_level_seed * 2654435761u + g_tick;
    h.board_size = game::At<uint32_t>(copy, kWriterOffSize);
    h.tick = g_tick;
    uint32_t total = static_cast<uint32_t>(sizeof h + h.board_size + modes.size());
    g_out.push_back(kItemBoard);
    Put(g_out, total);
    Put(g_out, h);
    const auto* data = game::At<const uint8_t*>(copy, kWriterOffData);
    g_out.insert(g_out.end(), data, data + h.board_size);
    g_out.insert(g_out.end(), modes.begin(), modes.end());
    Flush();
    AdoptBoard(board, copy, modes, h.seed, h.tick);
    FreeCopy(copy);
}

void ReceiveBoard(void* board, const uint8_t* item, uint32_t total)
{
    BoardHeader h;
    memcpy(&h, item, sizeof h);
    const uint8_t* bytes = item + sizeof h;
    arena::BeginDuel(h.arena_start);
    void* copy = CopyBoard(board);  // (a writer to hold the board that came)
    game::At<uint32_t>(copy, kWriterOffSize) = 0;
    using WriteFn = void(__thiscall*)(void*, const void*, size_t);
    reinterpret_cast<WriteFn>((*reinterpret_cast<void***>(copy))[kWriterSlotWrite])(copy, bytes, h.board_size);
    std::string modes(reinterpret_cast<const char*>(bytes + h.board_size), total - sizeof h - h.board_size);
    AdoptBoard(board, copy, modes, h.seed, h.tick);
    FreeCopy(copy);
    if (g_control_after_board) {
        g_control_after_board = false;
        TakeControl();
    }
}

bool BoardCanGo(void* board)  // not with a ball in play on the game that sends it
{
    int state = LogicState(board);
    return state == kStateAiming || state == kStateTurnPassing || state == kStateOver;
}

void TurnCheckpoint(void* board)
{
    int state = LogicState(board);
    bool reached = state != g_turn_check_state && (state == kStateAiming || state == kStateTurnPassing);
    g_turn_check_state = state;
    if (!reached)
        return;
    void* display = game::At<void*>(board, kBoardOffPowerDisplay);
    int flash = display ? game::At<int>(display, kDisplayOffFlash) : 0;
    if (display)
        game::At<int>(display, kDisplayOffFlash) = 0;
    void* copy = CopyBoard(board);
    if (display)
        game::At<int>(display, kDisplayOffFlash) = flash;
    uint32_t size = game::At<uint32_t>(copy, kWriterOffSize);
    const auto* data = game::At<const uint8_t*>(copy, kWriterOffData);
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < size; ++i)
        h = (h ^ data[i]) * 16777619u;
    // And what the next shot will draw on beside the board: the random numbers and the modes.
    for (uint32_t v : {g_mt.calls, static_cast<uint32_t>(g_mt.index), g_crt_calls, g_crt_state, chaos::StateHash()})
        h = (h ^ v) * 16777619u;
    if (DebugEnabled()) {  // what each computer had, for comparing
        std::string path = ModDir() + "mods\\logs\\board-" + std::to_string(GetCurrentProcessId()) + "-" +
                           std::to_string(g_level_seed) + "-" + std::to_string(g_tick) + ".bin";
        if (FILE* f = fopen(path.c_str(), "wb")) {
            fwrite(data, 1, size, f);
            fclose(f);
        }
    }
    FreeCopy(copy);
    Log("lockstep: turn check at tick %u: %u bytes, %08X", g_tick, size, h);
    g_our_check = {g_tick, h, size};
    if (!g_local_control) {
        g_at_checkpoint = true;  // (the stream has its check right after this tick, unless the boards have parted)
        return;
    }
    g_out.push_back(kItemTurn);
    Put(g_out, g_our_check);
    g_unanswered = true;
    g_unanswered_tick = g_tick;
}

// A difference in here alone needn't be one the players can see (the allocator's counters are in it), so it goes in
// the log; what they're told about is the boards, each turn.
void CheckHash(uint32_t tick, uint32_t theirs)
{
    uint32_t ours = StateHash(g_board);
    if (ours == theirs)
        return;
    ++g_mismatches;
    if (g_desync_reported)
        return;  // once diverged, every later tick differs too: the status line counts them
    g_desync_reported = true;
    Log("lockstep: DESYNC at tick %u: ours %08X theirs %08X (rng calls %u, state %d, player %d)", tick, ours, theirs,
        g_mt.calls, game::At<int>(LogicMgr(g_board), kLogicOffState), CurrentPlayer(g_board));
    DumpTrace("first mismatch");
    uint32_t t = tick;
    net::Send(proto::kDesync, &t, sizeof t);  // the controller dumps its trace too
}

void LocalTick(void* board)
{
    if (g_board_wanted && BoardCanGo(board))
        SendBoard(board);
    // The turn doesn't pass until the other game has answered: its board, if it wants this one, has to go before the
    // results can come up. An answer comes well before this unless that game has fallen far behind.
    if (g_unanswered && LogicState(board) == kStateTurnPassing &&
        game::At<int>(LogicMgr(board), kLogicOffStateSteps) >= kTurnPassingWaitSteps)
        return;
    EmitPolled(board);
    g_out.push_back(kItemTick);
    Put(g_out, g_tick);
    RunTickSynced(board);
    ++g_tick;
    ++g_stat_ticks;
    RecordTrace();
    TurnCheckpoint(board);
    g_out.push_back(kItemCheck);
    Put(g_out, g_tick);
    Put(g_out, StateHash(board));
    UpdateControl();
    Flush();
}

// Apply buffered items from the player in control. Returns when out of items, out of tick budget, or when
// control passes to us.
const char* g_follow_stop = "";  // diagnostics: why the follower last stopped applying the stream
uint32_t g_board_updates;

void RemoteTicks(void* board)
{
    if (g_board_wanted && BoardCanGo(board))  // (handed over already, and the other game waits for this)
        SendBoard(board);
    int budget = g_in_ticks > 30 ? 6 : g_in_ticks > 3 ? 2 : 1;
    g_follow_stop = g_local_control ? "in control" : g_tick >= g_hold_at ? "held"
                  : g_waiting_for_board ? "waiting for a board" : g_at_checkpoint ? "waiting at a check" : "caught up";
    while (!g_local_control && g_in_pos < g_in.size() && g_tick < g_hold_at) {
        const uint8_t* p = g_in.data() + g_in_pos;
        size_t left = g_in.size() - g_in_pos;
        bool skip = g_at_checkpoint || g_waiting_for_board;  // this board isn't where the stream is
        switch (p[0]) {
        case kItemPolled:
            if (left < 1 + sizeof(Polled)) {
                g_follow_stop = "partial polled";
                return;
            }
            memcpy(&g_remote, p + 1, sizeof(Polled));
            g_in_pos += 1 + sizeof(Polled);
            break;
        case kItemEvent: {
            if (left < 15) {
                g_follow_stop = "partial event";
                return;
            }
            int target = p[1], slot = p[2], a, b, c;
            memcpy(&a, p + 3, 4);
            memcpy(&b, p + 7, 4);
            memcpy(&c, p + 11, 4);
            g_in_pos += 15;
            if (skip)
                break;
            NoteEvent(target, slot, a, b, false);
            SavedLocal saved;
            OverridePolled(board, g_remote, saved);
            g_replaying = true;
            {
                SyncScope sync;
                CallTarget(target, slot, a, b, c);
            }
            g_replaying = false;
            if (!g_active)
                return;  // the event ended the duel (board gone)
            RestorePolled(board, saved);
            UpdateControl();
            break;
        }
        case kItemTick: {
            if (left < 5 || (budget == 0 && !skip)) {
                g_follow_stop = budget == 0 ? "budget" : "partial tick";
                return;
            }
            uint32_t tick;
            memcpy(&tick, p + 1, 4);
            if (skip) {
                g_in_pos += 5;
                --g_in_ticks;
                break;
            }
            if (tick != g_tick)
                Log("lockstep: stream tick %u but we are at %u", tick, g_tick);
            g_in_pos += 5;
            --g_in_ticks;
            --budget;
            SavedLocal saved;
            OverridePolled(board, g_remote, saved);
            RunTickSynced(board);
            RestorePolled(board, saved);
            ++g_tick;
            ++g_stat_ticks;
            RecordTrace();
            TurnCheckpoint(board);
            UpdateControl();
            break;
        }
        case kItemPlayNow:
            if (left < 4)
                return;
            g_in_pos += 4;
            ApplyPlayNow(p[1], p[2], p[3]);
            break;
        case kItemBoard: {
            if (left < 5)
                return;
            uint32_t total;
            memcpy(&total, p + 1, 4);
            if (left < 5 + total)
                return;
            std::vector<uint8_t> item(p + 5, p + 5 + total);
            g_in_pos += 5 + total;
            ReceiveBoard(board, item.data(), total);
            break;
        }
        case kItemCheck: {
            if (left < 9)
                return;
            uint32_t tick, hash;
            memcpy(&tick, p + 1, 4);
            memcpy(&hash, p + 5, 4);
            g_in_pos += 9;
            if (tick == g_tick && !skip)
                CheckHash(tick, hash);
            break;
        }
        case kItemTurn: {
            if (left < 1 + sizeof(TurnCheck))
                return;
            TurnCheck theirs;
            memcpy(&theirs, p + 1, sizeof theirs);
            g_in_pos += 1 + sizeof theirs;
            CompareTurn(theirs);
            break;
        }
        case kItemControl: {
            if (left < 5)
                return;
            uint32_t tick;
            memcpy(&tick, p + 1, 4);
            g_in_pos += 5;
            if (g_waiting_for_board) {
                g_control_after_board = true;
                break;
            }
            g_at_checkpoint = false;
            g_tick = tick;
            TakeControl();
            break;
        }
        default:
            Log("lockstep: bad stream item %u", p[0]);
            g_in_pos = g_in.size();
            return;
        }
    }
    if (g_in_pos > 65536 || g_in_pos == g_in.size()) {
        g_in.erase(g_in.begin(), g_in.begin() + g_in_pos);
        g_in_pos = 0;
    }
}

void __fastcall HookBoardUpdate(void* board, void*)
{
    if (!g_active || board != g_board) {
        g_orig_update(board);
        chaos::AfterTick(board);  // a local duel
        return;
    }
    HoldClock();  // undo this frame's increment: the clock moves with the ticks
    ++g_board_updates;
    if (g_tick >= g_hold_at)
        return;
    if (g_local_control)
        LocalTick(board);
    else
        RemoteTicks(board);
}

int CountTicks(const uint8_t* p, size_t n)
{
    int ticks = 0;
    for (size_t i = 0; i < n;) {
        switch (p[i]) {
        case kItemPolled: i += 1 + sizeof(Polled); break;
        case kItemEvent: i += 15; break;
        case kItemTick: i += 5; ++ticks; break;
        case kItemPlayNow: i += 4; break;
        case kItemCheck: i += 9; break;
        case kItemTurn: i += 1 + sizeof(TurnCheck); break;
        case kItemControl: i += 5; break;
        case kItemBoard: {
            if (i + 5 > n)
                return ticks;
            uint32_t total;
            memcpy(&total, p + i + 1, 4);
            i += 5 + total;
            break;
        }
        default: return ticks;
        }
    }
    return ticks;
}

GAME_CALL float FadeProgress(void* board)
{
    void* curve = static_cast<char*>(board) + kBoardOffFadeIn;
    float result;
    __asm {
        push esi
        mov esi, curve
        mov eax, kFadeProgressFn
        call eax
        fstp result
        pop esi
    }
    return result;
}

GAME_CALL bool InTransitionList(void* board)
{
    void* list = game::At<void*>(game::App(), kAppOffTransitionList);
    uint8_t listed = 0;
    if (list) {
        __asm {
            mov eax, list
            push board
            mov ecx, kTransitionListHasFn
            call ecx
            mov listed, al
        }
    }
    return listed != 0;
}

void RecordTrace()
{
    arena::SetTickTag(g_tick);
    if (arena::Recording()) {  // debug mode: the allocations of board setup and the first ticks, for comparing
        arena::MarkRecording(g_tick);
        if (g_tick == kRecordTicks)
            arena::StopRecording(ModDir() + "mods\\logs\\allocs-" + std::to_string(GetCurrentProcessId()) + ".txt");
    }
    if (DebugEnabled() && g_tick == kRecordTicks)  // and the frees of the same stretch
        arena::StopFreeRecording(ModDir() + "mods\\logs\\frees-" + std::to_string(GetCurrentProcessId()) + ".txt");
    void* lm = LogicMgr(g_board);
    void* cannon = game::At<void*>(g_board, kBoardOffCannon);
    arena::Stats a = arena::GetStats();
    g_trace[g_tick & 255] = {g_tick,
                             g_mt.calls,
                             g_crt_calls,
                             game::At<int>(lm, kLogicOffState),
                             CurrentPlayer(g_board),
                             static_cast<uint32_t>(a.allocated),
                             static_cast<uint32_t>(a.reused),
                             static_cast<uint32_t>(a.used_to - a.start),
                             game::At<float>(lm, kLogicOffAim),
                             cannon ? game::At<float>(cannon, kCannonOffAngle) : 0.0f,
                             game::At<uint8_t>(lm, kLogicOffAimDirty),
                             game::At<uint32_t>(g_board, kBoardOffFadeIn + kCurveOffStart) - g_clock_start,
                             FadeProgress(g_board),
                             static_cast<uint8_t>(InTransitionList(g_board))};
    if (DebugEnabled() && g_tick == 64)
        DumpTrace("opening");  // for comparing the two computers' starts even when they agree
}

void DumpTrace(const char* why)
{
    Log("lockstep: trace (%s), last 64 ticks up to %u:", why, g_tick);
    for (uint32_t t = g_tick > 64 ? g_tick - 63 : 1; t <= g_tick; ++t) {
        const TraceEntry& e = g_trace[t & 255];
        if (e.tick == t)
            Log("  tick %u rng %u+%u state %d player %d arena %u+%u %u aim %.4f%s cannon %.4f fade %d %.4f%s",
                e.tick, e.mt_calls, e.crt_calls, e.state, e.player, e.allocs, e.reused, e.arena_used, e.aim,
                e.aim_dirty ? "*" : "", e.cannon, static_cast<int>(e.fade_start), e.fade, e.transition ? " T" : "");
    }
}

}  // namespace

void Install()
{
    g_game_thread = GetCurrentThreadId();
    WriteJump(kRandStub, reinterpret_cast<void*>(RandHook));
    WriteJump(kIs3DGetter, reinterpret_cast<void*>(Is3DHook));
    WriteJump(kCrtRand, reinterpret_cast<void*>(CrtRandHook));
    g_progress_tramp = HookInline(kProfileProgress, 6, reinterpret_cast<void*>(ProgressHook));
    g_newest_level_tramp = HookInline(kOnNewestLevel, 6, reinterpret_cast<void*>(NewestLevelHook));
    g_orig_update = reinterpret_cast<UpdateFn>(HookVtable(kBoardVtable, kSlotUpdate, HookBoardUpdate));
    for (const InputSlot& s : kInputSlots) {
        void* hook = nullptr;
        switch (s.slot) {
        case 47: hook = reinterpret_cast<void*>(Input1<47>); break;
        case 48: hook = reinterpret_cast<void*>(Input1<48>); break;
        case 49: hook = reinterpret_cast<void*>(Input1<49>); break;
        case 50: hook = reinterpret_cast<void*>(Input0<50>); break;
        case 51: hook = reinterpret_cast<void*>(Input0<51>); break;
        case 52: hook = reinterpret_cast<void*>(Input2<52>); break;
        case 54: hook = reinterpret_cast<void*>(Input3<54>); break;
        case 56: hook = reinterpret_cast<void*>(Input3<56>); break;
        case 58: hook = reinterpret_cast<void*>(Input2<58>); break;
        case 59: hook = reinterpret_cast<void*>(Input1<59>); break;
        }
        g_orig_input[s.slot] = HookVtable(kBoardVtable, s.slot, hook);
    }
    g_orig_sel_press = reinterpret_cast<ButtonPressFn>(
        HookVtable(kCharDlgButtonVtable, 0, reinterpret_cast<void*>(HookSelPress)));
    g_orig_sel_depress = reinterpret_cast<ButtonDepressFn>(
        HookVtable(kCharDlgButtonVtable, 2, reinterpret_cast<void*>(HookSelDepress)));
    g_orig_sel_checkbox = reinterpret_cast<CheckboxFn>(
        HookVtable(kCharDlgCheckboxVtable, 0, reinterpret_cast<void*>(HookSelCheckbox)));
    g_orig_slot_update = reinterpret_cast<UpdateFn>(
        HookVtable(kSlotMachineVtable, kSlotUpdate, reinterpret_cast<void*>(HookSlotUpdate)));
    g_orig_slot_mouse_down = reinterpret_cast<Input3Fn>(
        HookVtable(kSlotMachineVtable, 54, reinterpret_cast<void*>(HookSlotMouseDown)));
    g_orig_endlevel_depress = reinterpret_cast<ButtonDepressFn>(
        HookVtable(kEndLevelButtonVtable, 2, reinterpret_cast<void*>(HookEndLevelDepress)));
    g_orig_board_button = reinterpret_cast<ButtonDepressFn>(
        HookVtable(kBoardButtonVtable, 2, reinterpret_cast<void*>(HookBoardButton)));
    g_orig_options_button = reinterpret_cast<ButtonDepressFn>(
        HookVtable(kOptionsButtonVtable, 2, reinterpret_cast<void*>(HookOptionsButton)));
    g_pause_trampoline = HookInline(kBoardSetPaused, 7, reinterpret_cast<void*>(PauseHook));
    g_save_trampoline = HookInline(kBoardSaveGame, 5, reinterpret_cast<void*>(SaveHook));
    g_orig_duel_bookkeeping = reinterpret_cast<DuelBookkeepingFn>(
        HookCall(kDuelBookkeepingCall, reinterpret_cast<void*>(HookDuelBookkeeping)));
    g_orig_play_level_music = reinterpret_cast<PlayLevelMusicFn>(
        HookInline(kPlayLevelMusic, 6, reinterpret_cast<void*>(HookPlayLevelMusic)));
    g_orig_set_curve = reinterpret_cast<SetCurveFn>(
        HookInline(kSetCurveFromText, 6, reinterpret_cast<void*>(HookSetCurve)));
    g_orig_build_frame_skin = reinterpret_cast<BuildFrameSkinFn>(
        HookInline(kBuildFrameSkin, 6, reinterpret_cast<void*>(HookBuildFrameSkin)));
    g_orig_build_title_card = HookInline(kBuildTitleCard, 6, reinterpret_cast<void*>(HookBuildTitleCard));
    g_in_transition_fn = HookCall(kBoardInTransitionCall, reinterpret_cast<void*>(HookBoardInTransition));
    g_orig_get_sound_instance = reinterpret_cast<GetSoundInstanceFn>(
        HookVtable(kSoundManagerVtable, kSlotGetSoundInstance, reinterpret_cast<void*>(HookGetSoundInstance)));
    g_orig_start_intro_curves = reinterpret_cast<StartIntroCurvesFn>(
        HookInline(kStartIntroCurves, 5, reinterpret_cast<void*>(HookStartIntroCurves)));
}

uint16_t CurrentFpuControl()
{
    uint16_t cw;
    __asm fnstcw cw
    return cw;
}

void SeedAndRunSynced(uint32_t seed, uint16_t fpu_control, uint32_t clock, void (*fn)(void*), void* arg)
{
    g_tick = 0;
    g_level_seed = seed;
    g_clock_start = clock;
    g_clock_held = true;
    // The opening coin flip avoids long streaks using two globals that persist across duels in a session
    // (who went first last time, and how often in a row). Start both games from a fresh game's values.
    *reinterpret_cast<int*>(kCoinFlipLastFirst) = -1;
    *reinterpret_cast<int*>(kCoinFlipStreak) = 0;
    g_mt.Seed(seed);
    g_crt_state = seed ^ 0x9E3779B9;
    g_crt_calls = 0;
    g_fpu_control = fpu_control;
    g_setup_caller_count = 0;
    g_setup_running = true;
    if (DebugEnabled())
        arena::StartRecording();
    // Building the board reads the input state (the cannon starts out aiming at the mouse): give both
    // computers the same one, the mouse below the cannon and nothing pressed.
    Polled neutral = {};
    neutral.x = 400;
    neutral.y = 300;
    SavedLocal saved;
    OverridePolled(nullptr, neutral, saved);
    {
        SyncScope sync;
        fn(arg);
    }
    RestorePolled(nullptr, saved);
    g_setup_running = false;  // (debug mode keeps recording allocations until RecordTrace stops it)
    char callers[16 * 9 + 1] = "";
    for (int i = 0; i < g_setup_caller_count; ++i)
        wsprintfA(callers + lstrlenA(callers), " %06X", static_cast<unsigned>(g_setup_callers[i]));
    Log("lockstep: synchronized setup done, %u + %u random numbers used (C runtime's from:%s)", g_mt.calls,
        g_crt_calls, callers);
}

void Begin(void* board, bool is_host, const char* host_name, const char* client_name)
{
    g_active = true;
    g_board = board;
    g_last_online_board = board;
    g_is_host = is_host;
    g_local_control = is_host;
    g_duel_started = false;
    g_replaying = false;
    g_tick = 0;
    g_out.clear();
    g_in.clear();
    g_in_pos = 0;
    g_in_ticks = 0;
    g_sent_any = false;
    g_desync_reported = false;
    g_mismatches = 0;
    g_turn_check_state = -1;
    g_turn_mismatches = g_turn_matches = 0;
    g_board_wanted = g_waiting_for_board = g_control_after_board = g_at_checkpoint = g_unanswered = false;
    g_allocators_apart = false;
    g_repairs_in_a_row = 0;
    memset(&g_our_check, 0xFF, sizeof g_our_check);
    g_pointer_in_sync = 0;
    g_host_name = host_name;
    g_client_name = client_name;
    g_stat_time = GetTickCount();
    g_stat_ticks = 0;
    g_watch_unsynced = true;
    g_last_state = -1;
    g_last_sel = false;
    if (DebugEnabled())
        arena::StartFreeRecording();
    Log("lockstep: begin (board %p, %s, fpu %04X)", board, is_host ? "host" : "joiner", g_fpu_control);
}

void End()
{
    if (!g_active)
        return;
    Log("lockstep: end at tick %u", g_tick);
    if (arena::Recording())
        arena::StopRecording(ModDir() + "mods\\logs\\allocs-" + std::to_string(GetCurrentProcessId()) + ".txt");
    if (DebugEnabled())
        arena::StopFreeRecording(ModDir() + "mods\\logs\\frees-" + std::to_string(GetCurrentProcessId()) + ".txt");
    g_active = false;
    g_clock_held = false;  // runs on from the duel's time, so it never goes backwards
    g_watch_unsynced = false;
    g_board = nullptr;
}

bool Active() { return g_active; }
uint32_t CurrentTick() { return g_tick; }

void SetBoardInputFilter(bool (*filter)(int slot)) { g_board_input_filter = filter; }

bool ControllerSkips(void* widget)
{
    if (!g_active)
        return false;
    if (void* dlg = SelectionDialog()) {
        if (game::At<void*>(dlg, game::kDialogOffYesButton) == widget)
            return !g_is_host;  // Play Now
        if (game::At<void*>(widget, kWidgetOffParent) == dlg && ui::IsA(widget, "ButtonWidget")) {
            int id = game::At<int>(widget, kButtonOffId);
            if (id >= 0 && id < 2 * kMastersPerColumn)  // a portrait: our own column, and not Random
                return (id < kMastersPerColumn) != g_is_host || id % kMastersPerColumn == kMasterRandom;
        }
    }
    if (ResultsShowing() && !g_is_host) {  // the host decides what comes next; either player may leave
        void* r = Results();
        if (game::At<void*>(r, game::kDialogOffYesButton) == widget)
            return false;  // Main Menu
        return game::At<void*>(r, game::kDialogOffNoButton) == widget ||
               game::At<void*>(widget, kWidgetOffParent) == r;
    }
    return false;
}

bool TakeDesyncNotice()
{
    bool n = g_desync_notice;
    g_desync_notice = false;
    return n;
}
void SetNewLevelHandler(void (*fn)()) { g_on_new_level = fn; }

std::string DebugState()
{
    if (!g_active)
        return "inactive";
    char buf[320];
    void* lm = LogicMgr(g_board);
    wsprintfA(buf,
              "tick=%u control=%d state=%d player=%d balls=%d,%d enddlg=%d seldlg=%d desync=%d hold=%d inpos=%u "
              "insize=%u inticks=%d updates=%u stop=%s",
              g_tick, g_local_control ? 1 : 0, game::At<int>(lm, kLogicOffState), CurrentPlayer(g_board),
              game::At<int>(lm, kLogicOffBalls), game::At<int>(lm, kLogicOffBalls + 4), ResultsShowing() ? 1 : 0,
              game::GetDialog(kCharDlgId) ? 1 : 0, g_desync_reported ? 1 : 0, static_cast<int>(g_hold_at),
              static_cast<unsigned>(g_in_pos), static_cast<unsigned>(g_in.size()), g_in_ticks, g_board_updates,
              g_follow_stop);
    return buf;
}
void HoldAt(uint32_t tick) { g_hold_at = tick; }

bool DumpBoard(const std::string& path)
{
    if (!g_active)
        return false;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f)
        return false;
    struct Part {
        const void* p;
        size_t n;
    } parts[] = {{g_board, 0x900}, {LogicMgr(g_board), 0x600}, {game::At<void*>(g_board, kBoardOffCannon), 0x200}};
    for (const Part& part : parts) {
        uint32_t n = part.p ? static_cast<uint32_t>(part.n) : 0;
        fwrite(&n, 4, 1, f);
        if (n)
            fwrite(part.p, 1, n, f);
    }
    fclose(f);
    return true;
}

void Update()
{
    if (!g_active)
        return;
    if (game::At<void*>(game::App(), game::kAppOffBoard) != g_board) {
        End();
        return;
    }
    Flush();
    ApplyNames();
    HideOfflineControls();  // before the frame is drawn, so a new selection dialog never shows them
    DWORD now = GetTickCount();
    if (now - g_stat_time >= 5000) {
        arena::Stats a = arena::GetStats();
        Log("lockstep: %d ticks in %lums, tick %u, %s, backlog %d, rng %u+%u, state %d, player %d, arena %u "
            "allocs %u reused %uKB leaked to %08X",
            g_stat_ticks, now - g_stat_time, g_tick, g_local_control ? "in control" : "following", g_in_ticks,
            g_mt.calls, g_crt_calls, game::At<int>(LogicMgr(g_board), kLogicOffState), CurrentPlayer(g_board) + 1,
            static_cast<unsigned>(a.allocated), static_cast<unsigned>(a.reused),
            static_cast<unsigned>(a.leaked_bytes >> 10), static_cast<unsigned>(a.used_to));
        if (g_mismatches)
            Log("lockstep: %u ticks differed from the opponent's", g_mismatches);
        g_mismatches = 0;
        g_stat_time = now;
        g_stat_ticks = 0;
    }
}

bool HandleMessage(const net::Message& m)
{
    if (m.type == proto::kStream) {
        if (!g_active)
            return true;
        g_in.insert(g_in.end(), m.data.begin(), m.data.end());
        g_in_ticks += CountTicks(m.data.data(), m.data.size());
        return true;
    }
    if (m.type == proto::kTurnReply) {
        proto::TurnReply r;
        if (g_active && m.data.size() >= sizeof r) {
            memcpy(&r, m.data.data(), sizeof r);
            OnTurnReply(r);
        }
        return true;
    }
    if (m.type == proto::kDesync) {
        uint32_t t = 0;
        if (m.data.size() >= 4)
            memcpy(&t, m.data.data(), 4);
        Log("lockstep: opponent found a mismatch at tick %u", t);
        if (g_active)
            DumpTrace("opponent's report");
        return true;
    }
    if (m.type == proto::kSelect) {
        proto::Select s;
        if (g_active && m.data.size() >= sizeof s) {
            memcpy(&s, m.data.data(), sizeof s);
            if (void* dlg = SelectionDialog())
                SetSelectedMaster(dlg, s.player & 1, s.master < kMasterRandom ? s.master : 0);
        }
        return true;
    }
    return false;
}

}  // namespace lockstep
