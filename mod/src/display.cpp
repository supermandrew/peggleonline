#include "display.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "game.h"
#include "hook.h"
#include "log.h"
#include "present.h"
#include "ui.h"

namespace display {

namespace {

constexpr int kGameW = 800, kGameH = 600;
constexpr int kAppOffHwnd = 0x350;
// The game's mouse remapping (WidgetManager::RemapMouse, 0x4E9DB0): a point in the window's client area, inside
// `from`, lands at the matching point of `to`, the game's 800x600. Rects as x, y, w, h.
constexpr int kWmOffRemapTo = 0xBC, kWmOffRemapFrom = 0xCC;

// Every 10 ms (WM_TIMER, in ProcessDeferredMessages) the game checks that the mouse is still in its window: over it,
// and in its client area as far as the 800x600 frame reaches from the top left corner. Every mouse message puts it
// back in. With a bigger picture (a scaled window, borderless) the right and bottom of it were "outside", so there the
// game kept letting go of the mouse (MouseExit, the pointer switching) and taking it back: the pointer flickered and
// clicks got lost. The check measures the whole client area instead.
constexpr uintptr_t kMouseInWidth = 0x54B137;   // mov ecx, [eax+0xA0]: the frame's width (ESI = the window)
constexpr uintptr_t kMouseInHeight = 0x54B141;  // mov edx, [eax+0xA4]: its height
// The cursor thread puts the mouse's place in the window (GetCursorPos, ScreenToClient) into the DDInterface, which
// draws the custom cursor (Options > Custom Cursors) there in the frame: it is mapped the way mouse messages are.
constexpr uintptr_t kCursorThreadToClient = 0x54C050;  // call [ScreenToClient]
constexpr int kAppOffMouseIn = 0x4CC;   // bool: the game has the mouse in its window
constexpr int kDDOffNextCursor = 0xD0C;  // x, y: where the cursor thread last saw the mouse, in the frame

// The Options dialog (main menu Options and the in-game Menu).
constexpr uintptr_t kOptionsVtable = 0x68EFAC;
constexpr int kOptionsSlotResize = 40, kOptionsSlotPreferredHeight = 73;
constexpr int kOptionsOffFullscreen = 0x18C, kOptionsOffCustomCursors = 0x190;  // ThunderCheckbox*: left column
constexpr int kRow = 34;  // one row of its checkboxes and buttons
constexpr int kBorderlessId = 9102;

enum Mode { kUnmanaged, kWindowed, kBorderless };
Mode g_mode = kUnmanaged;
LONG g_saved_style, g_saved_ex_style;  // the window as the game had it, for leaving borderless
RECT g_saved_rect;
const char* g_awareness = "unaware";

void* g_options;   // the Options dialog our checkbox is in
void* g_checkbox;  // BORDERLESS

// ---- Text scaling ----
void SetDpiAware()
{
    HMODULE user = GetModuleHandleA("user32.dll");
    using ContextFn = BOOL(WINAPI*)(HANDLE);
    if (auto set = reinterpret_cast<ContextFn>(GetProcAddress(user, "SetProcessDpiAwarenessContext"))) {
        if (set(reinterpret_cast<HANDLE>(-4))) {  // per monitor, v2
            g_awareness = "per-monitor-v2";
        } else if (set(reinterpret_cast<HANDLE>(-3))) {
            g_awareness = "per-monitor";
        } else {  // already chosen (a compatibility setting): see which
            using GetFn = HANDLE(WINAPI*)();
            using AwarenessFn = int(WINAPI*)(HANDLE);
            auto get = reinterpret_cast<GetFn>(GetProcAddress(user, "GetThreadDpiAwarenessContext"));
            auto awareness = reinterpret_cast<AwarenessFn>(GetProcAddress(user, "GetAwarenessFromDpiAwarenessContext"));
            int a = get && awareness ? awareness(get()) : 0;
            g_awareness = a == 2 ? "per-monitor (preset)" : a == 1 ? "system (preset)" : "unaware (preset)";
        }
        return;
    }
    if (HMODULE shcore = LoadLibraryA("shcore.dll")) {  // Windows 8.1
        using AwarenessFn = HRESULT(WINAPI*)(int);
        auto set = reinterpret_cast<AwarenessFn>(GetProcAddress(shcore, "SetProcessDpiAwareness"));
        if (set && SUCCEEDED(set(2))) {
            g_awareness = "per-monitor";
            return;
        }
    }
    using OldFn = BOOL(WINAPI*)();
    if (auto set = reinterpret_cast<OldFn>(GetProcAddress(user, "SetProcessDPIAware")); set && set())
        g_awareness = "system";
}

UINT WindowDpi(HWND wnd)
{
    using Fn = UINT(WINAPI*)(HWND);
    static auto get = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow"));
    if (get) {
        if (UINT dpi = get(wnd))
            return dpi;
    }
    HDC dc = GetDC(nullptr);
    int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    return dpi > 0 ? dpi : 96;
}

bool SameRect(const RECT& a, const RECT& b)
{
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

// ---- The window ----
void Borderless(HWND wnd)
{
    MONITORINFO mi = {sizeof mi};
    if (!GetMonitorInfoW(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &mi))
        return;
    const RECT& m = mi.rcMonitor;
    LONG style = GetWindowLongW(wnd, GWL_STYLE), ex_style = GetWindowLongW(wnd, GWL_EXSTYLE);
    if (g_mode != kBorderless) {
        g_saved_style = style;
        g_saved_ex_style = ex_style;
        GetWindowRect(wnd, &g_saved_rect);
        g_mode = kBorderless;
        Log("display: borderless on %ldx%ld at %ld,%ld", m.right - m.left, m.bottom - m.top, m.left, m.top);
    }
    // No title bar or edges; the system menu and minimize stay (the taskbar's minimize, Alt+Space).
    LONG want = style & ~(WS_CAPTION | WS_THICKFRAME);
    LONG want_ex = ex_style & ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME | WS_EX_STATICEDGE);
    bool restyle = style != want || ex_style != want_ex;
    if (restyle) {
        SetWindowLongW(wnd, GWL_STYLE, want);
        SetWindowLongW(wnd, GWL_EXSTYLE, want_ex);
    }
    RECT r;
    GetWindowRect(wnd, &r);
    if (restyle || !SameRect(r, m))
        SetWindowPos(wnd, nullptr, m.left, m.top, m.right - m.left, m.bottom - m.top,
                     SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

// A window with a title bar, its picture as large as Windows' text scaling makes things on that monitor.
void Windowed(HWND wnd)
{
    if (g_mode == kBorderless) {
        SetWindowLongW(wnd, GWL_STYLE, g_saved_style);
        SetWindowLongW(wnd, GWL_EXSTYLE, g_saved_ex_style);
        SetWindowPos(wnd, nullptr, g_saved_rect.left, g_saved_rect.top, g_saved_rect.right - g_saved_rect.left,
                     g_saved_rect.bottom - g_saved_rect.top,
                     SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        Log("display: back to a window");
    }
    g_mode = kWindowed;
    UINT dpi = WindowDpi(wnd);
    int cw = MulDiv(kGameW, dpi, 96), ch = MulDiv(kGameH, dpi, 96);
    RECT c, r;
    if (!GetClientRect(wnd, &c) || (c.right == cw && c.bottom == ch) || !GetWindowRect(wnd, &r))
        return;
    // The frame stays as it is (title bar, edges): only the client area changes.
    SetWindowPos(wnd, nullptr, r.left, r.top, cw + (r.right - r.left) - c.right, ch + (r.bottom - r.top) - c.bottom,
                 SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
}

void Remap(const int (&to)[4], const int (&from)[4])
{
    void* wm = ui::WidgetManager();
    if (!wm)
        return;
    for (int i = 0; i < 4; ++i) {
        game::At<int>(wm, kWmOffRemapTo + 4 * i) = to[i];
        game::At<int>(wm, kWmOffRemapFrom + 4 * i) = from[i];
    }
}

// ---- The mouse ----
int __stdcall ClientExtent(HWND wnd, int vertical)
{
    RECT c;
    if (!GetClientRect(wnd, &c))
        return vertical ? kGameH : kGameW;
    return vertical ? c.bottom : c.right;
}

__declspec(naked) void MouseInWidth()  // ECX = the width of window ESI's client area
{
    __asm {
        push eax
        push edx
        push 0
        push esi
        call ClientExtent
        mov ecx, eax
        pop edx
        pop eax
        ret
    }
}

__declspec(naked) void MouseInHeight()  // EDX = its height
{
    __asm {
        push eax
        push ecx
        push 1
        push esi
        call ClientExtent
        mov edx, eax
        pop ecx
        pop eax
        ret
    }
}

// The cursor thread's ScreenToClient (it runs on its own thread): the point of the frame the mouse is over.
BOOL __stdcall CursorToFrame(HWND wnd, POINT* p)
{
    if (!ScreenToClient(wnd, p))
        return FALSE;
    void* wm = game::App() ? ui::WidgetManager() : nullptr;
    if (!wm)
        return TRUE;
    const int* to = &game::At<int>(wm, kWmOffRemapTo);
    const int* from = &game::At<int>(wm, kWmOffRemapFrom);
    if (from[2] && from[3]) {  // as WidgetManager::RemapMouse
        p->x = (p->x - from[0]) * to[2] / from[2] + to[0];
        p->y = (p->y - from[1]) * to[3] / from[3] + to[1];
    }
    // Over the black bars the mouse is in the window, which hides Windows' pointer: the custom one stays at the
    // picture's edge rather than going out of the frame.
    p->x = std::clamp<LONG>(p->x, 0, kGameW - 1);
    p->y = std::clamp<LONG>(p->y, 0, kGameH - 1);
    return TRUE;
}

// A 6-byte instruction becomes a call to fn (and a nop), if it is the one expected.
void PatchCall(uintptr_t at, const char (&expect)[7], void* fn)
{
    if (memcmp(reinterpret_cast<const void*>(at), expect, 6) != 0) {
        Log("display: unexpected code at %08X, left alone", static_cast<unsigned>(at));
        return;
    }
    uint8_t call[6] = {0xE8, 0, 0, 0, 0, 0x90};
    *reinterpret_cast<int32_t*>(call + 1) = static_cast<int32_t>(reinterpret_cast<uintptr_t>(fn) - at - 5);
    PatchBytes(at, call, sizeof call);
}

void ManageWindow()
{
    void* app = game::App();
    HWND wnd = app ? game::At<HWND>(app, kAppOffHwnd) : nullptr;
    if (!wnd || !IsWindow(wnd))
        return;
    static const int kGame[4] = {0, 0, kGameW, kGameH};
    if (!game::At<bool>(app, game::kAppOffIsWindowed)) {
        // The game's own full screen (the screen switches to 800x600): the game's window, unscaled.
        if (g_mode != kUnmanaged)
            Remap(kGame, kGame);
        g_mode = kUnmanaged;
        return;
    }
    if (IsIconic(wnd))
        return;
    if (config::Borderless())
        Borderless(wnd);
    else
        Windowed(wnd);
    RECT picture;
    SIZE client;
    if (PictureRect(wnd, picture, client)) {
        int from[4] = {picture.left, picture.top, picture.right - picture.left, picture.bottom - picture.top};
        Remap(kGame, from);
    }
}

// ---- BORDERLESS in the Options dialog ----
struct CheckboxListener {
    void* const* vtable;
};

void __fastcall OnChecked(CheckboxListener*, void*, int, int checked)
{
    bool on = (checked & 0xFF) != 0;
    config::SetBorderless(on);
    Log("display: borderless %s", on ? "chosen" : "off");
    // Borderless is a window: Fullscreen goes off, and the game switches to a window when the dialog closes.
    void* fullscreen = on && g_options ? game::At<void*>(g_options, kOptionsOffFullscreen) : nullptr;
    if (fullscreen && ui::IsChecked(fullscreen))
        ui::SetChecked(fullscreen, false, true);
}

void* const g_listener_vtable[1] = {reinterpret_cast<void*>(OnChecked)};
CheckboxListener g_listener = {g_listener_vtable};

// A row for BORDERLESS goes under the checkboxes' left column (Fullscreen, Custom Cursors), a row lower.
bool RowBelowCheckboxes(void* dialog, ui::Rect& row)
{
    void* fullscreen = game::At<void*>(dialog, kOptionsOffFullscreen);
    void* cursors = game::At<void*>(dialog, kOptionsOffCustomCursors);
    if (!fullscreen || !cursors)
        return false;
    ui::Rect a = ui::ScreenRect(fullscreen), b = ui::ScreenRect(cursors);
    row = {b.x, b.y + (b.y > a.y ? b.y - a.y : kRow), b.w, b.h};
    return true;
}

void PlaceCheckbox(void* dialog)
{
    ui::Rect row;
    if (g_checkbox && dialog == g_options && RowBelowCheckboxes(dialog, row))
        ui::Resize(g_checkbox, row.x, row.y, row.w, row.h);
}

using ResizeFn = void(__thiscall*)(void*, int, int, int, int);
using PreferredHeightFn = int(__thiscall*)(void*, int);
ResizeFn g_orig_options_resize;
PreferredHeightFn g_orig_options_height;

// The dialog lays itself out again whenever it moves (it slides in and out): our checkbox goes along.
void __fastcall HookOptionsResize(void* dialog, void*, int x, int y, int w, int h)
{
    g_orig_options_resize(dialog, x, y, w, h);
    PlaceCheckbox(dialog);
}

// A row taller. The game lays the rows of buttons out upwards from the bottom one (Done), so they move down a row
// and leave room for BORDERLESS under the checkboxes.
int __fastcall HookOptionsHeight(void* dialog, void*, int width)
{
    return g_orig_options_height(dialog, width) + kRow;
}

void* FindOptions()
{
    for (void* w : ui::Children(ui::WidgetManager()))
        if (ui::IsA(w, "OptionsDialog") && ui::Shown(w))
            return w;
    return nullptr;
}

void RemoveCheckbox()
{
    if (g_checkbox) {
        ui::RemoveFromScreen(g_checkbox);
        ui::Delete(g_checkbox);
    }
    g_checkbox = nullptr;
    g_options = nullptr;
}

void OptionsCheckbox()
{
    void* dialog = FindOptions();
    if (dialog != g_options)
        RemoveCheckbox();
    if (!dialog)
        return;
    if (!g_checkbox) {
        g_options = dialog;
        g_checkbox = ui::NewCheckbox(kBorderlessId, &g_listener, "Borderless");
        ui::SetChecked(g_checkbox, config::Borderless(), false);
        ui::AddToScreen(g_checkbox, 0);  // after the dialog's own widgets: above it, so it takes clicks
        PlaceCheckbox(dialog);
    }
    // Ticking Fullscreen unticks this.
    void* fullscreen = game::At<void*>(dialog, kOptionsOffFullscreen);
    if (fullscreen && ui::IsChecked(fullscreen) && ui::IsChecked(g_checkbox))
        ui::SetChecked(g_checkbox, false, true);
}

}  // namespace

void Install()
{
    SetDpiAware();
    Log("display: DPI awareness %s", g_awareness);
    PatchCall(kMouseInWidth, "\x8B\x88\xA0\x00\x00\x00", reinterpret_cast<void*>(MouseInWidth));
    PatchCall(kMouseInHeight, "\x8B\x90\xA4\x00\x00\x00", reinterpret_cast<void*>(MouseInHeight));
    PatchCall(kCursorThreadToClient, "\xFF\x15\xD4\x02\x67\x00", reinterpret_cast<void*>(CursorToFrame));
    g_orig_options_resize = reinterpret_cast<ResizeFn>(
        HookVtable(kOptionsVtable, kOptionsSlotResize, reinterpret_cast<void*>(HookOptionsResize)));
    g_orig_options_height = reinterpret_cast<PreferredHeightFn>(
        HookVtable(kOptionsVtable, kOptionsSlotPreferredHeight, reinterpret_cast<void*>(HookOptionsHeight)));
}

void Update()
{
    OptionsCheckbox();
    ManageWindow();
}

bool PictureRect(HWND wnd, RECT& picture, SIZE& client)
{
    RECT c;
    if (!GetClientRect(wnd, &c) || c.right <= 0 || c.bottom <= 0)
        return false;
    // As large as fits, in shape (4:3), centered.
    int w = c.right, h = MulDiv(c.right, kGameH, kGameW);
    if (h > c.bottom) {
        h = c.bottom;
        w = MulDiv(c.bottom, kGameW, kGameH);
    }
    int x = (c.right - w) / 2, y = (c.bottom - h) / 2;
    picture = {x, y, x + w, y + h};
    client = {c.right, c.bottom};
    return true;
}

std::string DebugState()
{
    void* app = game::App();
    HWND wnd = app ? game::At<HWND>(app, kAppOffHwnd) : nullptr;
    if (!wnd)
        return "nowindow";
    RECT r = {}, picture = {};
    SIZE client = {};
    GetWindowRect(wnd, &r);
    PictureRect(wnd, picture, client);
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &mi);
    void* wm = ui::WidgetManager();
    void* dd = game::At<void*>(app, game::kAppOffDDInterface);
    // What the game's mouse check finds over the middle of the picture: the game's window (the mod's Direct3D window
    // on top of it lets the mouse through), or another program's covering it.
    POINT mid = {(picture.left + picture.right) / 2, (picture.top + picture.bottom) / 2};
    ClientToScreen(wnd, &mid);
    HWND over = WindowFromPoint(mid);
    const char* under = over == wnd ? "game" : over && GetParent(over) == wnd ? "picture" : "other";
    char buf[660];
    snprintf(buf, sizeof buf,
             "mode=%s setting=%d aware=%s dpi=%u window=%ld,%ld,%ld,%ld client=%ldx%ld picture=%ld,%ld,%ld,%ld "
             "monitor=%ld,%ld,%ld,%ld remapfrom=%d,%d,%d,%d remapto=%d,%d,%d,%d style=%08lX checkbox=%d mousein=%d "
             "cursor=%d,%d under=%s %s",
             !game::At<bool>(app, game::kAppOffIsWindowed) ? "fullscreen"
             : g_mode == kBorderless                       ? "borderless"
                                                           : "windowed",
             config::Borderless() ? 1 : 0, g_awareness, WindowDpi(wnd), r.left, r.top, r.right, r.bottom, client.cx,
             client.cy, picture.left, picture.top, picture.right - picture.left, picture.bottom - picture.top,
             mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom,
             game::At<int>(wm, kWmOffRemapFrom), game::At<int>(wm, kWmOffRemapFrom + 4),
             game::At<int>(wm, kWmOffRemapFrom + 8), game::At<int>(wm, kWmOffRemapFrom + 12),
             game::At<int>(wm, kWmOffRemapTo), game::At<int>(wm, kWmOffRemapTo + 4),
             game::At<int>(wm, kWmOffRemapTo + 8), game::At<int>(wm, kWmOffRemapTo + 12),
             static_cast<unsigned long>(GetWindowLongW(wnd, GWL_STYLE)),
             g_checkbox ? (ui::IsChecked(g_checkbox) ? 1 : 0) : -1, game::At<uint8_t>(app, kAppOffMouseIn),
             dd ? game::At<int>(dd, kDDOffNextCursor) : -1, dd ? game::At<int>(dd, kDDOffNextCursor + 4) : -1, under,
             PresentState().c_str());
    return buf;
}

}  // namespace display
