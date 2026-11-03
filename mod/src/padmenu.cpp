#include "padmenu.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>

#include "config.h"
#include "dialogs.h"
#include "game.h"
#include "log.h"
#include "pad.h"
#include "ui.h"

namespace padmenu {

namespace {

constexpr int kDlgController = 9010;
constexpr int kOptionsButtonId = 9101;
// The Options dialog's own buttons below the checkboxes (Help, Demo, Main Menu, New Level, ...) have ids 7 to 12.
constexpr int kOptionsFirstId = 7, kOptionsLastId = 12;
constexpr int kWideButton = 300;  // a button across the whole dialog, which ours shares
constexpr int kButtonOffId = 0x88;
constexpr int kWidgetOffParent = 0x14;

enum Row { kRowVibration = config::kPadActionCount, kRowAimSpeed, kRowPlayers, kRowDefaults, kRowCount };
const char* const kActionNames[config::kPadActionCount] = {"SHOOT / SELECT", "BACK", "CHAT", "INSTANT REPLAY",
                                                           "FAST-FORWARD",   "MENU", "FLIPPERS"};
const char* const kAimSpeedNames[config::kAimSpeedCount] = {"SLOW", "NORMAL", "FAST"};
constexpr int kDialogW = 470, kRowX = 35, kRowW = 400, kRowH = 34, kRowStep = 38, kRowsTop = 62, kFooterArea = 74;
constexpr DWORD kCaptureTimeoutMs = 6000;
// The D-pad moves the highlight and turns the cannon, so it can't be given an action.
constexpr uint32_t kUnbindable = (1u << pad::kUp) | (1u << pad::kDown) | (1u << pad::kLeft) | (1u << pad::kRight);

void* g_options_dialog;
void* g_options_button;  // ours, in the Options dialog
void* g_rows[kRowCount];
int g_capturing = -1;    // the action waiting for its button
bool g_capture_armed;    // every button was let go since the row was chosen
DWORD g_capture_start;

std::string Buttons(uint32_t bits)
{
    std::string s;
    for (int b = 0; b < pad::kButtonCount; ++b)
        if (bits & (1u << b))
            s += (s.empty() ? "" : ", ") + std::string(pad::ButtonName(b));
    return s.empty() ? "NONE" : s;
}

std::string RowLabel(int row)
{
    if (row < config::kPadActionCount) {
        std::string what = row == g_capturing ? "PRESS A BUTTON..." : Buttons(config::PadBinding(row));
        return std::string(kActionNames[row]) + ": " + what;
    }
    if (row == kRowVibration)
        return std::string("VIBRATION: ") + (config::PadVibration() ? "ON" : "OFF");
    if (row == kRowAimSpeed)
        return std::string("AIM SPEED: ") + kAimSpeedNames[config::PadAimSpeed()];
    if (row == kRowPlayers)  // a local duel with two controllers
        return std::string("LOCAL DUEL: CONTROLLER 1 = PLAYER ") + (config::PadPlayersSwapped() ? "2" : "1");
    return "RESET TO DEFAULTS";
}

void RefreshLabels()
{
    for (int r = 0; r < kRowCount; ++r)
        if (g_rows[r])
            dialogs::SetLabel(g_rows[r], RowLabel(r));
}

// The pressed button does the action now; an action that loses its only button takes this one's old ones.
void Assign(int action, int button)
{
    uint32_t bit = 1u << button, old = config::PadBinding(action);
    for (int a = 0; a < config::kPadActionCount; ++a) {
        uint32_t b = config::PadBinding(a);
        if (a != action && (b & bit))
            config::SetPadBinding(a, (b & ~bit) ? (b & ~bit) : old);
    }
    config::SetPadBinding(action, bit);
    Log("pad: %s is now %s", kActionNames[action], pad::ButtonKey(button));
}

void OnResult(int, int button_id)
{
    if (button_id == dialogs::kFooter) {
        g_capturing = -1;
        dialogs::Close(kDlgController);
        for (void*& r : g_rows)
            r = nullptr;
        return;
    }
    if (button_id < config::kPadActionCount) {
        g_capturing = button_id;
        g_capture_armed = false;
        g_capture_start = GetTickCount();
    } else if (button_id == kRowVibration) {
        config::SetPadVibration(!config::PadVibration());
        if (config::PadVibration())
            pad::Rumble(0.5f, 0.5f, 300);  // so it can be felt
    } else if (button_id == kRowAimSpeed) {
        config::SetPadAimSpeed((config::PadAimSpeed() + 1) % config::kAimSpeedCount);
    } else if (button_id == kRowPlayers) {
        config::SetPadPlayersSwapped(!config::PadPlayersSwapped());
    } else if (button_id == kRowDefaults) {
        g_capturing = -1;
        for (int a = 0; a < config::kPadActionCount; ++a)
            config::SetPadBinding(a, config::DefaultPadBinding(a));
        config::SetPadVibration(true);
        config::SetPadAimSpeed(config::kAimNormal);
        config::SetPadPlayersSwapped(false);
    }
    RefreshLabels();
}

void OpenDialog()
{
    int height = kRowsTop + kRowCount * kRowStep + kFooterArea;
    dialogs::Panel(kDlgController, "CONTROLLER", "", kDialogW, height, "DONE", OnResult);
    g_capturing = -1;
    for (int r = 0; r < kRowCount; ++r)
        g_rows[r] = dialogs::AddButton(kDlgController, r, RowLabel(r).c_str(), kRowX, kRowsTop + r * kRowStep, kRowW, kRowH);
    Log("pad: controller settings open");
}

// The CONTROLLER button's ButtonListener. Slot order of the game's ButtonListener: Press(id, clicks), Press(id),
// Depress(id), DownTick, MouseEnter, MouseLeave, MouseMove(id, x, y).
struct Listener {
    void* const* vtable;
};
void __fastcall LPress2(Listener*, void*, int, int) {}
void __fastcall LId(Listener*, void*, int) {}
void __fastcall LDepress(Listener*, void*, int) { OpenDialog(); }
void __fastcall LMove(Listener*, void*, int, int, int) {}
void* const g_listener_vtable[7] = {reinterpret_cast<void*>(LPress2), reinterpret_cast<void*>(LId),
                                    reinterpret_cast<void*>(LDepress), reinterpret_cast<void*>(LId),
                                    reinterpret_cast<void*>(LId),     reinterpret_cast<void*>(LId),
                                    reinterpret_cast<void*>(LMove)};
Listener g_listener = {g_listener_vtable};

void* FindOptionsDialog()
{
    for (void* w : ui::Children(ui::WidgetManager()))
        if (ui::IsA(w, "OptionsDialog") && ui::Shown(w))
            return w;
    return nullptr;
}

void* g_split;  // the dialog's button that ours shares a row with
int g_split_w;  // its full width

void RemoveOptionsButton()
{
    if (g_options_button) {
        ui::RemoveFromScreen(g_options_button);
        ui::Delete(g_options_button);
    }
    g_options_button = nullptr;
    g_options_dialog = nullptr;
    g_split = nullptr;
}

void ResizeIfMoved(void* w, int x, int y, int width, int height)
{
    ui::Rect r = ui::ScreenRect(w);
    if (r.x != x || r.y != y || r.w != width || r.h != height)
        ui::Resize(w, x, y, width, height);
}

// Ours takes the right half of the dialog's last button, if that one spans the dialog. The dialog lays its
// buttons out again whenever it moves (it slides in and out), so this runs every frame.
void Fit(void* dialog)
{
    if (!g_split) {
        ui::Rect d = ui::ScreenRect(dialog);
        int last_row_y = -1, wide_y = -1;
        for (void* w : ui::Children(ui::WidgetManager())) {
            if (w == g_options_button || !ui::IsA(w, "ButtonWidget") || !ui::Shown(w))
                continue;
            int id = game::At<int>(w, kButtonOffId);
            ui::Rect r = ui::ScreenRect(w);
            if (id < kOptionsFirstId || id > kOptionsLastId || r.x < d.x || r.x + r.w > d.x + d.w || r.y < d.y ||
                r.y > d.y + d.h)
                continue;
            last_row_y = std::max(last_row_y, r.y);
            if (r.w >= kWideButton && r.y > wide_y) {
                g_split = w;
                g_split_w = r.w;
                wide_y = r.y;
            }
        }
        if (g_split && wide_y != last_row_y)
            g_split = nullptr;  // a row of two below it: no room
        if (!g_split) {
            ui::SetVisible(g_options_button, false);
            return;
        }
    }
    ui::Rect r = ui::ScreenRect(g_split);
    if (r.w >= kWideButton)
        g_split_w = r.w;  // laid out again at full width
    int half = (g_split_w - 4) / 2;
    ResizeIfMoved(g_split, r.x, r.y, half, r.h);
    ResizeIfMoved(g_options_button, r.x + g_split_w - half, r.y, half, r.h);
    if (!game::At<uint8_t>(g_options_button, 0x54))
        ui::SetVisible(g_options_button, true);
}

}  // namespace

void Update()
{
    void* dialog = FindOptionsDialog();
    if (dialog != g_options_dialog)
        RemoveOptionsButton();
    if (!dialog)
        return;
    if (!g_options_button) {
        g_options_dialog = dialog;
        g_options_button = dialogs::NewButton(kOptionsButtonId, &g_listener, "Controller");
        ui::AddToScreen(g_options_button, 0);  // after the dialog's own buttons: above it, so it takes clicks
    }
    Fit(dialog);
}

bool Capturing()
{
    if (g_capturing >= 0 && !dialogs::IsOpen(kDlgController))
        g_capturing = -1;
    return g_capturing >= 0;
}

void Capture()
{
    if (GetTickCount() - g_capture_start > kCaptureTimeoutMs) {
        g_capturing = -1;
        RefreshLabels();
        return;
    }
    if (!g_capture_armed) {  // the press that chose the row doesn't count
        g_capture_armed = pad::DownBits() == 0;
        return;
    }
    uint32_t pressed = pad::PressedBits() & ~kUnbindable;
    if (!pressed)
        return;
    int button = 0;
    while (!(pressed & (1u << button)))
        ++button;
    Assign(g_capturing, button);
    g_capturing = -1;
    RefreshLabels();
}

std::string DebugState()
{
    std::string bindings;
    for (int a = 0; a < config::kPadActionCount; ++a) {
        uint32_t bits = config::PadBinding(a);
        std::string keys;
        for (int b = 0; b < pad::kButtonCount; ++b)
            if (bits & (1u << b))
                keys += (keys.empty() ? "" : "+") + std::string(pad::ButtonKey(b));
        bindings += (a ? "," : "") + keys;
    }
    char buf[300];
    snprintf(buf, sizeof buf, "options_button=%d dialog=%d capturing=%d vibration=%d aim=%d swapped=%d bindings=%s",
             g_options_button && game::At<uint8_t>(g_options_button, 0x54) ? 1 : 0,
             dialogs::IsOpen(kDlgController) ? 1 : 0, g_capturing, config::PadVibration() ? 1 : 0,
             config::PadAimSpeed(), config::PadPlayersSwapped() ? 1 : 0, bindings.c_str());
    return buf;
}

}  // namespace padmenu
