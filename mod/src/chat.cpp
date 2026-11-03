#include "chat.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <deque>

#include "game.h"
#include "log.h"
#include "protocol.h"
#include "ui.h"

namespace chat {

namespace {

// In the order a player reaches for them during a duel.
const char* const kPhrases[] = {
    "Nice shot!", "No way!", "Impossible!", "Lucky!",  // the opponent's shot
    "Calculated.", "I'm too good.", "Oops!",           // your own
    "Praise the bucket!", "EXTREME FEVER!",            // the game's big moments
    "Good game!", "Rematch?",                          // the end
};
constexpr int kPhraseCount = sizeof kPhrases / sizeof kPhrases[0];
constexpr int kGroupStarts[] = {4, 7, 9};  // a small gap above each group

constexpr int kButtonId = 9100;
constexpr int kZOrder = 1000;  // above the game's dialogs, so chat also works on the results screen
constexpr int kScreenW = 800, kScreenH = 600;
// The button covers the corner ornament above the ball counter, in line with the Menu button at the bottom.
constexpr int kButtonX = 12, kButtonY = 0;
constexpr int kListX = 10, kListY = 56;                     // the phrases, under the button
constexpr int kBoxX = 84, kBoxBottom = 586, kBoxLines = 5;  // what was said, at the bottom left of the board
constexpr int kPad = 7, kRowExtra = 4, kGroupGap = 6;
constexpr DWORD kShowMs = 5000, kFadeMs = 600;
constexpr size_t kHistoryMax = 50;

const ui::Color kFill = {16, 10, 48, 215};
const ui::Color kBorder = {170, 110, 255, 255};
const ui::Color kSeparator = {170, 110, 255, 90};
const ui::Color kHover = {120, 60, 200, 230};
const ui::Color kText = {255, 255, 255, 255};
const ui::Color kHoverText = {255, 240, 120, 255};
const ui::Color kMyName = {255, 210, 90, 255};
const ui::Color kTheirName = {110, 215, 255, 255};
const ui::Color kLabel = {235, 85, 220, 255};  // CHAT, in the Menu button's magenta
const ui::Color kLabelLit = {255, 200, 250, 255};
const ui::Color kNote = {140, 255, 140, 255};  // the mod telling this player something (only they see it)

struct Line {
    bool mine;
    int phrase;
    std::string note;  // instead of a phrase
};

std::deque<Line> g_lines;
std::string g_my_name = "You", g_their_name = "Opponent";
DWORD g_activity;  // when the box last had news: a message, or the list opened or closed
bool g_open;       // the phrase list
int g_hover = -1;
int g_drawn_alpha = -1;

bool g_attached;
bool g_unavailable_logged;
void* g_button;
void* g_overlay;  // whole screen: draws the box and the list, takes the mouse only while the list is open
ui::WidgetClass g_overlay_class;

void* g_font;  // measured when attaching
int g_ascent, g_line_h, g_row_h, g_list_w, g_list_h;

ui::Color Faded(ui::Color c, int alpha)
{
    c.a = c.a * alpha / 255;
    return c;
}

int RowTop(int i)  // from the top of the list
{
    int gaps = 0;
    for (int start : kGroupStarts)
        gaps += i >= start;
    return kPad + i * g_row_h + gaps * kGroupGap;
}

int RowAt(int x, int y)
{
    if (x < kListX || x >= kListX + g_list_w)
        return -1;
    for (int i = 0; i < kPhraseCount; ++i) {
        int top = kListY + RowTop(i);
        if (y >= top && y < top + g_row_h)
            return i;
    }
    return -1;
}

void Measure()
{
    g_font = ui::Resource<void>(ui::kFontPlain);
    g_ascent = ui::Ascent(g_font);
    g_line_h = ui::LineSpacing(g_font);
    g_row_h = g_line_h + kRowExtra;
    int widest = 0;
    for (const char* p : kPhrases)
        widest = std::max(widest, ui::StringWidth(g_font, p));
    g_list_w = widest + 2 * kPad + 12;
    g_list_h = RowTop(kPhraseCount) + kPad;
}

// Fully shown while the list is open and for a while after the last news, then fading out.
int BoxAlpha()
{
    if (g_lines.empty())
        return 0;
    if (g_open)
        return 255;
    DWORD idle = GetTickCount() - g_activity;
    if (idle < kShowMs)
        return 255;
    if (idle >= kShowMs + kFadeMs)
        return 0;
    return 255 - static_cast<int>((idle - kShowMs) * 255 / kFadeMs);
}

void Panel(void* g, int x, int y, int w, int h, int alpha)
{
    ui::SetColor(g, Faded(kFill, alpha));
    ui::FillRect(g, x + 2, y + 2, w - 4, h - 4);
    ui::SetColor(g, Faded(kBorder, alpha));  // the corners stay open, which rounds them a little
    ui::FillRect(g, x + 2, y, w - 4, 2);
    ui::FillRect(g, x + 2, y + h - 2, w - 4, 2);
    ui::FillRect(g, x, y + 2, 2, h - 4);
    ui::FillRect(g, x + w - 2, y + 2, 2, h - 4);
}

void DrawBox(void* g, int alpha)
{
    int n = static_cast<int>(std::min<size_t>(g_lines.size(), kBoxLines));
    const Line* lines[kBoxLines];
    std::string names[kBoxLines];
    int name_w[kBoxLines], width = 0;
    for (int i = 0; i < n; ++i) {
        lines[i] = &g_lines[g_lines.size() - n + i];
        bool note = !lines[i]->note.empty();
        names[i] = note ? std::string() : (lines[i]->mine ? g_my_name : g_their_name) + ": ";
        name_w[i] = ui::StringWidth(g_font, names[i]);
        width = std::max(width, name_w[i] + ui::StringWidth(g_font, note ? lines[i]->note : kPhrases[lines[i]->phrase]));
    }
    int w = width + 2 * kPad, h = n * g_line_h + 2 * kPad;
    int x = kBoxX, y = kBoxBottom - h;
    Panel(g, x, y, w, h, alpha);
    ui::SetFont(g, g_font);
    for (int i = 0; i < n; ++i) {
        int base = y + kPad + i * g_line_h + g_ascent;
        if (!lines[i]->note.empty()) {
            ui::SetColor(g, Faded(kNote, alpha));
            ui::DrawString(g, lines[i]->note, x + kPad, base);
            continue;
        }
        ui::SetColor(g, Faded(lines[i]->mine ? kMyName : kTheirName, alpha));
        ui::DrawString(g, names[i], x + kPad, base);
        ui::SetColor(g, Faded(kText, alpha));
        ui::DrawString(g, kPhrases[lines[i]->phrase], x + kPad + name_w[i], base);
    }
}

void DrawList(void* g)
{
    Panel(g, kListX, kListY, g_list_w, g_list_h, 255);
    ui::SetColor(g, kSeparator);
    for (int start : kGroupStarts)
        ui::FillRect(g, kListX + 10, kListY + RowTop(start) - (kGroupGap + 1) / 2, g_list_w - 20, 1);
    ui::SetFont(g, g_font);
    for (int i = 0; i < kPhraseCount; ++i) {
        int top = kListY + RowTop(i);
        if (i == g_hover) {
            ui::SetColor(g, kHover);
            ui::FillRect(g, kListX + 3, top, g_list_w - 6, g_row_h);
        }
        ui::SetColor(g, i == g_hover ? kHoverText : kText);
        ui::DrawString(g, kPhrases[i], kListX + kPad + 4, top + (g_row_h - g_line_h) / 2 + g_ascent);
    }
}

void AddLine(bool mine, int phrase, const std::string& note = std::string())
{
    g_lines.push_back({mine, phrase, note});
    if (g_lines.size() > kHistoryMax)
        g_lines.pop_front();
    g_activity = GetTickCount();
    if (g_attached)
        ui::MarkDirty(g_overlay);
}

void Say(int phrase)
{
    proto::Chat c = {static_cast<uint8_t>(phrase)};
    net::Send(proto::kChat, &c, sizeof c);
    AddLine(true, phrase);
    ui::PlaySound(ui::kSoundButton);
    Log("chat: we said \"%s\"", kPhrases[phrase]);
}

void ShowList(bool open)
{
    g_open = open;
    g_hover = -1;
    g_activity = GetTickCount();
    game::At<uint8_t>(g_overlay, game::kWidgetOffMouseVisible) = open;  // closed, clicks reach the game again
    ui::MarkDirty(g_overlay);
}

void SetHover(int row)
{
    if (row == g_hover)
        return;
    g_hover = row;
    ui::MarkDirty(g_overlay);
}

void __fastcall OverlayDraw(void*, void*, void* g)
{
    g_drawn_alpha = BoxAlpha();
    if (g_drawn_alpha > 0)
        DrawBox(g, g_drawn_alpha);
    if (g_open)
        DrawList(g);
}

void __fastcall OverlayMouseMove(void*, void*, int x, int y) { SetHover(RowAt(x, y)); }
void __fastcall OverlayMouseLeave(void*, void*) { SetHover(-1); }

void __fastcall OverlayMouseDown(void*, void*, int x, int y, int clicks)
{
    if (!g_open)
        return;
    int row = RowAt(x, y);
    ShowList(false);  // any click closes the list, and one on a phrase says it
    if (row >= 0 && clicks > 0)
        Say(row);
}

// The CHAT button's ButtonListener. Slot order of the game's ButtonListener: Press(id, clicks), Press(id),
// Depress(id), DownTick, MouseEnter, MouseLeave, MouseMove(id, x, y).
struct Listener {
    void* const* vtable;
};

void __fastcall LPress2(Listener*, void*, int, int) {}
void __fastcall LId(Listener*, void*, int) {}
void __fastcall LDepress(Listener*, void*, int) { ShowList(!g_open); }
void __fastcall LMove(Listener*, void*, int, int, int) {}

void* const g_listener_vtable[7] = {reinterpret_cast<void*>(LPress2), reinterpret_cast<void*>(LId),
                                    reinterpret_cast<void*>(LDepress), reinterpret_cast<void*>(LId),
                                    reinterpret_cast<void*>(LId),     reinterpret_cast<void*>(LId),
                                    reinterpret_cast<void*>(LMove)};
Listener g_listener = {g_listener_vtable};

// A round button like Menu: the game's blank purple one, lettered with the font of the game's round buttons.
void* MakeButton(void* image)
{
    void* b = ui::NewImageButton(kButtonId, &g_listener, image);
    // The game's factory expects two cells; this image has three: dark (as Menu looks on the board), lit, bright.
    int w = game::At<int>(image, game::kImageOffWidth);
    int h = game::At<int>(image, game::kImageOffHeight) / 3;
    auto* cells = reinterpret_cast<ui::Rect*>(static_cast<char*>(b) + game::kButtonOffRects);
    cells[0] = {0, 0, w, h};      // normal
    cells[1] = {0, h, w, h};      // mouse over
    cells[2] = {0, 2 * h, w, h};  // pressed
    cells[3] = {0, 0, w, h};      // disabled
    game::AssignString(static_cast<char*>(b) + game::kButtonOffLabel, "CHAT");
    game::At<void*>(b, game::kButtonOffFont) = ui::Resource<void>(ui::kFontOverload12);
    auto* colors = game::At<ui::Color*>(b, game::kWidgetOffColors);
    colors[0] = kLabel;
    colors[1] = kLabelLit;
    ui::Resize(b, kButtonX, kButtonY, w, h);
    return b;
}

void Attach()
{
    void* image = ui::Resource<void>(ui::kImagePurpleButton);
    if (!image || game::At<int>(image, game::kImageOffRows) != 3 || !ui::Resource<void>(ui::kFontPlain) ||
        !ui::Resource<void>(ui::kFontOverload12)) {
        if (!g_unavailable_logged)
            Log("chat: the game's button image or fonts are not loaded; no chat");
        g_unavailable_logged = true;
        return;
    }
    Measure();
    ui::InitWidgetClass(g_overlay_class);
    g_overlay_class.vtable[ui::kSlotDraw] = reinterpret_cast<void*>(OverlayDraw);
    g_overlay_class.vtable[ui::kSlotMouseMove] = reinterpret_cast<void*>(OverlayMouseMove);
    g_overlay_class.vtable[ui::kSlotMouseDrag] = reinterpret_cast<void*>(OverlayMouseMove);
    g_overlay_class.vtable[ui::kSlotMouseLeave] = reinterpret_cast<void*>(OverlayMouseLeave);
    g_overlay_class.vtable[ui::kSlotMouseDown] = reinterpret_cast<void*>(OverlayMouseDown);
    g_overlay = ui::NewWidget(g_overlay_class);
    ui::Resize(g_overlay, 0, 0, kScreenW, kScreenH);
    game::At<uint8_t>(g_overlay, game::kWidgetOffHasAlpha) = 1;  // see-through: what is under it is drawn first
    game::At<uint8_t>(g_overlay, game::kWidgetOffMouseVisible) = 0;
    ui::AddToScreen(g_overlay, kZOrder);
    g_button = MakeButton(image);
    ui::AddToScreen(g_button, kZOrder + 1);  // above the overlay, so it also closes the list
    g_open = false;
    g_hover = -1;
    g_drawn_alpha = -1;
    g_attached = true;
    Log("chat: on screen (text ascent %d, line %d, list %dx%d)", g_ascent, g_line_h, g_list_w, g_list_h);
}

void Detach()
{
    for (void* w : {g_button, g_overlay}) {
        ui::RemoveFromScreen(w);
        ui::Delete(w);
    }
    g_button = g_overlay = nullptr;
    g_open = false;
    g_attached = false;
    Log("chat: off screen");
}

}  // namespace

void Update(bool on_board)
{
    if (on_board && !g_attached)
        Attach();
    else if (!on_board && g_attached)
        Detach();
    if (g_attached && BoxAlpha() != g_drawn_alpha)
        ui::MarkDirty(g_overlay);  // fading out, or news to show
}

void SetNames(const std::string& me, const std::string& opponent)
{
    g_my_name = me;
    g_their_name = opponent;
}

void OnMessage(const net::Message& m)
{
    proto::Chat c;
    if (m.data.size() < sizeof c)
        return;
    memcpy(&c, m.data.data(), sizeof c);
    if (c.phrase >= kPhraseCount)
        return;
    AddLine(false, c.phrase);
    ui::PlaySound(ui::kSoundSpeechBubble);
    Log("chat: %s said \"%s\"", g_their_name.c_str(), kPhrases[c.phrase]);
}

void Note(const std::string& text)
{
    AddLine(true, 0, text);
    Log("chat: noted \"%s\"", text.c_str());
}

void End()
{
    if (g_attached)
        Detach();
    g_lines.clear();
}

std::string DebugState()
{
    std::string last;
    if (!g_lines.empty())
        last = !g_lines.back().note.empty()
                   ? g_lines.back().note
                   : (g_lines.back().mine ? g_my_name : g_their_name) + ": " + kPhrases[g_lines.back().phrase];
    char buf[160];
    wsprintfA(buf, "attached=%d open=%d alpha=%d lines=%d button=%d,%d last=", g_attached ? 1 : 0, g_open ? 1 : 0,
              BoxAlpha(), static_cast<int>(g_lines.size()), kButtonX + 25, kButtonY + 25);
    return buf + last;
}

bool Available() { return g_attached; }
void* Button() { return g_button; }
bool ListOpen() { return g_open; }

void ToggleList()
{
    if (g_attached)
        ShowList(!g_open);
}

int RowCount() { return kPhraseCount; }

bool RowCenter(int row, int& x, int& y)
{
    if (!g_open || row < 0 || row >= kPhraseCount)
        return false;
    x = kListX + g_list_w / 2;
    y = kListY + RowTop(row) + g_row_h / 2;
    return true;
}

}  // namespace chat
