#include "replays.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "chaos.h"
#include "chaos_effects.h"
#include "chat.h"
#include "game.h"
#include "hook.h"
#include "log.h"
#include "online.h"
#include "ui.h"

namespace replays {

namespace {

using chaos::ShotState;

// ---- The game's replays (see re/NOTES.md, section 9) ----
constexpr int kBoardOffShots = 0x744;      // std::deque<DataWriter*>: the shots recorded, oldest first (49 at most)
constexpr int kBoardOffShotCount = 0x754;
constexpr int kBoardOffLiveCopy = 0x758;   // DataWriter*: the board as a replay began, put back when it's done
constexpr int kBoardOffScore = 0x6E0;      // goes in a replay file's header
constexpr int kBoardOffLogic = 0x720;
constexpr int kBoardOffReplayButton = 0x738;
constexpr int kLogicOffState = 0x04, kLogicOffReplaying = 0x101;
constexpr int kStateBallInPlay = 2;
constexpr int kWriterOffData = 0x08, kWriterOffSize = 0x0C;  // DataWriter: what it holds
constexpr int kBufferOffData = 0x08, kBufferOffBits = 0x14;  // Buffer (a file read in): its bytes, its size in bits
constexpr uintptr_t kShotAt = 0x43DC20;  // ESI deque, stdcall(index) -> the element's address
constexpr uintptr_t kRecordCall = 0x46C3EB;  // call 0x405F10: a shot's copy as it's fired (EAX board)
constexpr uintptr_t kLoadCalls[] = {0x4061EE, 0x406092};  // call 0x405DB0: ECX copy, EDX board, stdcall(bool) -> AL
constexpr uintptr_t kParseCalls[] = {0x406CE1, 0x42E90D};  // call 0x406D10: ECX Buffer (a file), stdcall(board) -> AL
constexpr uintptr_t kWriteFileCall = 0x4C8205;  // call 0x4C7D70: EBX dialog, stdcall(const std::string* path)
constexpr uintptr_t kDataFolder = 0x6C7958;     // std::string: where userdata\ is
constexpr uint32_t kFileVersion = 0x52;
constexpr uint32_t kTrailerMagic = 0x53524E50;  // "PNRS", the last four bytes of a file with our states

void* Logic(void* board) { return board ? game::At<void*>(board, kBoardOffLogic) : nullptr; }

int ShotCount(void* board) { return board ? game::At<int>(board, kBoardOffShotCount) : 0; }

void* ShotCopy(void* board, int i)
{
    void* deque = static_cast<char*>(board) + kBoardOffShots;
    void** slot;
    __asm {
        push i
        mov esi, deque
        mov eax, kShotAt
        call eax
        mov slot, eax
    }
    return slot ? *slot : nullptr;
}

ShotState EmptyShot()
{
    ShotState s;
    memset(&s, 0, sizeof s);
    s.size = sizeof s;
    s.red.index = s.bomb.index = -1;
    for (chaos::ShotPeg& p : s.fake_pegs)
        p.index = -1;  // (what a file from before them reads as)
    return s;
}

// ---- The effects' state of each recorded shot, by the game's copy of it ----
struct Entry {
    void* copy;
    ShotState state;
    bool traced;  // the shot is over: its trace is in
};
constexpr int kMaxEntries = 64;  // (fixed: recorded inside the simulation online, where nothing may differ)
Entry g_entries[kMaxEntries];
int g_entry_count;
void* g_entries_board;

Entry* Find(void* copy)
{
    for (int i = 0; i < g_entry_count; ++i)
        if (g_entries[i].copy == copy)
            return &g_entries[i];
    return nullptr;
}

// Entries whose copy the board no longer has (dropped as the 50th came, or a file loaded instead) go.
void Prune(void* board)
{
    if (board != g_entries_board) {
        g_entry_count = 0;
        g_entries_board = board;
    }
    int n = ShotCount(board), kept = 0;
    for (int i = 0; i < g_entry_count; ++i) {
        bool there = false;
        for (int j = 0; j < n && !there; ++j)
            there = ShotCopy(board, j) == g_entries[i].copy;
        if (there)
            g_entries[kept++] = g_entries[i];
    }
    g_entry_count = kept;
}

void Add(void* board, void* copy, const ShotState& s, bool traced)
{
    Prune(board);
    if (Entry* e = Find(copy)) {
        e->state = s;
        e->traced = traced;
        return;
    }
    if (g_entry_count == kMaxEntries) {
        std::copy(g_entries + 1, g_entries + kMaxEntries, g_entries);
        --g_entry_count;
    }
    g_entries[g_entry_count++] = {copy, s, traced};
}

// ---- Recording, and playing a replay ----
void* g_tracing;  // the shot in flight whose trace isn't in yet

void __cdecl OnShotRecorded(void* board)  // (inside the simulation online)
{
    int n = ShotCount(board);
    void* copy = n ? ShotCopy(board, n - 1) : nullptr;
    if (!copy)
        return;
    ShotState s;
    chaos::CaptureShot(board, s);
    Add(board, copy, s, false);
    chaos::ResetTrace();
    g_tracing = copy;
}

uintptr_t g_orig_record;

__declspec(naked) void RecordHook()  // replaces a call: EAX board
{
    __asm {
        push eax
        call dword ptr [g_orig_record]
        call OnShotRecorded
        add esp, 4
        ret
    }
}

ShotState g_live;      // the effects as the replays began, put back with the board
bool g_in_replay;
void* g_replay_board;
Entry* g_replay_entry;
void* g_replay_copy;
bool g_replay_tracing;
uint32_t g_replay_trace, g_replay_steps;
int g_replays, g_replays_traced;  // testing: how many played, and finished

void* g_load_copy;
void* g_load_board;

void __cdecl OnBeforeLoad()
{
    void* board = g_load_board;
    if (!board || g_load_copy == game::At<void*>(board, kBoardOffLiveCopy))
        return;  // the board as it was comes back
    if (!g_in_replay || g_replay_board != board) {
        chaos::CaptureShot(board, g_live);  // (before the load: its pegs are about to be replaced)
        g_in_replay = true;
        g_replay_board = board;
    }
}

void __cdecl OnAfterLoad(int ok)
{
    void* board = g_load_board;
    if (!board)
        return;
    if (g_load_copy == game::At<void*>(board, kBoardOffLiveCopy)) {
        if (g_in_replay && g_replay_board == board)
            chaos::ApplyShot(board, g_live);
        g_in_replay = false;
        g_replay_board = nullptr;
        g_replay_entry = nullptr;
        g_replay_tracing = false;
        Log("replays: back from the replays");
    } else {
        Entry* e = Find(g_load_copy);
        ShotState s = e ? e->state : EmptyShot();
        chaos::ApplyShot(board, s);
        g_replay_entry = e;
        g_replay_copy = g_load_copy;
        g_replay_tracing = true;
        g_replay_trace = g_replay_steps = 0;
        chaos::ResetTrace();
        ++g_replays;
        Log("replays: playing a shot (%s, effects %X, loaded %d)", e ? "its effects kept" : "none kept", s.active,
            ok & 0xFF);
    }
    chaos::BoardReloaded(board);
}

uintptr_t g_orig_load;

__declspec(naked) void LoadHook()  // replaces a call: ECX copy, EDX board, stdcall(bool with the shot's inputs) -> AL
{
    __asm {
        mov g_load_copy, ecx
        mov g_load_board, edx
        pushad
        call OnBeforeLoad
        popad
        push dword ptr [esp + 4]
        call dword ptr [g_orig_load]
        pushad
        movzx eax, al
        push eax
        call OnAfterLoad
        add esp, 4
        popad
        ret 4
    }
}

// The trace of a shot (where its balls went) once it's over, the original's and a replay's (testing).
void FinishTraces(void* board)
{
    void* lm = Logic(board);
    if (!lm || game::At<int>(lm, kLogicOffState) == kStateBallInPlay || !chaos::TraceSteps())
        return;
    bool replaying = game::At<uint8_t>(lm, kLogicOffReplaying) != 0;
    if (g_tracing && !replaying) {
        if (Entry* e = Find(g_tracing)) {
            e->state.trace = chaos::Trace();
            e->state.trace_steps = chaos::TraceSteps();
            e->traced = true;
        }
        g_tracing = nullptr;
    }
    if (g_replay_tracing && replaying) {
        g_replay_tracing = false;
        g_replay_trace = chaos::Trace();
        g_replay_steps = chaos::TraceSteps();
        ++g_replays_traced;
        bool known = g_replay_entry && g_replay_entry->traced;
        Log("replays: the replay's balls went %s (%u steps; the shot's %u)",
            !known ? "somewhere (no trace of the shot)"
                   : g_replay_trace == g_replay_entry->state.trace ? "where the shot's did" : "elsewhere",
            g_replay_steps, known ? g_replay_entry->state.trace_steps : 0);
    }
}

// ---- Replay files: the game's data, then our states (one per shot, in the same order) ----
uint32_t Get32(const uint8_t* p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

void Put32(std::string& out, uint32_t v) { out.append(reinterpret_cast<const char*>(&v), 4); }

struct Span {
    size_t offset, size;
};

// The shots in a replay file (as the game reads it: 0x406D10).
bool ParseShots(const uint8_t* d, size_t n, std::vector<Span>& shots)
{
    if (n < 4)
        return false;
    uint32_t version = Get32(d);
    size_t p = 4;
    if (version >= 0x46)
        p += 4;
    if (version < 0x32) {
        shots.push_back({p, n - p});
        return n >= p;
    }
    if (n < p + 2)
        return false;
    uint16_t count;
    memcpy(&count, d + p, 2);
    p += 2;
    for (int i = 0; i < count; ++i) {
        if (n < p + 4)
            return false;
        uint32_t size = Get32(d + p);
        p += 4;
        if (n - p < size)
            return false;
        shots.push_back({p, size});
        p += size;
    }
    return true;
}

// Footer: count, the size of a state, the trailer's whole size, magic.
std::string Trailer(const std::vector<ShotState>& states)
{
    std::string t;
    for (const ShotState& s : states)
        t.append(reinterpret_cast<const char*>(&s), sizeof s);
    Put32(t, static_cast<uint32_t>(states.size()));
    Put32(t, sizeof(ShotState));
    Put32(t, static_cast<uint32_t>(states.size() * sizeof(ShotState) + 16));
    Put32(t, kTrailerMagic);
    return t;
}

bool ReadTrailer(const uint8_t* d, size_t n, std::vector<ShotState>& states)
{
    if (n < 16 || Get32(d + n - 4) != kTrailerMagic)
        return false;
    uint32_t total = Get32(d + n - 8), size = Get32(d + n - 12), count = Get32(d + n - 16);
    if (size == 0 || size > 0x10000 || count > 100 || total != count * size + 16 || total > n)
        return false;
    const uint8_t* p = d + n - total;
    for (uint32_t i = 0; i < count; ++i) {
        ShotState s = EmptyShot();
        memcpy(&s, p + i * size, std::min<size_t>(size, sizeof s));  // (an older build's: shorter, the rest empty)
        s.size = sizeof s;
        states.push_back(s);
    }
    return true;
}

bool ReadAll(const std::string& path, std::string& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    char buf[65536];
    size_t got;
    out.clear();
    while ((got = fread(buf, 1, sizeof buf, f)) > 0)
        out.append(buf, got);
    fclose(f);
    return true;
}

// A replay file loaded into a board (the board's shots are now exactly the file's).
void* g_parse_buffer;
void* g_parse_board;
int g_files_read, g_files_with_states;  // testing

void __cdecl OnFileParsed(int ok)
{
    void* board = g_parse_board;
    if (!(ok & 0xFF) || !board || !g_parse_buffer)
        return;
    const uint8_t* d = game::At<const uint8_t*>(g_parse_buffer, kBufferOffData);
    size_t n = (static_cast<size_t>(game::At<int>(g_parse_buffer, kBufferOffBits)) + 7) / 8;
    std::vector<ShotState> states;
    bool have = d && ReadTrailer(d, n, states);
    int shots = ShotCount(board);
    for (int i = 0; i < shots; ++i)
        Add(board, ShotCopy(board, i), i < static_cast<int>(states.size()) ? states[i] : EmptyShot(),
            i < static_cast<int>(states.size()));
    ++g_files_read;
    g_files_with_states += have;
    Log("replays: a replay file with %d shots, %s", shots, have ? "and the effects of each" : "no effects kept");
}

uintptr_t g_orig_parse;

__declspec(naked) void ParseHook()  // replaces a call: ECX Buffer, stdcall(board) -> AL
{
    __asm {
        mov g_parse_buffer, ecx
        mov eax, [esp + 4]
        mov g_parse_board, eax
        push eax
        call dword ptr [g_orig_parse]
        pushad
        movzx eax, al
        push eax
        call OnFileParsed
        add esp, 4
        popad
        ret 4
    }
}

// The game saved a replay file (Save in the replay panel): our states go after its data, matched to its shots by
// their bytes.
const void* g_write_path;
std::string g_last_saved;
int g_saves;

void __cdecl OnFileWritten()
{
    std::string path = g_write_path ? game::ReadString(g_write_path) : std::string();
    void* board = game::App() ? game::At<void*>(game::App(), game::kAppOffBoard) : nullptr;
    std::string data;
    std::vector<Span> shots;
    std::vector<ShotState> had;
    if (path.empty() || !board || !ReadAll(path, data) || data.empty() ||
        !ParseShots(reinterpret_cast<const uint8_t*>(data.data()), data.size(), shots) ||
        ReadTrailer(reinterpret_cast<const uint8_t*>(data.data()), data.size(), had))
        return;
    std::vector<ShotState> states;
    int n = ShotCount(board);
    for (const Span& s : shots) {
        ShotState st = EmptyShot();
        for (int i = 0; i < n; ++i) {
            void* copy = ShotCopy(board, i);
            if (copy && game::At<uint32_t>(copy, kWriterOffSize) == s.size &&
                memcmp(game::At<const void*>(copy, kWriterOffData), data.data() + s.offset, s.size) == 0) {
                if (Entry* e = Find(copy))
                    st = e->state;
                break;
            }
        }
        states.push_back(st);
    }
    FILE* f = fopen(path.c_str(), "ab");
    if (!f)
        return;
    std::string t = Trailer(states);
    fwrite(t.data(), 1, t.size(), f);
    fclose(f);
    g_last_saved = path;
    ++g_saves;
    Log("replays: saved %s with the effects of its %d shots", path.c_str(), static_cast<int>(shots.size()));
}

uintptr_t g_orig_write_file;

__declspec(naked) void WriteFileHook()  // replaces a call: EBX dialog, stdcall(const std::string* path)
{
    __asm {
        mov eax, [esp + 4]
        mov g_write_path, eax
        push eax
        call dword ptr [g_orig_write_file]
        pushad
        call OnFileWritten
        popad
        ret 4
    }
}

// ---- Online: SAVE in Instant Replay's place ----
// The game's own Save isn't used: with a ball in flight it adds the shot's recording to the copy on this computer
// only. The file is written from the recorded copy (only read), after the shot in flight is over.
constexpr int kSaveButtonId = 9105;
constexpr int kZOrder = 1001;  // as the CHAT button (above the chat box's whole-screen layer)
const ui::Color kLabel = {235, 85, 220, 255}, kLabelLit = {255, 200, 250, 255};

void* g_save_button;
bool g_save_pending;

std::string SafeName(std::string s)
{
    for (char& c : s)
        if (strchr("\\/:*?\"<>|", c) || static_cast<unsigned char>(c) < 32)
            c = '_';
    return s;
}

bool Exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

void SaveLatest(void* board)
{
    int n = ShotCount(board);
    void* copy = n ? ShotCopy(board, n - 1) : nullptr;
    if (!copy) {
        chat::Note("No shot to save yet.");
        return;
    }
    Entry* e = Find(copy);
    ShotState s = e ? e->state : EmptyShot();
    std::string folder = game::ReadString(reinterpret_cast<const void*>(kDataFolder)) + "userdata\\";
    CreateDirectoryA(folder.c_str(), nullptr);
    folder += "replays\\";
    CreateDirectoryA(folder.c_str(), nullptr);
    std::string base = SafeName(online::PlayerName(s.shooter) + "'s shot"), name = base;
    for (int i = 2; Exists(folder + name + ".pegn") && i < 1000; ++i)
        name = base + " " + std::to_string(i);
    std::string path = folder + name + ".pegn", data;
    Put32(data, kFileVersion);
    Put32(data, game::At<uint32_t>(board, kBoardOffScore));
    uint16_t one = 1;
    data.append(reinterpret_cast<const char*>(&one), 2);
    uint32_t size = game::At<uint32_t>(copy, kWriterOffSize);
    Put32(data, size);
    data.append(game::At<const char*>(copy, kWriterOffData), size);
    data += Trailer({s});
    FILE* f = fopen(path.c_str(), "wb");
    bool written = f && fwrite(data.data(), 1, data.size(), f) == data.size();
    if (f)
        fclose(f);
    if (!written) {
        chat::Note("Couldn't save the replay.");
        Log("replays: couldn't write %s", path.c_str());
        return;
    }
    g_last_saved = path;
    ++g_saves;
    chat::Note("Replay saved: " + name);
    ui::PlaySound(ui::kSoundButton);
    Log("replays: saved the last shot (%s's, effects %X) as %s", online::PlayerName(s.shooter).c_str(), s.active,
        path.c_str());
}

void RequestSave()
{
    void* board = game::App() ? game::At<void*>(game::App(), game::kAppOffBoard) : nullptr;
    void* lm = Logic(board);
    if (!lm)
        return;
    if (game::At<int>(lm, kLogicOffState) == kStateBallInPlay) {
        g_save_pending = true;  // once the shot is over (and the game has added where its balls went)
        Log("replays: SAVE pressed with the ball in flight: after the shot");
        return;
    }
    SaveLatest(board);
}

struct Listener {
    void* const* vtable;
};
void __fastcall LPress2(Listener*, void*, int, int) {}
void __fastcall LId(Listener*, void*, int) {}
void __fastcall LDepress(Listener*, void*, int) { RequestSave(); }
void __fastcall LMove(Listener*, void*, int, int, int) {}
void* const g_listener_vtable[7] = {reinterpret_cast<void*>(LPress2), reinterpret_cast<void*>(LId),
                                    reinterpret_cast<void*>(LDepress), reinterpret_cast<void*>(LId),
                                    reinterpret_cast<void*>(LId),     reinterpret_cast<void*>(LId),
                                    reinterpret_cast<void*>(LMove)};
Listener g_listener = {g_listener_vtable};

// A round button like CHAT and Menu, over the board's Instant Replay button (the same size).
void AttachSave(void* board)
{
    void* image = ui::Resource<void>(ui::kImagePurpleButton);
    void* replay = game::At<void*>(board, kBoardOffReplayButton);
    if (!image || game::At<int>(image, game::kImageOffRows) != 3 || !replay)
        return;
    void* b = ui::NewImageButton(kSaveButtonId, &g_listener, image);
    int w = game::At<int>(image, game::kImageOffWidth), h = game::At<int>(image, game::kImageOffHeight) / 3;
    auto* cells = reinterpret_cast<ui::Rect*>(static_cast<char*>(b) + game::kButtonOffRects);
    cells[0] = {0, 0, w, h};
    cells[1] = {0, h, w, h};
    cells[2] = {0, 2 * h, w, h};
    cells[3] = {0, 0, w, h};
    game::AssignString(static_cast<char*>(b) + game::kButtonOffLabel, "SAVE");
    game::At<void*>(b, game::kButtonOffFont) = ui::Resource<void>(ui::kFontOverload12);
    auto* colors = game::At<ui::Color*>(b, game::kWidgetOffColors);
    colors[0] = kLabel;
    colors[1] = kLabelLit;
    ui::Rect r = ui::ScreenRect(replay);
    ui::Resize(b, r.x + r.w / 2 - w / 2, r.y + r.h / 2 - h / 2, w, h);
    ui::AddToScreen(b, kZOrder);
    g_save_button = b;
    Log("replays: SAVE on screen at %d,%d", r.x + r.w / 2, r.y + r.h / 2);
}

void DetachSave()
{
    ui::RemoveFromScreen(g_save_button);
    ui::Delete(g_save_button);
    g_save_button = nullptr;
}

}  // namespace

void Install()
{
    g_orig_record = HookCall(kRecordCall, reinterpret_cast<void*>(RecordHook));
    for (uintptr_t at : kLoadCalls)
        g_orig_load = HookCall(at, reinterpret_cast<void*>(LoadHook));
    for (uintptr_t at : kParseCalls)
        g_orig_parse = HookCall(at, reinterpret_cast<void*>(ParseHook));
    g_orig_write_file = HookCall(kWriteFileCall, reinterpret_cast<void*>(WriteFileHook));
}

void Update()
{
    void* board = game::App() ? game::At<void*>(game::App(), game::kAppOffBoard) : nullptr;
    if (g_entries_board && board != g_entries_board) {
        g_entry_count = 0;  // (its shots went with it)
        g_entries_board = nullptr;
        g_tracing = nullptr;
    }
    if (g_in_replay && board != g_replay_board) {
        g_in_replay = false;
        g_replay_board = nullptr;
        g_replay_entry = nullptr;
        g_replay_tracing = false;
    }
    if (board)
        FinishTraces(board);
    bool online_board = board && chat::Available();
    if (online_board && !g_save_button)
        AttachSave(board);
    else if (!online_board && g_save_button)
        DetachSave();
    if (!online_board)
        g_save_pending = false;
    if (g_save_pending && Logic(board) && game::At<int>(Logic(board), kLogicOffState) != kStateBallInPlay) {
        g_save_pending = false;
        SaveLatest(board);
    }
}

void* SaveButton() { return g_save_button && ui::Shown(g_save_button) ? g_save_button : nullptr; }

// replay: the state. replay save: press SAVE (online). replay button: where Instant Replay is.
std::string DebugCommand(const std::string& args)
{
    void* board = game::App() ? game::At<void*>(game::App(), game::kAppOffBoard) : nullptr;
    if (args == "save") {
        if (!g_save_button)
            return "error: no SAVE button";
        RequestSave();
        return "ok";
    }
    if (args == "button") {
        void* b = board ? game::At<void*>(board, kBoardOffReplayButton) : nullptr;
        if (!b)
            return "none";
        ui::Rect r = ui::ScreenRect(b);
        char buf[96];
        snprintf(buf, sizeof buf, "%d %d %d %d shown=%d enabled=%d", r.x, r.y, r.w, r.h, ui::Shown(b) ? 1 : 0,
                 game::At<uint8_t>(b, 0x56) ? 0 : 1);
        return buf;
    }
    Entry* e = g_replay_entry;
    bool known = e && e->traced;
    ui::Rect save = g_save_button ? ui::ScreenRect(g_save_button) : ui::Rect{-1, -1, 0, 0};
    int n = ShotCount(board);
    Entry* latest = n ? Find(ShotCopy(board, n - 1)) : nullptr;
    char buf[700];
    snprintf(buf, sizeof buf,
             "entries=%d shots=%d latestshooter=%d latesteffects=%X inreplay=%d replays=%d finished=%d "
             "replayeffects=%X trace=%08X steps=%u original=%08X originalsteps=%u match=%d tracing=%d filesread=%d "
             "fileswithstates=%d saves=%d pending=%d save=%d,%d lastsaved=",
             g_entry_count, n, latest ? latest->state.shooter : -1, latest ? latest->state.active : 0,
             g_in_replay ? 1 : 0, g_replays, g_replays_traced,
             e ? e->state.active : 0, g_replay_trace, g_replay_steps, known ? e->state.trace : 0,
             known ? e->state.trace_steps : 0, known && g_replay_trace == e->state.trace &&
             g_replay_steps == e->state.trace_steps ? 1 : 0, g_replay_tracing ? 1 : 0, g_files_read,
             g_files_with_states, g_saves, g_save_pending ? 1 : 0, save.x + save.w / 2, save.y + save.h / 2);
    return buf + g_last_saved;
}

}  // namespace replays
