#include "controls.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <vector>

#include "chat.h"
#include "config.h"
#include "game.h"
#include "hook.h"
#include "keyboard.h"
#include "lockstep.h"
#include "log.h"
#include "online.h"
#include "pad.h"
#include "padmenu.h"
#include "replays.h"
#include "ui.h"

namespace controls {

namespace {

// ---- Game addresses ----
constexpr int kAppSlotEnforceCursor = 12;  // SexyAppBase: set the mouse pointer for what is under it
constexpr int kAppOffCursor = 0x4B0;       // the pointer it wants (11: none)
constexpr int kCursorNone = 11;
constexpr int kBoardOffLogic = 0x720, kLogicOffState = 0x04, kStateAiming = 1;
constexpr int kLogicOffTwoPlayers = 0x1A8, kLogicOffCurrentPlayer = 0x1AC, kLogicOffComputer = 0x2D8;  // bool[2]
constexpr int kBoardOffMenuButton = 0x734, kBoardOffReplayButton = 0x738;
constexpr int kBoardOffReplayPanel = 0x728;  // ReplayDialog: on screen while an Instant Replay plays
constexpr int kLogicOffReplaying = 0x101;
constexpr int kReplayAgain = 0, kReplayDone = 4;  // its buttons (1 Next, 2 Prev: saved replays; 3 Save)
constexpr int kBoardOffSlotMachine = 0x730;  // Lucky Spin, on screen while it spins
// Two Player Selection (dialog 1, over a duel's board): portraits are buttons 0-11 for player 1, 12-23 for player 2.
constexpr int kSelectionDialogId = 1;
constexpr int kSelectionOffEditName2 = 0x194, kSelectionOffPlayFriend = 0x198;  // player 2's EDIT; a ThunderCheckbox
constexpr int kSelectionOffMasters = 0x22C;  // per player: std::vector of 12-byte entries, `selected` at +4
constexpr int kMastersPerColumn = 12;
constexpr int kCheckboxOffChecked = 0x90;
constexpr int kBoardSlotMouseEnter = 50, kBoardSlotMouseMove = 52, kBoardSlotMouseDrag = 58;
constexpr int kWidgetOffManager = 0x10, kWidgetOffVisible = 0x54, kWidgetOffDisabled = 0x56;
constexpr int kButtonOffId = 0x88;
constexpr int kWidgetOffParent = 0x14;
constexpr int kSliderOffListener = 0x88, kSliderOffValue = 0x90, kSliderOffId = 0x98;  // value: double, 0..1
constexpr int kListOffLines = 0xA0;     // std::vector<std::string>: begin, end
constexpr int kListOffPosition = 0xC0;  // double: the first line shown
constexpr int kListOffItemHeight = 0xEC;
constexpr int kListTopPad = 4;
constexpr int kStringSize = 0x1C;

constexpr int kScreenW = 800, kScreenH = 600;
// While the controller plays the board, the pointer waits here: on the board itself, away from the corner
// buttons (a click over them doesn't shoot).
constexpr int kParkX = 400, kParkY = 300;

// Aiming. The left stick turns the cannon in steps of a tenth of a degree, like the game's own wheel steps of
// two or more; the right stick and the D-pad send single notches, which the game makes finer the slower they come.
const float kAimDegreesPerSecond[config::kAimSpeedCount] = {45, 90, 160};
constexpr float kFineRateMin = 4, kFineRateMax = 50;  // notches a second, right stick from barely to fully pushed
constexpr DWORD kDpadFineAfter = 300, kDpadFastAfter = 1200;
constexpr float kDpadFineRate = 20, kDpadFastRate = 60;
constexpr int kWheelRight = 1;  // the game's right arrow key

// Moving between buttons repeats while the D-pad or stick is held.
constexpr DWORD kRepeatDelay = 380, kRepeatEvery = 110;
constexpr float kStickPush = 0.55f, kStickRelease = 0.35f;
constexpr double kSliderStep = 0.05;

enum Dir { kNoDir = -1, kDirUp, kDirDown, kDirLeft, kDirRight };

struct Target {
    void* widget;
    int row;  // a line of a list (or of the chat phrases), else -1
    ui::Rect rect;
    int cx() const { return rect.x + rect.w / 2; }
    int cy() const { return rect.y + rect.h / 2; }
};

// A mouse button the controller is holding down, and where.
struct Held {
    bool down;
    int x, y, clicks;
};

using EnforceCursorFn = void(__thiscall*)(void*);
EnforceCursorFn g_orig_enforce_cursor;
bool g_pointer_hidden;

LARGE_INTEGER g_freq, g_last_frame;
float g_dt;  // seconds since the last frame

// Moving a highlight: on press, then repeating while the D-pad or stick is held.
struct Repeat {
    int dir = kNoDir;
    DWORD next = 0;
    int stick = kNoDir;  // the stick's direction, with some give before it lets go
};
Repeat g_nav;            // the highlight
Repeat g_player_nav[2];  // each player's own (a local duel's master selection)

const char* g_context = "mouse";
void* g_focus_widget;
int g_focus_row = -1;
bool g_focus_auto;  // the highlight is where the screen starts it, not where the player put it
int g_focus_x, g_focus_y;
std::map<void*, std::pair<void*, int>> g_layer_focus;  // the highlight each dialog or screen had last
int g_target_count;
bool g_chat_was_open;
int g_chat_row;

Held g_left = {false, 0, 0, 1}, g_right = {false, 0, 0, -1}, g_menu_press = {false, 0, 0, 1};

// Sliders, checkboxes and text boxes look the same with the pointer over them, so the highlight on one of them
// is a frame of our own, in the yellow of a highlighted button's lettering. So are the two players' own
// highlights on a local duel's master selection (player 2's is blue), since there is only one pointer.
constexpr int kFrameZOrder = 999;  // above the game's dialogs, below the chat
const ui::Color kFrameColor[2] = {{255, 230, 90, 255}, {110, 200, 255, 255}};
const ui::Color kFrameGlow[2] = {{255, 230, 90, 110}, {110, 200, 255, 110}};
ui::WidgetClass g_frame_class;
void* g_frame;
bool g_frame_on[2];
ui::Rect g_frame_rect[2];
bool g_framed;  // a frame was asked for this frame
float g_coarse_aim, g_fine_aim;
DWORD g_dpad_since;
int g_wheel_steps;  // testing: wheel steps sent to the board

// A local duel with a controller each.
void* g_player_focus[2];
void* g_replay_focus;
void* g_turn_board;     // the board whose turn ...
int g_turn_controller;  // ... this controller plays (kept through an Instant Replay of the other player's shot)
void* g_buzz_board;
int g_buzz_player = -1;
bool g_was_aiming;
int g_buzzes, g_buzz_controller = -1;  // testing

bool ActionDown(int action) { return (pad::DownBits() & config::PadBinding(action)) != 0; }
bool ActionPressed(int action) { return (pad::PressedBits() & config::PadBinding(action)) != 0; }

// Mouse input for the game, delivered at the very end of the frame: a click can open one of the game's dialogs
// that runs its own loop until it's closed, and that loop runs Update again (so the controller works in it).
std::vector<std::function<void()>> g_input;

void MoveLater(int x, int y)
{
    g_input.push_back([=] { ui::MouseMove(x, y); });
}

void DeliverInput()
{
    std::vector<std::function<void()>> now;
    now.swap(g_input);
    for (const auto& f : now)
        f();
}

void Press(Held& h, bool down, int x, int y)
{
    if (down && !h.down) {
        h.down = true;
        h.x = x;
        h.y = y;
        int clicks = h.clicks;
        g_input.push_back([=] { ui::MouseDown(x, y, clicks); });
    } else if (!down && h.down) {
        h.down = false;
        int px = h.x, py = h.y, clicks = h.clicks;
        g_input.push_back([=] { ui::MouseUp(px, py, clicks); });
    }
}

void ReleaseBoard()
{
    Press(g_left, false, 0, 0);
    Press(g_right, false, 0, 0);
}

void ReleaseAll()
{
    ReleaseBoard();
    Press(g_menu_press, false, 0, 0);
}

void Click(int x, int y)
{
    g_input.push_back([=] {
        ui::MouseMove(x, y);
        ui::MouseDown(x, y, 1);
        ui::MouseUp(x, y, 1);
    });
}

// ---- The highlight frame ----
void __fastcall FrameDraw(void*, void*, void* g)
{
    for (int p = 0; p < 2; ++p) {
        if (!g_frame_on[p])
            continue;
        const ui::Rect& r = g_frame_rect[p];
        for (int ring = 0; ring < 2; ++ring) {
            int x = r.x - 3 - ring * 2, y = r.y - 3 - ring * 2, w = r.w + 6 + ring * 4, h = r.h + 6 + ring * 4;
            ui::SetColor(g, ring ? kFrameGlow[p] : kFrameColor[p]);
            ui::FillRect(g, x + 2, y, w - 4, 2);
            ui::FillRect(g, x + 2, y + h - 2, w - 4, 2);
            ui::FillRect(g, x, y + 2, 2, h - 4);
            ui::FillRect(g, x + w - 2, y + 2, 2, h - 4);
        }
    }
}

bool NeedsFrame(const void* w)
{
    return w && (ui::IsA(w, "Slider") || ui::IsA(w, "Checkbox") || ui::IsA(w, "EditWidget"));
}

bool SameRect(const ui::Rect& a, const ui::Rect& b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }

// The highlight frames: `r` the usual one (or player 1's), `r2` player 2's; null for none.
void ShowFrame(const ui::Rect* r, const ui::Rect* r2 = nullptr)
{
    if (!r && !r2) {
        if (g_frame) {
            ui::RemoveFromScreen(g_frame);
            ui::Delete(g_frame);
            g_frame = nullptr;
        }
        return;
    }
    g_framed = true;
    const ui::Rect* want[2] = {r, r2};
    bool changed = !g_frame;
    for (int p = 0; p < 2; ++p)
        changed |= g_frame_on[p] != (want[p] != nullptr) || (want[p] && !SameRect(*want[p], g_frame_rect[p]));
    if (!changed)
        return;
    if (!g_frame) {
        if (!g_frame_class.type_info) {
            ui::InitWidgetClass(g_frame_class);
            g_frame_class.vtable[ui::kSlotDraw] = reinterpret_cast<void*>(FrameDraw);
        }
        g_frame = ui::NewWidget(g_frame_class);
        ui::Resize(g_frame, 0, 0, kScreenW, kScreenH);
        game::At<uint8_t>(g_frame, game::kWidgetOffHasAlpha) = 1;
        game::At<uint8_t>(g_frame, game::kWidgetOffMouseVisible) = 0;
        ui::AddToScreen(g_frame, kFrameZOrder);
    }
    for (int p = 0; p < 2; ++p) {
        g_frame_on[p] = want[p] != nullptr;
        if (want[p])
            g_frame_rect[p] = *want[p];
    }
    ui::MarkDirty(g_frame);
}

// ---- The mouse pointer ----
void __fastcall HookEnforceCursor(void* app, void*)
{
    if (!g_pointer_hidden) {
        g_orig_enforce_cursor(app);
        return;
    }
    int& cursor = game::At<int>(app, kAppOffCursor);
    int wanted = cursor;
    cursor = kCursorNone;
    g_orig_enforce_cursor(app);
    cursor = wanted;
}

void HidePointer(bool hide)
{
    if (hide == g_pointer_hidden)
        return;
    g_pointer_hidden = hide;
    void* app = game::App();
    reinterpret_cast<EnforceCursorFn>((*static_cast<void***>(app))[kAppSlotEnforceCursor])(app);
}

// While the controller plays, the board never sees the pointer move: it's parked out of the way, and the game's
// own re-checks of what's under it (when a dialog closes, say) would turn the cannon towards it.
bool BlockBoardPointer(int slot)
{
    if (slot != kBoardSlotMouseEnter && slot != kBoardSlotMouseMove && slot != kBoardSlotMouseDrag)
        return false;
    return pad::InUse() && !pad::MouseTookOver();
}

// ---- Directions ----
int StickDir(int held)
{
    pad::Stick s = pad::Left();
    float ax = std::fabs(s.x), ay = std::fabs(s.y);
    if (std::max(ax, ay) < (held == kNoDir ? kStickPush : kStickRelease))
        return kNoDir;
    if (ax > ay)
        return s.x > 0 ? kDirRight : kDirLeft;
    return s.y > 0 ? kDirUp : kDirDown;
}

int NavStep(Repeat& r = g_nav)
{
    int dir = pad::Down(pad::kUp) ? kDirUp : pad::Down(pad::kDown) ? kDirDown : pad::Down(pad::kLeft) ? kDirLeft
            : pad::Down(pad::kRight) ? kDirRight : kNoDir;
    r.stick = StickDir(r.stick);
    if (dir == kNoDir)
        dir = r.stick;
    DWORD now = GetTickCount();
    if (dir == kNoDir) {
        r.dir = kNoDir;
        return kNoDir;
    }
    if (dir != r.dir) {
        r.dir = dir;
        r.next = now + kRepeatDelay;
        return dir;
    }
    if (static_cast<int>(now - r.next) >= 0) {
        r.next = now + kRepeatEvery;
        return dir;
    }
    return kNoDir;
}

// ---- What can be highlighted ----
bool Clickable(const void* w)
{
    return ui::IsA(w, "ButtonWidget") || ui::IsA(w, "Checkbox") || ui::IsA(w, "Slider") || ui::IsA(w, "EditWidget");
}

void AddListRows(void* list, const ui::Rect& r, std::vector<Target>& out)
{
    int h = game::At<int>(list, kListOffItemHeight);
    char* begin = game::At<char*>(list, kListOffLines);
    char* end = game::At<char*>(list, kListOffLines + 4);
    if (h <= 0 || !begin)
        return;
    int count = static_cast<int>((end - begin) / kStringSize);
    double pos = game::At<double>(list, kListOffPosition);
    for (int i = std::max(0, static_cast<int>(pos)); i < count; ++i) {
        int top = r.y + kListTopPad + static_cast<int>((i - pos) * h);
        if (top + h > r.y + r.h)
            break;
        out.push_back({list, i, {r.x, top, r.w, h}});
    }
}

// Everything a click would reach (nothing under an open dialog): buttons, checkboxes, sliders, text boxes, and
// the lines of lists.
void Collect(void* container, std::vector<Target>& out)
{
    for (void* w : ui::Children(container)) {
        if (!game::At<uint8_t>(w, kWidgetOffVisible))
            continue;  // nor anything inside it
        if (game::At<uint8_t>(w, game::kWidgetOffMouseVisible) && !game::At<uint8_t>(w, kWidgetOffDisabled) &&
            w != chat::Button() && w != replays::SaveButton() && !lockstep::ControllerSkips(w)) {  // (Y, X)
            ui::Rect r = ui::ScreenRect(w);
            int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
            bool on_screen = r.w > 0 && r.h > 0 && cx >= 0 && cx < kScreenW && cy >= 0 && cy < kScreenH;
            if (on_screen && ui::IsA(w, "ListWidget")) {
                if (ui::WidgetAt(cx, cy) == w)
                    AddListRows(w, r, out);
            } else if (on_screen && Clickable(w) && ui::WidgetAt(cx, cy) == w) {
                out.push_back({w, -1, r});
            }
        }
        Collect(w, out);
    }
}

int Find(const std::vector<Target>& t, const void* widget, int row)
{
    for (int i = 0; i < static_cast<int>(t.size()); ++i)
        if (t[i].widget == widget && t[i].row == row)
            return i;
    return -1;
}

// The dialog on top, or else the screen: where a highlight is remembered.
void* Layer()
{
    void* screen = nullptr;
    void* dialog = nullptr;
    for (void* w : ui::Children(ui::WidgetManager())) {
        if (!game::At<uint8_t>(w, kWidgetOffVisible))
            continue;
        if (ui::IsA(w, "Dialog"))
            dialog = w;
        else if (!screen)
            screen = w;
    }
    return dialog ? dialog : screen;
}

std::string Label(const void* w)
{
    return ui::IsA(w, "ButtonWidget") ? game::ReadString(static_cast<const char*>(w) + game::kButtonOffLabel) : "";
}

int DefaultFocus(const std::vector<Target>& t, void* layer)
{
    auto remembered = g_layer_focus.find(layer);
    if (remembered != g_layer_focus.end()) {
        int i = Find(t, remembered->second.first, remembered->second.second);
        if (i >= 0)
            return i;
    }
    // A yes-or-no question: the safe answer, so a quick second press doesn't quit or leave.
    if (layer && ui::IsA(layer, "Dialog") && t.size() == 2) {
        int yes = Find(t, game::At<void*>(layer, game::kDialogOffYesButton), -1);
        int no = Find(t, game::At<void*>(layer, game::kDialogOffNoButton), -1);
        if (yes >= 0 && no >= 0)
            return no;
    }
    // A dialog with a list, or a text box already filled in (Join Game remembers the address): its OK.
    if (layer && ui::IsA(layer, "Dialog")) {
        bool box = false, empty_box = false;
        for (const Target& x : t) {
            if (x.row >= 0) {
                box = true;
            } else if (ui::IsA(x.widget, "EditWidget")) {
                box = true;
                empty_box |= game::ReadString(static_cast<char*>(x.widget) + game::kEditOffText).empty();
            }
        }
        if (box && !empty_box) {
            int i = Find(t, game::At<void*>(layer, game::kDialogOffYesButton), -1);
            if (i >= 0)
                return i;
        }
    }
    int best = -1;  // otherwise the first in reading order
    for (int i = 0; i < static_cast<int>(t.size()); ++i) {
        if (best < 0)
            best = i;
        else if (t[i].rect.y < t[best].rect.y - 8 || (std::abs(t[i].rect.y - t[best].rect.y) <= 8 && t[i].rect.x < t[best].rect.x))
            best = i;
    }
    return best;
}

int SpanGap(int a, int a_len, int b, int b_len)  // 0 where the two spans overlap
{
    return std::max(0, std::max(a, b) - std::min(a + a_len, b + b_len));
}

// The nearest target in that direction: one in line first (in the same column going up or down, the same row going
// sideways), so a sparse column, like the Two Player Selection's middle one, isn't left for the busier ones beside
// it; otherwise the closest row (or column), then the one most in line.
int Neighbor(const std::vector<Target>& t, int from, int dir)
{
    const ui::Rect& f = t[from].rect;
    bool vertical = dir == kDirUp || dir == kDirDown;
    for (int pass = 0; pass < 2; ++pass) {
        int best = -1;
        float best_score = 1e9f;
        for (int i = 0; i < static_cast<int>(t.size()); ++i) {
            if (i == from)
                continue;
            const ui::Rect& c = t[i].rect;
            int ahead = dir == kDirUp ? t[from].cy() - t[i].cy() : dir == kDirDown ? t[i].cy() - t[from].cy()
                      : dir == kDirLeft ? t[from].cx() - t[i].cx() : t[i].cx() - t[from].cx();
            if (ahead <= 0)
                continue;
            int gap = dir == kDirUp ? f.y - (c.y + c.h) : dir == kDirDown ? c.y - (f.y + f.h)
                    : dir == kDirLeft ? f.x - (c.x + c.w) : c.x - (f.x + f.w);
            int offset = vertical ? SpanGap(f.x, f.w, c.x, c.w) : SpanGap(f.y, f.h, c.y, c.h);
            if (pass == 0 && offset > 0)
                continue;
            int in_line = vertical ? std::abs(t[i].cx() - t[from].cx()) : std::abs(t[i].cy() - t[from].cy());
            float score = std::max(gap, 0) + offset * 2.0f + in_line * 0.05f;
            if (score < best_score) {
                best_score = score;
                best = i;
            }
        }
        if (best >= 0)
            return best;
    }
    return -1;
}

void NudgeSlider(void* slider, double step)
{
    double& value = game::At<double>(slider, kSliderOffValue);
    double v = std::min(1.0, std::max(0.0, value + step));
    if (v == value)
        return;
    value = v;
    if (void* listener = game::At<void*>(slider, kSliderOffListener)) {  // SliderVal(id, value), as dragging does
        using SliderValFn = void(__thiscall*)(void*, int, double);
        reinterpret_cast<SliderValFn>((*static_cast<void***>(listener))[0])(listener, game::At<int>(slider, kSliderOffId), v);
    }
    ui::MarkDirty(slider);
}

// B: the way back out, by what the buttons say. A whole screen (not a dialog: there Main Menu would leave the
// game) also goes back by its Main Menu button, which on the level screens is round and unlabelled.
const char* const kBackLabels[] = {"cancel", "back", "close", "no", "back to game", "done", "leave", "ok"};
constexpr int kLevelScreenMainMenu = 100;

bool ClickIf(const std::vector<Target>& t, const std::function<bool(const Target&)>& match)
{
    for (const Target& x : t)
        if (x.row < 0 && match(x)) {
            Click(x.cx(), x.cy());
            return true;
        }
    return false;
}

void Back(const std::vector<Target>& t, void* layer)
{
    for (const char* label : kBackLabels)
        if (ClickIf(t, [&](const Target& x) { return _stricmp(Label(x.widget).c_str(), label) == 0; }))
            return;
    if (layer && ui::IsA(layer, "ReplayFileDialog")) {  // the saved replays: Cancel says Main Menu after one played
        ClickIf(t, [](const Target& x) { return _stricmp(Label(x.widget).c_str(), "main menu") == 0; });
        return;
    }
    if (!layer || ui::IsA(layer, "Dialog"))
        return;
    if (ClickIf(t, [](const Target& x) { return _stricmp(Label(x.widget).c_str(), "main menu") == 0; }))
        return;
    if (ui::IsA(layer, "LevelScreen") || ui::IsA(layer, "TrophyScreen"))
        ClickIf(t, [&](const Target& x) {
            return ui::IsA(x.widget, "ButtonWidget") && game::At<int>(x.widget, kButtonOffId) == kLevelScreenMainMenu &&
                   game::At<void*>(x.widget, kWidgetOffParent) == layer;
        });
}

void* Board()
{
    void* app = game::App();
    return app ? game::At<void*>(app, game::kAppOffBoard) : nullptr;
}

// Start closes the in-game Menu again, as it opened it.
void CloseGameMenu(const std::vector<Target>& t, void* layer)
{
    if (!layer || !ui::IsA(layer, "OptionsDialog") || !Board())
        return;
    int i = Find(t, game::At<void*>(layer, game::kDialogOffYesButton), -1);
    if (i >= 0)
        Click(t[i].cx(), t[i].cy());
}

void MenuNav()
{
    g_context = "menu";
    ReleaseBoard();
    std::vector<Target> t;
    Collect(ui::WidgetManager(), t);
    g_target_count = static_cast<int>(t.size());
    void* layer = Layer();
    std::vector<void*> screens = ui::Children(ui::WidgetManager());  // forget dialogs that closed (their memory
    for (auto it = g_layer_focus.begin(); it != g_layer_focus.end();)  // may hold a new one next time)
        it = std::find(screens.begin(), screens.end(), it->first) == screens.end() ? g_layer_focus.erase(it) : ++it;
    int cur = Find(t, g_focus_widget, g_focus_row);
    bool appeared = cur < 0;  // a new highlight: the press that brought it up doesn't also move it
    // Until the player moves it, the highlight stays on the screen's first button: a dialog sliding in from the
    // top shows its bottom buttons first.
    if (cur < 0 || g_focus_auto) {
        cur = DefaultFocus(t, layer);
        g_focus_auto = true;
    }
    int dir = NavStep();
    if (cur >= 0 && dir != kNoDir && !appeared) {
        const Target& f = t[cur];
        if (f.row < 0 && ui::IsA(f.widget, "Slider") && (dir == kDirLeft || dir == kDirRight)) {
            NudgeSlider(f.widget, dir == kDirRight ? kSliderStep : -kSliderStep);
        } else {
            int next = Neighbor(t, cur, dir);
            if (next >= 0)
                cur = next;
        }
        g_focus_auto = false;
    }
    if (ActionPressed(config::kPadShoot))
        g_focus_auto = false;
    ShowFrame(cur >= 0 && t[cur].row < 0 && NeedsFrame(t[cur].widget) ? &t[cur].rect : nullptr);
    if (cur < 0) {
        g_focus_widget = nullptr;
        g_focus_row = -1;
    } else {
        const Target& f = t[cur];
        g_focus_widget = f.widget;
        g_focus_row = f.row;
        g_focus_x = f.cx();
        g_focus_y = f.cy();
        if (g_layer_focus.size() > 64)
            g_layer_focus.clear();
        if (!g_focus_auto)
            g_layer_focus[layer] = {f.widget, f.row};
        // The pointer sits on the highlighted button, so it shows the game's hover look (dialogs slide in, so
        // follow it).
        if (!g_menu_press.down && (ui::MouseX() != f.cx() || ui::MouseY() != f.cy()))
            MoveLater(f.cx(), f.cy());
    }
    // A is the left mouse button on the highlighted thing (with nothing to highlight, wherever the pointer is).
    if (ActionPressed(config::kPadShoot)) {
        if (cur >= 0 && t[cur].row < 0 && ui::IsA(t[cur].widget, "EditWidget"))
            keyboard::Open(t[cur].widget);
        else
            Press(g_menu_press, true, cur >= 0 ? t[cur].cx() : ui::MouseX(), cur >= 0 ? t[cur].cy() : ui::MouseY());
    } else if (g_menu_press.down && !ActionDown(config::kPadShoot)) {
        Press(g_menu_press, false, 0, 0);
    }
    if (ActionPressed(config::kPadBack))
        Back(t, layer);
    if (ActionPressed(config::kPadMenu))
        CloseGameMenu(t, layer);
}

void ChatNav()
{
    g_context = "chat";
    ReleaseAll();
    if (!g_chat_was_open)
        g_chat_row = 0;
    int dir = NavStep();
    if (dir == kDirUp && g_chat_row > 0)
        --g_chat_row;
    else if (dir == kDirDown && g_chat_row < chat::RowCount() - 1)
        ++g_chat_row;
    int x, y;
    if (!chat::RowCenter(g_chat_row, x, y))
        return;
    if (ui::MouseX() != x || ui::MouseY() != y)
        MoveLater(x, y);  // highlights the phrase
    if (ActionPressed(config::kPadShoot))
        Click(x, y);  // says it, and closes the list
    else if (ActionPressed(config::kPadBack))
        chat::ToggleList();
}

// ---- The board ----
void* PlayBoard()
{
    void* board = Board();
    if (!board || !game::At<void*>(board, kWidgetOffManager) || !ui::Shown(board))
        return nullptr;
    return ui::WidgetAt(kParkX, kParkY) == board ? board : nullptr;  // nothing over it takes the mouse there
}

void PressBoardButton(void* b)  // Menu, Instant Replay
{
    if (!b || !ui::Shown(b) || !game::At<uint8_t>(b, game::kWidgetOffMouseVisible) ||
        game::At<uint8_t>(b, kWidgetOffDisabled))
        return;
    ui::Rect r = ui::ScreenRect(b);
    Click(r.x + r.w / 2, r.y + r.h / 2);
}

void Wheel(void* board, int steps)
{
    ui::MouseWheel(board, steps);
    g_wheel_steps += std::abs(steps);
}

void Aim(void* board)
{
    void* logic = game::At<void*>(board, kBoardOffLogic);
    bool aiming = logic && game::At<int>(logic, kLogicOffState) == kStateAiming;
    pad::Stick l = pad::Left(), r = pad::Right();
    // Left stick: as fast as the setting allows when pushed all the way, slower (squared) in between.
    if (aiming && l.x != 0) {
        g_coarse_aim += std::copysign(l.x * l.x, l.x) * kAimDegreesPerSecond[config::PadAimSpeed()] * 10 * g_dt;
        int steps = static_cast<int>(g_coarse_aim);
        if (steps >= 2 || steps <= -2) {  // two or more: plain tenths of a degree
            Wheel(board, steps * kWheelRight);
            g_coarse_aim -= steps;
        }
    } else {
        g_coarse_aim = 0;
    }
    // Right stick and D-pad: single notches.
    float rate = 0;
    int dir = 0;
    if (r.x != 0) {
        rate = kFineRateMin + (kFineRateMax - kFineRateMin) * std::pow(std::fabs(r.x), 1.5f);
        dir = r.x > 0 ? 1 : -1;
    }
    DWORD now = GetTickCount();
    bool dl = pad::Down(pad::kLeft), dr = pad::Down(pad::kRight);
    if (pad::Pressed(pad::kLeft) || pad::Pressed(pad::kRight)) {
        g_dpad_since = now;
        if (aiming)
            Wheel(board, (pad::Pressed(pad::kRight) ? 1 : -1) * kWheelRight);
    } else if (dl != dr && now - g_dpad_since >= kDpadFineAfter) {
        rate = now - g_dpad_since >= kDpadFastAfter ? kDpadFastRate : kDpadFineRate;
        dir = dr ? 1 : -1;
    }
    if (aiming && dir) {
        g_fine_aim += rate * g_dt;
        if (g_fine_aim >= 1) {
            Wheel(board, dir * kWheelRight);
            g_fine_aim = std::min(g_fine_aim - 1, 1.0f);
        }
    } else {
        g_fine_aim = 0;
    }
}

// Fast-forward is the game's own: holding the right button runs the board 8 times as fast, except while a ball
// is in flight (4 times during an Instant Replay, with the pointer over the board). A held button only takes
// hold with a fresh press.
void FastForward()
{
    Press(g_right, g_right.down ? ActionDown(config::kPadFastForward) : ActionPressed(config::kPadFastForward), kParkX,
          kParkY);
}

void ParkPointer()
{
    if (ui::MouseX() != kParkX || ui::MouseY() != kParkY)
        MoveLater(kParkX, kParkY);  // the board doesn't see it move (BlockBoardPointer)
}

// While an Instant Replay plays, its panel sits at the bottom right: Instant Replay (again), Save and Done, and
// Prev and Next for saved replays. Up and down move between them (the pointer on one lights it up), A presses;
// X watches again and B is Done. Fast-forward wants the pointer on the board, so it goes there while RT is held.
void* ReplayPanel(void* board)
{
    void* panel = game::At<void*>(board, kBoardOffReplayPanel);
    return panel && game::At<void*>(panel, kWidgetOffManager) && ui::Shown(panel) ? panel : nullptr;
}

bool ReplayOn(void* board)  // the replay makes the replayed shot's player the current one meanwhile
{
    void* lm = board ? game::At<void*>(board, kBoardOffLogic) : nullptr;
    return lm && (game::At<uint8_t>(lm, kLogicOffReplaying) || ReplayPanel(board));
}

void ReplayControls(void* board, void* panel)
{
    g_context = "replay";
    Press(g_menu_press, false, 0, 0);
    Press(g_left, false, 0, 0);
    std::vector<Target> t;
    for (void* w : ui::Children(panel))
        if (ui::Shown(w) && game::At<uint8_t>(w, game::kWidgetOffMouseVisible) && !game::At<uint8_t>(w, kWidgetOffDisabled))
            t.push_back({w, -1, ui::ScreenRect(w)});
    g_target_count = static_cast<int>(t.size());
    auto button = [&](int id) {
        for (int i = 0; i < static_cast<int>(t.size()); ++i)
            if (game::At<int>(t[i].widget, kButtonOffId) == id)
                return i;
        return -1;
    };
    int cur = Find(t, g_replay_focus, -1);
    if (cur < 0)
        cur = button(kReplayDone) >= 0 ? button(kReplayDone) : t.empty() ? -1 : 0;
    int dir = NavStep();
    if (cur >= 0 && dir != kNoDir) {
        int next = Neighbor(t, cur, dir);
        if (next >= 0)
            cur = next;
    }
    g_replay_focus = cur >= 0 ? t[cur].widget : nullptr;
    bool fast = g_right.down ? ActionDown(config::kPadFastForward) : ActionPressed(config::kPadFastForward);
    if (fast || cur < 0)
        ParkPointer();
    else if (ui::MouseX() != t[cur].cx() || ui::MouseY() != t[cur].cy())
        MoveLater(t[cur].cx(), t[cur].cy());
    FastForward();
    int press = ActionPressed(config::kPadShoot) ? cur
              : ActionPressed(config::kPadBack)   ? button(kReplayDone)
              : ActionPressed(config::kPadReplay) ? button(kReplayAgain)
                                                  : -1;
    if (press >= 0) {
        ReleaseBoard();
        Click(t[press].cx(), t[press].cy());
    } else if (ActionPressed(config::kPadMenu)) {
        ReleaseBoard();
        PressBoardButton(game::At<void*>(board, kBoardOffMenuButton));
    }
}

void BoardControls(void* board)
{
    if (void* panel = ReplayPanel(board)) {
        ReplayControls(board, panel);
        return;
    }
    g_context = "board";
    g_replay_focus = nullptr;  // each replay starts on Done
    Press(g_menu_press, false, 0, 0);
    ParkPointer();
    Aim(board);
    // Shoot and flip with the left button. A button only takes hold with a fresh press: one still held from
    // choosing a chat phrase or closing a dialog doesn't shoot.
    bool left = ActionDown(config::kPadShoot) || ActionDown(config::kPadFlippers);
    bool left_pressed = ActionPressed(config::kPadShoot) || ActionPressed(config::kPadFlippers);
    Press(g_left, g_left.down ? left : left_pressed, kParkX, kParkY);
    FastForward();
    if (ActionPressed(config::kPadMenu)) {
        ReleaseBoard();
        PressBoardButton(game::At<void*>(board, kBoardOffMenuButton));
    } else if (ActionPressed(config::kPadReplay)) {
        ReleaseBoard();
        void* save = replays::SaveButton();  // online: SAVE in Instant Replay's place
        PressBoardButton(save ? save : game::At<void*>(board, kBoardOffReplayButton));
    }
}

// ---- A local duel with a controller each ----
// Two people on this computer with two controllers: the first controller plays player 1 and the second player 2
// (the other way round if the settings say so). A player's turns, Lucky Spin and master answer to their own
// controller only; menus answer to both.
void* LogicOf(void* board) { return board ? game::At<void*>(board, kBoardOffLogic) : nullptr; }
int CurrentPlayer(void* board) { return game::At<int>(LogicOf(board), kLogicOffCurrentPlayer) & 1; }
int PlayerController(int player) { return player ^ (config::PadPlayersSwapped() ? 1 : 0); }

bool SplitPlayers()
{
    void* app = game::App();
    void* lm = LogicOf(Board());
    if (pad::Count() < 2 || !lm || lockstep::Active() || online::InSession() ||
        game::At<int>(app, game::kAppOffGameMode) != game::kModeDuel || !game::At<uint8_t>(lm, kLogicOffTwoPlayers))
        return false;
    if (void* sel = game::GetDialog(kSelectionDialogId)) {  // still choosing: Play a Friend or Play Computer
        void* play_friend = game::At<void*>(sel, kSelectionOffPlayFriend);
        return play_friend && game::At<uint8_t>(play_friend, kCheckboxOffChecked);
    }
    return !game::At<uint8_t>(lm, kLogicOffComputer) && !game::At<uint8_t>(lm, kLogicOffComputer + 1);
}

bool SlotMachineShowing(void* board)
{
    void* slot = board ? game::At<void*>(board, kBoardOffSlotMachine) : nullptr;
    return slot && game::At<void*>(slot, kWidgetOffManager) && ui::Shown(slot);
}

void* SelectionOnTop()
{
    void* sel = game::GetDialog(kSelectionDialogId);
    return sel && Layer() == sel ? sel : nullptr;
}

// On the master selection, player 2 has their column of portraits and their name's EDIT, player 1 everything
// else; Play Now is for both (-1).
int Side(void* sel, const Target& t)
{
    if (t.widget == game::At<void*>(sel, game::kDialogOffYesButton))
        return -1;
    if (t.widget == game::At<void*>(sel, kSelectionOffEditName2))
        return 1;
    if (game::At<void*>(t.widget, kWidgetOffParent) == sel && ui::IsA(t.widget, "ButtonWidget")) {
        int id = game::At<int>(t.widget, kButtonOffId);
        if (id >= kMastersPerColumn && id < 2 * kMastersPerColumn)
            return 1;
    }
    return 0;
}

void* Portrait(void* sel, int player)  // the one with the player's master
{
    char* masters = static_cast<char*>(sel) + kSelectionOffMasters + player * 0x10;
    char* begin = game::At<char*>(masters, 4);
    char* end = game::At<char*>(masters, 8);
    int master = 0;
    for (int i = 0; begin && begin + i * 0xC < end; ++i)
        if (begin[i * 0xC + 4])
            master = i;
    for (void* w : ui::Children(sel))
        if (ui::IsA(w, "ButtonWidget") && game::At<int>(w, kButtonOffId) == player * kMastersPerColumn + master)
            return w;
    return nullptr;
}

// Both players choose at once, each with a highlight of their own (frames: the pointer can only show one), which
// starts on their master.
void PlayersSelect(void* sel)
{
    g_context = "players";
    ReleaseAll();
    std::vector<Target> all;
    Collect(ui::WidgetManager(), all);
    g_target_count = static_cast<int>(all.size());
    ui::Rect at[2];
    bool shown[2] = {false, false};
    for (int p = 0; p < 2; ++p) {
        pad::Select(PlayerController(p));
        std::vector<Target> mine;
        for (const Target& t : all)
            if (t.row < 0 && (Side(sel, t) == p || Side(sel, t) < 0))
                mine.push_back(t);
        int cur = Find(mine, g_player_focus[p], -1);
        if (cur < 0)
            cur = Find(mine, Portrait(sel, p), -1);
        if (cur < 0)
            cur = DefaultFocus(mine, nullptr);
        if (cur < 0) {
            g_player_focus[p] = nullptr;
            continue;
        }
        int dir = NavStep(g_player_nav[p]);
        if (dir != kNoDir) {
            int next = Neighbor(mine, cur, dir);
            if (next >= 0)
                cur = next;
        }
        g_player_focus[p] = mine[cur].widget;
        at[p] = mine[cur].rect;
        shown[p] = true;
        if (ActionPressed(config::kPadShoot))
            Click(mine[cur].cx(), mine[cur].cy());
    }
    pad::Select(pad::kEveryone);
    ShowFrame(shown[0] ? &at[0] : nullptr, shown[1] ? &at[1] : nullptr);
    ui::Rect d = ui::ScreenRect(sel);  // the pointer waits on the title, where it highlights nothing
    if (ui::MouseX() != d.x + d.w / 2 || ui::MouseY() != d.y + 30)
        MoveLater(d.x + d.w / 2, d.y + 30);
}

// Each player's controller gives a short buzz as their turn starts, so they know which one is theirs (not for
// an Instant Replay, which plays the other player's shot).
void TurnBuzz(bool split, void* board, bool replay)
{
    void* lm = LogicOf(board);
    bool aiming = lm && !replay && game::At<int>(lm, kLogicOffState) == kStateAiming;
    if (board != g_buzz_board) {
        g_buzz_board = board;
        g_buzz_player = -1;
    }
    if (split && aiming && !g_was_aiming && CurrentPlayer(board) != g_buzz_player) {
        g_buzz_player = CurrentPlayer(board);
        g_buzz_controller = PlayerController(g_buzz_player);
        ++g_buzzes;
        pad::Rumble(0.35f, 0.35f, 150, g_buzz_controller);
    }
    g_was_aiming = aiming;
}

}  // namespace

void Install()
{
    QueryPerformanceFrequency(&g_freq);
    g_orig_enforce_cursor = reinterpret_cast<EnforceCursorFn>(
        HookVtable(game::kAppVtable, kAppSlotEnforceCursor, reinterpret_cast<void*>(HookEnforceCursor)));
    lockstep::SetBoardInputFilter(BlockBoardPointer);
}

void Update()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_dt = g_last_frame.QuadPart
               ? std::min(0.05f, static_cast<float>(now.QuadPart - g_last_frame.QuadPart) / g_freq.QuadPart)
               : 0;
    g_last_frame = now;

    pad::Update();
    pad::Select(pad::kEveryone);
    keyboard::Tick();
    HidePointer(pad::InUse());
    bool chat_open = chat::ListOpen();
    g_framed = false;
    void* board = Board();
    bool split = SplitPlayers();
    bool replay = ReplayOn(board);
    if (split && (!replay || board != g_turn_board)) {
        g_turn_board = board;
        g_turn_controller = PlayerController(CurrentPlayer(board));
    }
    int turn = split ? g_turn_controller : pad::kEveryone;  // whose controller plays now
    pad::SetSoundRumbleTarget(turn);
    TurnBuzz(split, board, replay);
    if (!pad::InUse()) {
        ReleaseAll();
        g_context = "mouse";
    } else if (padmenu::Capturing()) {
        ReleaseAll();
        g_context = "capture";
        padmenu::Capture();
    } else if (keyboard::IsOpen()) {
        ReleaseAll();
        g_context = "keyboard";
        MenuInput in = {NavStep(),
                        ActionPressed(config::kPadShoot),
                        ActionPressed(config::kPadBack),
                        pad::Pressed(pad::kStart),
                        pad::Pressed(pad::kX),
                        pad::Pressed(pad::kY)};
        keyboard::Update(in);
    } else {
        if (ActionPressed(config::kPadChat) && chat::Available()) {
            ReleaseAll();
            chat::ToggleList();
        } else if (chat::ListOpen()) {
            ChatNav();
        } else if (void* play = PlayBoard()) {
            pad::Select(turn);
            BoardControls(play);
        } else if (void* sel = split ? SelectionOnTop() : nullptr) {
            PlayersSelect(sel);
        } else {
            if (split && SlotMachineShowing(board))
                pad::Select(turn);  // the spin is part of the turn
            MenuNav();
        }
    }
    pad::Select(pad::kEveryone);
    g_chat_was_open = chat_open;
    if (!g_framed)
        ShowFrame(nullptr);
    DeliverInput();  // last: it may not return until a dialog it opens is closed
}

void Focus(void* widget)
{
    g_focus_widget = widget;
    g_focus_row = -1;
    g_focus_auto = false;
}

std::string DebugState()
{
    char buf[400];
    std::string focus = "none";
    if (g_focus_widget && std::string(g_context) == "menu") {
        char f[160];
        snprintf(f, sizeof f, "%s,%d,%d,%d,%s", ui::ClassName(g_focus_widget).c_str(), g_focus_x, g_focus_y,
                 g_focus_row, Label(g_focus_widget).c_str());
        focus = f;
        for (char& c : focus)
            if (c == ' ')
                c = '_';
    }
    void* board = Board();
    void* logic = LogicOf(board);
    // The highlight frames: player 1's (or the only one) and player 2's centers.
    auto frame = [](int p) {
        const ui::Rect& r = g_frame_rect[p];
        return g_frame && g_frame_on[p] ? std::to_string(r.x + r.w / 2) + "," + std::to_string(r.y + r.h / 2)
                                        : std::string("none");
    };
    std::string replay = "none";
    if (g_replay_focus && std::string(g_context) == "replay")
        replay = std::to_string(game::At<int>(g_replay_focus, kButtonOffId));
    snprintf(buf, sizeof buf,
             "context=%s focus=%s targets=%d pointer=%d,%d hidden=%d wheel=%d aim=%.3f chatrow=%d split=%d "
             "frame1=%s frame2=%s replayfocus=%s buzzes=%d buzzed=%d",
             g_context, focus.c_str(), g_target_count, ui::MouseX(), ui::MouseY(), g_pointer_hidden ? 1 : 0,
             g_wheel_steps, logic ? game::At<float>(logic, 0xEC) : 0.0f, g_chat_row, SplitPlayers() ? 1 : 0,
             frame(0).c_str(), frame(1).c_str(), replay.c_str(), g_buzzes, g_buzz_controller);
    return buf;
}

}  // namespace controls
