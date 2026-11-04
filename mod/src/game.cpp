#include "game.h"

#include <cstring>

namespace game {

namespace {

constexpr uintptr_t kStringAssign = 0x4021B0;      // std::string::assign(const char*, size_t), thiscall
constexpr uintptr_t kOperatorNew = 0x634E7A;       // cdecl
constexpr uintptr_t kOperatorDelete = 0x634D4B;    // cdecl
constexpr uintptr_t kDialogShowAnim = 0x4CD3F0;    // ESI = dialog, EDI = from_top
constexpr uintptr_t kCloseDialogById = 0x432A00;   // fastcall(app, id)
constexpr uintptr_t kEditDialogCtor = 0x4ADB70;    // stdcall(mem, id, const std::string& header, bool short_name)
constexpr uintptr_t kEditWidgetStyle = 0x42A000;   // ECX = edit widget: game font, 12-char limit
constexpr uintptr_t kMainMenuContinue = 0x4C1EA0;  // EBX = MainMenu widget
constexpr uintptr_t kNewBoardOther = 0x42E1D0;     // thiscall(app, bool, bool)
constexpr uintptr_t kShowMainMenu = 0x42F180;      // fastcall(app)

template <typename Fn>
Fn VCall(void* obj, int slot)
{
    return reinterpret_cast<Fn>((*reinterpret_cast<void***>(obj))[slot]);
}

}  // namespace

GameString::GameString(const char* s)
{
    alloc_proxy = 0;
    bx.buf[0] = 0;
    size = 0;
    capacity = 15;
    Assign(s);
}

GameString::~GameString()
{
    if (capacity >= 16)
        reinterpret_cast<void(__cdecl*)(void*)>(kOperatorDelete)(bx.ptr);
}

void GameString::Assign(const char* s)
{
    AssignString(this, s);
}

void AssignString(void* game_string, const char* s)
{
    using AssignFn = void*(__thiscall*)(void*, const char*, size_t);
    reinterpret_cast<AssignFn>(kStringAssign)(game_string, s, strlen(s));
}

std::string ReadString(const void* game_string)
{
    auto* s = static_cast<const GameString*>(game_string);
    return std::string(s->c_str(), s->size);
}

void* GameNew(size_t size)
{
    return reinterpret_cast<void*(__cdecl*)(size_t)>(kOperatorNew)(size);
}

void* DoDialog(int id, const char* header, const char* lines, const char* footer, int button_mode)
{
    GameString h(header), l(lines), f(footer);
    using Fn = void*(__thiscall*)(void*, int, int, GameString*, GameString*, GameString*, int);
    return VCall<Fn>(App(), kAppSlotDoDialog)(App(), id, 1, &h, &l, &f, button_mode);
}

void* GetDialog(int id)
{
    using Fn = void*(__thiscall*)(void*, int);
    return VCall<Fn>(App(), kAppSlotGetDialog)(App(), id);
}

void AddDialog(int id, void* dialog)
{
    using Fn = void(__thiscall*)(void*, int, void*);
    VCall<Fn>(App(), kAppSlotAddDialog)(App(), id, dialog);
}

GAME_CALL void ShowDialogAnimated(void* dialog, bool from_top)
{
    int top = from_top ? 1 : 0;
    __asm {
        push esi
        push edi
        mov esi, dialog
        mov edi, top
        mov eax, kDialogShowAnim
        call eax
        pop edi
        pop esi
    }
}

void CloseDialog(int id)
{
    if (GetDialog(id))
        reinterpret_cast<void(__fastcall*)(void*, int)>(kCloseDialogById)(App(), id);
}

GAME_CALL void* NewEditDialog(int id, const char* header)
{
    void* mem = GameNew(kEditDialogSize);
    GameString h(header);
    using Ctor = void*(__stdcall*)(void*, int, GameString*, int);
    void* dlg = reinterpret_cast<Ctor>(kEditDialogCtor)(mem, id, &h, 0);
    void* edit = At<void*>(dlg, kEditDialogOffEdit);
    __asm {
        mov ecx, edit
        mov eax, kEditWidgetStyle
        call eax
    }
    At<int>(edit, kEditOffMaxChars) = 40;
    return dlg;
}

GAME_CALL void ShowLevelScreenFromMainMenu(void* main_menu_listener)
{
    At<uint8_t>(main_menu_listener, kMainMenuListenerOffResume) = 0;
    void* app = At<void*>(main_menu_listener, kMainMenuListenerOffApp);
    At<int>(app, kAppOffGameMode) = kModeDuel;
    void* menu = static_cast<char*>(main_menu_listener) + kMainMenuListenerToWidget;
    __asm {
        push ebx
        mov ebx, menu
        mov eax, kMainMenuContinue
        call eax
        pop ebx
    }
}

void ShowMainMenu(void* app)
{
    reinterpret_cast<void(__fastcall*)(void*)>(kShowMainMenu)(app);
}

void NewBoard(void* app)
{
    reinterpret_cast<void(__thiscall*)(void*, int, int)>(kNewBoardOther)(app, 1, 0);
}

}  // namespace game
