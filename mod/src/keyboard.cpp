#include "keyboard.h"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "game.h"
#include "log.h"
#include "ui.h"

namespace keyboard {

namespace {

constexpr int kZOrder = 1100;            // above the chat, which is above the game's dialogs
constexpr int kEditSlotProcessKey = 70;  // EditWidget: (key code, character), what its KeyDown and KeyChar feed
constexpr int kWmSlotSetFocus = 8;       // WidgetManager: give a widget the keyboard focus
constexpr int kScreenW = 800, kScreenH = 600;

const char* const kRows[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL.", "ZXCVBNM-:_"};
constexpr int kCharRows = 4, kColumns = 10;
enum Special { kNotSpecial = -1, kShift, kSpace, kDelete, kDone, kSpecialCount };
const char* const kSpecialLabels[kSpecialCount] = {"SHIFT", "SPACE", "DEL", "DONE"};
const int kSpecialSpan[kSpecialCount] = {2, 4, 2, 2};  // in keys; ten across, like the rows above
constexpr int kKeyW = 40, kKeyH = 34, kGap = 4, kPad = 10;

const ui::Color kFill = {16, 10, 48, 230};  // the chat box's colours
const ui::Color kBorder = {170, 110, 255, 255};
const ui::Color kKey = {48, 30, 100, 255};
const ui::Color kKeyLit = {120, 60, 200, 255};
const ui::Color kText = {255, 255, 255, 255};
const ui::Color kTextLit = {255, 240, 120, 255};

struct Key {
    ui::Rect r;
    char c;
    int special;
    int row;
};

std::vector<Key> g_keys;
ui::Rect g_panel;
int g_focus;
bool g_lower;
void* g_edit;
void* g_dialog;  // the dialog the text box is in
void* g_overlay;
ui::WidgetClass g_class;
bool g_class_ready;
void* g_font;

void Layout(const ui::Rect& avoid)
{
    int w = 2 * kPad + kColumns * kKeyW + (kColumns - 1) * kGap;
    int h = 2 * kPad + (kCharRows + 1) * kKeyH + kCharRows * kGap;
    int x = (kScreenW - w) / 2, y = avoid.y + avoid.h + 6;  // under the dialog if it fits, else over it
    if (y + h > kScreenH - 4)
        y = avoid.y - 6 - h;
    if (y < 4)
        y = kScreenH - 4 - h;
    g_panel = {x, y, w, h};
    g_keys.clear();
    for (int r = 0; r < kCharRows; ++r)
        for (int i = 0; kRows[r][i]; ++i)
            g_keys.push_back({{x + kPad + i * (kKeyW + kGap), y + kPad + r * (kKeyH + kGap), kKeyW, kKeyH}, kRows[r][i],
                              kNotSpecial, r});
    int kx = x + kPad, ky = y + kPad + kCharRows * (kKeyH + kGap);
    for (int s = 0; s < kSpecialCount; ++s) {
        int kw = kSpecialSpan[s] * kKeyW + (kSpecialSpan[s] - 1) * kGap;
        g_keys.push_back({{kx, ky, kw, kKeyH}, 0, s, kCharRows});
        kx += kw + kGap;
    }
}

std::string KeyLabel(const Key& k)
{
    if (k.special != kNotSpecial)
        return kSpecialLabels[k.special];
    return std::string(1, g_lower ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.c))) : k.c);
}

bool InTree(void* container, void* target)  // compares pointers only: target may be gone
{
    for (void* w : ui::Children(container))
        if (w == target || InTree(w, target))
            return true;
    return false;
}

void Type(int key, char c)
{
    using ProcessKeyFn = void(__thiscall*)(void*, int, int);
    reinterpret_cast<ProcessKeyFn>((*static_cast<void***>(g_edit))[kEditSlotProcessKey])(g_edit, key,
                                                                                         static_cast<unsigned char>(c));
}

void Redraw()
{
    if (g_overlay)
        ui::MarkDirty(g_overlay);
}

void PressKey(int i)
{
    const Key& k = g_keys[i];
    switch (k.special) {
    case kShift:
        g_lower = !g_lower;
        break;
    case kSpace:
        Type(0, ' ');
        break;
    case kDelete:
        Type(VK_BACK, 0);
        break;
    case kDone:
        Close();
        return;
    default:
        Type(0, g_lower ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.c))) : k.c);
        break;
    }
    Redraw();
}

void Move(int dir)
{
    const Key f = g_keys[g_focus];
    if (dir == 2 || dir == 3) {  // left, right: along the row, wrapping round
        std::vector<int> row;
        int at = 0;
        for (int i = 0; i < static_cast<int>(g_keys.size()); ++i)
            if (g_keys[i].row == f.row) {
                if (i == g_focus)
                    at = static_cast<int>(row.size());
                row.push_back(i);
            }
        int n = static_cast<int>(row.size());
        g_focus = row[(at + (dir == 3 ? 1 : n - 1)) % n];
        return;
    }
    int to = f.row + (dir == 1 ? 1 : -1);  // up, down: the key in that row nearest below or above
    if (to < 0 || to > kCharRows)
        return;
    int cx = f.r.x + f.r.w / 2, best = -1;
    for (int i = 0; i < static_cast<int>(g_keys.size()); ++i)
        if (g_keys[i].row == to &&
            (best < 0 || std::abs(g_keys[i].r.x + g_keys[i].r.w / 2 - cx) < std::abs(g_keys[best].r.x + g_keys[best].r.w / 2 - cx)))
            best = i;
    if (best >= 0)
        g_focus = best;
}

int KeyAt(int x, int y)
{
    for (int i = 0; i < static_cast<int>(g_keys.size()); ++i) {
        const ui::Rect& r = g_keys[i].r;
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
            return i;
    }
    return -1;
}

void __fastcall OverlayDraw(void*, void*, void* g)
{
    const ui::Rect& p = g_panel;
    ui::SetColor(g, kFill);
    ui::FillRect(g, p.x + 2, p.y + 2, p.w - 4, p.h - 4);
    ui::SetColor(g, kBorder);
    ui::FillRect(g, p.x + 2, p.y, p.w - 4, 2);
    ui::FillRect(g, p.x + 2, p.y + p.h - 2, p.w - 4, 2);
    ui::FillRect(g, p.x, p.y + 2, 2, p.h - 4);
    ui::FillRect(g, p.x + p.w - 2, p.y + 2, 2, p.h - 4);
    ui::SetFont(g, g_font);
    int ascent = ui::Ascent(g_font), line = ui::LineSpacing(g_font);
    for (int i = 0; i < static_cast<int>(g_keys.size()); ++i) {
        const Key& k = g_keys[i];
        bool lit = i == g_focus;
        ui::SetColor(g, lit ? kKeyLit : kKey);
        ui::FillRect(g, k.r.x, k.r.y, k.r.w, k.r.h);
        std::string label = KeyLabel(k);
        ui::SetColor(g, lit ? kTextLit : kText);
        ui::DrawString(g, label, k.r.x + (k.r.w - ui::StringWidth(g_font, label)) / 2,
                       k.r.y + (k.r.h - line) / 2 + ascent);
    }
}

void __fastcall OverlayMouseMove(void*, void*, int x, int y)
{
    int k = KeyAt(x, y);
    if (k >= 0 && k != g_focus) {
        g_focus = k;
        Redraw();
    }
}

void __fastcall OverlayMouseDown(void*, void*, int x, int y, int clicks)
{
    if (clicks < 0)
        return;
    int k = KeyAt(x, y);
    const ui::Rect& p = g_panel;
    if (k >= 0)
        PressKey(k);
    else if (x < p.x || x >= p.x + p.w || y < p.y || y >= p.y + p.h)
        Close();  // a click beside the keyboard puts it away
}

bool Ready()
{
    g_font = ui::Resource<void>(ui::kFontPlain);
    if (!g_font)
        return false;
    if (!g_class_ready) {
        ui::InitWidgetClass(g_class);
        g_class.vtable[ui::kSlotDraw] = reinterpret_cast<void*>(OverlayDraw);
        g_class.vtable[ui::kSlotMouseMove] = reinterpret_cast<void*>(OverlayMouseMove);
        g_class.vtable[ui::kSlotMouseDrag] = reinterpret_cast<void*>(OverlayMouseMove);
        g_class.vtable[ui::kSlotMouseDown] = reinterpret_cast<void*>(OverlayMouseDown);
        g_class_ready = true;
    }
    return true;
}

}  // namespace

void Open(void* edit)
{
    if (!Ready())
        return;
    Close();
    g_edit = edit;
    g_dialog = nullptr;
    // The dialog the box belongs to: dialogs keep their text boxes and buttons beside them on the screen, not
    // inside, so it's the dialog the box lies on.
    ui::Rect e = ui::ScreenRect(edit);
    void* wm = ui::WidgetManager();
    for (void* w : ui::Children(wm)) {
        ui::Rect d = ui::ScreenRect(w);
        if (ui::IsA(w, "Dialog") && ui::Shown(w) && e.x >= d.x && e.x + e.w <= d.x + d.w && e.y >= d.y &&
            e.y + e.h <= d.y + d.h)
            g_dialog = w;
    }
    Layout(ui::ScreenRect(g_dialog ? g_dialog : edit));
    g_focus = 0;
    g_lower = false;
    g_overlay = ui::NewWidget(g_class);
    ui::Resize(g_overlay, 0, 0, kScreenW, kScreenH);
    game::At<uint8_t>(g_overlay, game::kWidgetOffHasAlpha) = 1;
    ui::AddToScreen(g_overlay, kZOrder);
    // The text box takes the keyboard focus, which shows its caret (and a real keyboard still works).
    reinterpret_cast<void(__thiscall*)(void*, void*)>((*static_cast<void***>(wm))[kWmSlotSetFocus])(wm, edit);
    Log("keyboard: open");
}

bool IsOpen() { return g_overlay != nullptr; }

void Tick()
{
    if (g_overlay && !InTree(ui::WidgetManager(), g_edit)) {
        g_edit = nullptr;  // its dialog closed some other way
        g_dialog = nullptr;
        Close();
    }
}

void Update(const controls::MenuInput& in)
{
    if (!g_overlay)
        return;
    if (in.dir >= 0) {
        Move(in.dir);
        Redraw();
    }
    if (in.select)
        PressKey(g_focus);
    else if (in.back)
        Type(VK_BACK, 0);
    else if (in.x)
        Type(0, ' ');
    else if (in.y)
        g_lower = !g_lower;
    else if (in.start)
        Close();
    Redraw();
}

void Close()
{
    if (!g_overlay)
        return;
    ui::RemoveFromScreen(g_overlay);
    ui::Delete(g_overlay);
    g_overlay = nullptr;
    if (g_dialog && InTree(ui::WidgetManager(), g_dialog))
        controls::Focus(game::At<void*>(g_dialog, game::kDialogOffYesButton));  // on to OK
    g_edit = nullptr;
    g_dialog = nullptr;
    Log("keyboard: closed");
}

std::string DebugState()
{
    char buf[200];
    std::string text = g_edit ? game::ReadString(static_cast<char*>(g_edit) + game::kEditOffText) : "";
    snprintf(buf, sizeof buf, "open=%d key=%d lower=%d text=", g_overlay ? 1 : 0, g_focus, g_lower ? 1 : 0);
    return buf + text;
}

}  // namespace keyboard
