#include "dialogs.h"

#include <windows.h>

#include <cctype>
#include <cstring>
#include <map>

#include "game.h"
#include "hook.h"
#include "log.h"

namespace dialogs {

namespace {

constexpr int kFirstId = 9000, kLastId = 9099;  // the game's own dialog ids stay below 100

constexpr uintptr_t kMakeThunderButton = 0x4298D0;  // ECX = image (0: default); cdecl(id, listener, label)
constexpr int kWidgetSlotAddWidget = 3;
constexpr int kWidgetSlotMarkDirty = 17;

ResultHandler g_handler;
std::map<int, ResultHandler> g_panel_handlers;
int g_lines_offset = -1;  // where Dialog keeps its body text; found the first time we show one

void Report(int dialog_id, int button_id)
{
    auto it = g_panel_handlers.find(dialog_id);
    if (it != g_panel_handlers.end())
        it->second(dialog_id, button_id);
    else if (g_handler)
        g_handler(dialog_id, button_id);
}

using DialogResultFn = void(__thiscall*)(void* listener, int dialog_id, int button_id);
DialogResultFn g_orig_dialog_result;

bool IsOurs(int id) { return id >= kFirstId && id <= kLastId; }

// The game's text-entry dialog is made for player names: letters, digits, space and _-(). Ours take addresses.
constexpr uintptr_t kEditDialogListenerVtable = 0x68E7B8;  // EditDialog's EditListener; slot 2: AllowChar(id, c)
constexpr int kEditDialogOffListener = 0x178;
using AllowCharFn = bool(__thiscall*)(void* listener, int edit_id, char c);
AllowCharFn g_orig_allow_char;

bool __fastcall HookAllowChar(void* listener, void*, int edit_id, char c)
{
    void* dlg = static_cast<char*>(listener) - kEditDialogOffListener;
    if (IsOurs(game::At<int>(dlg, game::kDialogOffId)))
        return isalnum(static_cast<unsigned char>(c)) || c == '.' || c == ':' || c == '-' || c == '_';
    return g_orig_allow_char(listener, edit_id, c);
}

void __fastcall HookDialogResult(void* listener, void*, int dialog_id, int button_id)
{
    if (!IsOurs(dialog_id)) {
        g_orig_dialog_result(listener, dialog_id, button_id);
        return;
    }
    Report(dialog_id, button_id);
}

// A ButtonListener for the extra buttons of Choice dialogs (stock Dialog only forwards ids 1000/1001).
// Slot order of the game's ButtonListener: Press(id, clicks), Press(id), Depress(id), DownTick, MouseEnter,
// MouseLeave, MouseMove(id, x, y).
struct ChoiceListener {
    void** vtable;
    int dialog_id;
};

void __fastcall LPress2(ChoiceListener*, void*, int, int) {}
void __fastcall LPress1(ChoiceListener*, void*, int) {}
void __fastcall LDepress(ChoiceListener* self, void*, int button_id)
{
    if (game::GetDialog(self->dialog_id))
        Report(self->dialog_id, button_id);
}
void __fastcall LId(ChoiceListener*, void*, int) {}
void __fastcall LMove(ChoiceListener*, void*, int, int, int) {}

void* g_listener_vtable[7] = {reinterpret_cast<void*>(LPress2), reinterpret_cast<void*>(LPress1),
                              reinterpret_cast<void*>(LDepress), reinterpret_cast<void*>(LId),
                              reinterpret_cast<void*>(LId),     reinterpret_cast<void*>(LId),
                              reinterpret_cast<void*>(LMove)};
ChoiceListener g_listeners[kLastId - kFirstId + 1];

GAME_CALL void* MakeButton(int id, void* listener, const char* label)
{
    game::GameString text(label);
    void* button;
    __asm {
        lea eax, text
        push eax
        push listener
        push id
        xor ecx, ecx
        mov eax, kMakeThunderButton
        call eax
        add esp, 12
        mov button, eax
    }
    return button;
}

template <typename Fn>
Fn VCall(void* obj, int slot)
{
    return reinterpret_cast<Fn>((*reinterpret_cast<void***>(obj))[slot]);
}

void Resize(void* widget, int x, int y, int w, int h)
{
    VCall<void(__thiscall*)(void*, int, int, int, int)>(widget, game::kWidgetSlotResize)(widget, x, y, w, h);
}

void MarkDirty(void* widget) { VCall<void(__thiscall*)(void*)>(widget, kWidgetSlotMarkDirty)(widget); }

void FindLinesOffset(void* dlg, const char* text)
{
    if (g_lines_offset >= 0)
        return;
    size_t len = strlen(text);
    for (int off = 0x40; off < 0x170; off += 4) {
        auto* s = reinterpret_cast<game::GameString*>(static_cast<char*>(dlg) + off);
        if (s->size == len && s->capacity >= len && s->capacity < 0x10000 && memcmp(s->c_str(), text, len) == 0) {
            g_lines_offset = off;
            Log("dialogs: body text is at Dialog+%X", off);
            return;
        }
    }
    Log("dialogs: body text offset not found");
}

}  // namespace

void Install(ResultHandler handler)
{
    g_handler = std::move(handler);
    g_orig_dialog_result = reinterpret_cast<DialogResultFn>(
        HookVtable(game::kAppDialogListenerVtable, 1, reinterpret_cast<void*>(HookDialogResult)));
    g_orig_allow_char = reinterpret_cast<AllowCharFn>(
        HookVtable(kEditDialogListenerVtable, 2, reinterpret_cast<void*>(HookAllowChar)));
    for (int i = 0; i <= kLastId - kFirstId; ++i)
        g_listeners[i] = {g_listener_vtable, kFirstId + i};
}

void Message(int id, const char* header, const std::string& text, const char* footer)
{
    game::CloseDialog(id);
    void* dlg = game::DoDialog(id, header, text.c_str(), footer, game::kButtonsFooter);
    FindLinesOffset(dlg, text.c_str());
    game::ShowDialogAnimated(dlg, true);
}

void Choice(int id, const char* header, const std::string& text, const char* choice_a, const char* choice_b,
            const char* footer)
{
    game::CloseDialog(id);
    // Blank lines leave room for the two choice buttons above the bottom button.
    std::string body = text + "\n\n\n";
    void* dlg = game::DoDialog(id, header, body.c_str(), footer, game::kButtonsFooter);
    FindLinesOffset(dlg, body.c_str());
    // The dialog's own buttons are placed in screen coordinates; ours are its children, so make them relative.
    void* footer_button = game::At<void*>(dlg, game::kDialogOffYesButton);
    int fx = game::At<int>(footer_button, game::kWidgetOffX) - game::At<int>(dlg, game::kWidgetOffX);
    int fy = game::At<int>(footer_button, game::kWidgetOffY) - game::At<int>(dlg, game::kWidgetOffY);
    int fw = game::At<int>(footer_button, game::kWidgetOffWidth);
    int fh = game::At<int>(footer_button, game::kWidgetOffHeight);
    int gap = 12, bw = (fw - gap) / 2, by = fy - fh - 16;
    ChoiceListener* listener = &g_listeners[id - kFirstId];
    void* a = MakeButton(kChoiceA, listener, choice_a);
    void* b = MakeButton(kChoiceB, listener, choice_b);
    Resize(a, fx, by, bw, fh);
    Resize(b, fx + bw + gap, by, bw, fh);
    VCall<void(__thiscall*)(void*, void*)>(dlg, kWidgetSlotAddWidget)(dlg, a);
    VCall<void(__thiscall*)(void*, void*)>(dlg, kWidgetSlotAddWidget)(dlg, b);
    game::ShowDialogAnimated(dlg, true);
}

void TextEntry(int id, const char* header, const std::string& initial)
{
    game::CloseDialog(id);
    void* dlg = game::NewEditDialog(id, header);
    void* edit = game::At<void*>(dlg, game::kEditDialogOffEdit);
    game::GameString text(initial.c_str());
    VCall<void(__thiscall*)(void*, game::GameString*, int)>(edit, game::kEditSlotSetText)(edit, &text, 1);
    int w = 350;
    int h = VCall<int(__thiscall*)(void*, int)>(dlg, game::kDialogSlotPreferredHeight)(dlg, w);
    Resize(dlg, 400 - w / 2, 300 - h / 2, w, h);
    game::ShowDialogAnimated(dlg, true);
    game::AddDialog(id, dlg);
}

std::string TextEntryValue(int id)
{
    void* dlg = game::GetDialog(id);
    if (!dlg)
        return {};
    void* edit = game::At<void*>(dlg, game::kEditDialogOffEdit);
    return game::ReadString(static_cast<char*>(edit) + game::kEditOffText);
}

void SetText(int id, const std::string& text)
{
    void* dlg = game::GetDialog(id);
    if (!dlg || g_lines_offset < 0)
        return;
    game::AssignString(static_cast<char*>(dlg) + g_lines_offset, text.c_str());
    MarkDirty(dlg);
}

bool IsOpen(int id) { return game::GetDialog(id) != nullptr; }

void Close(int id) { game::CloseDialog(id); }

void* Panel(int id, const char* header, const std::string& text, int width, int height, const char* footer,
            ResultHandler handler)
{
    game::CloseDialog(id);
    g_panel_handlers[id] = std::move(handler);
    void* dlg = game::DoDialog(id, header, text.c_str(), footer, game::kButtonsFooter);
    Resize(dlg, 400 - width / 2, 300 - height / 2, width, height);  // the dialog lays its bottom button out again
    game::ShowDialogAnimated(dlg, true);
    return dlg;
}

void* AddButton(int id, int button_id, const char* label, int x, int y, int w, int h)
{
    void* dlg = game::GetDialog(id);
    if (!dlg)
        return nullptr;
    void* b = MakeButton(button_id, &g_listeners[id - kFirstId], label);
    Resize(b, x, y, w, h);
    VCall<void(__thiscall*)(void*, void*)>(dlg, kWidgetSlotAddWidget)(dlg, b);
    return b;
}

void SetLabel(void* button, const std::string& label)
{
    game::AssignString(static_cast<char*>(button) + game::kButtonOffLabel, label.c_str());
    MarkDirty(button);
}

void* NewButton(int button_id, void* listener, const char* label) { return MakeButton(button_id, listener, label); }

}  // namespace dialogs
