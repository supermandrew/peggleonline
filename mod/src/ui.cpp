#include "ui.h"

#include <cstring>

#include "game.h"

namespace ui {

namespace {

constexpr uintptr_t kSoundApp = 0x6CBE04;  // SexyAppBase* the game plays its sounds through
constexpr int kAppSlotPlaySample = 56;     // PlaySample(id)
constexpr int kAppOffWidgetManager = 0x320;

constexpr uintptr_t kGraphicsSetFont = 0x51AD80;     // EAX = g, ECX = font
constexpr uintptr_t kGraphicsSetColor = 0x51AD90;    // thiscall(g, const Color&)
constexpr uintptr_t kGraphicsFillRect = 0x51AE20;    // EAX = g, stdcall(x, y, w, h): current color and draw mode
constexpr uintptr_t kGraphicsDrawString = 0x51B750;  // thiscall(g, const std::string&, x, baseline y)
constexpr int kFontSlotAscent = 1, kFontSlotLineSpacing = 6, kFontSlotStringWidth = 7;

constexpr uintptr_t kWidgetCtor = 0x4EABC0;      // stdcall(this)
constexpr uintptr_t kWidgetVtable = 0x68FABC;
constexpr uintptr_t kNewImageButton = 0x4299E0;  // cdecl(id, listener, image, bool) -> ThunderButton*
constexpr uintptr_t kNewCheckbox = 0x429E40;     // ECX id, EAX CheckboxListener*, EDI style (0), EBX label -> EAX
constexpr int kSlotAddWidget = 3, kSlotRemoveWidget = 4, kSlotMarkDirty = 17, kSlotMouseWheel = 59;
constexpr int kCheckboxSlotSetChecked = 70, kCheckboxSlotIsChecked = 71;  // (bool checked, bool tell); ()

// WidgetManager entry points, as SexyAppBase's message handling calls them.
constexpr uintptr_t kWmMouseMove = 0x4EA970;    // EAX x, ECX y, EDX wm
constexpr uintptr_t kWmMouseDown = 0x4EA860;    // ECX wm, push x, EAX y, EBX clicks
constexpr uintptr_t kWmMouseUp = 0x4EA7B0;      // EAX wm, push clicks, push x, EBX y
constexpr uintptr_t kWmKeyDown = 0x4EAB30;      // EAX wm, ECX key
constexpr uintptr_t kWmKeyUp = 0x4EAB70;        // EAX wm, ECX key
constexpr uintptr_t kWmKeyChar = 0x4EAAE0;      // EAX wm, ECX char
constexpr uintptr_t kWmWidgetAt = 0x4E9A80;     // EAX y, ECX x, EDX wm, stdcall(int* local_x, int* local_y)
constexpr int kWmOffMouseX = 0xE0, kWmOffMouseY = 0xE4;
constexpr int kWmOffHasFocus = 0x84;  // bool: ButtonWidget::MouseUp only fires while the window has the focus

constexpr int kContainerOffListHead = 0x08;  // std::list<Widget*> of children: head node {next, prev, value}
constexpr int kWidgetOffParent = 0x14;
constexpr int kWidgetOffVisible = 0x54;

template <typename Fn>
Fn VCall(void* obj, int slot)
{
    return reinterpret_cast<Fn>((*reinterpret_cast<void***>(obj))[slot]);
}

// MSVC's run-time type information: the vtable's slot -1 points at the class's "complete object locator".
struct TypeDescriptor {
    const void* vtable;
    void* spare;
    char name[1];  // ".?AVThunderButton@Sexy@@"
};
struct BaseClassDescriptor {
    const TypeDescriptor* type;
};
struct ClassHierarchyDescriptor {
    uint32_t signature, attributes, base_count;
    const BaseClassDescriptor* const* bases;  // the class itself first
};
struct CompleteObjectLocator {
    uint32_t signature, offset, cd_offset;
    const TypeDescriptor* type;
    const ClassHierarchyDescriptor* hierarchy;
};

const CompleteObjectLocator* Locator(const void* obj)
{
    return static_cast<const CompleteObjectLocator* const*>(*static_cast<void* const*>(obj))[-1];
}

bool NameIs(const TypeDescriptor* t, const char* class_name)
{
    const char* n = t->name;
    if (strncmp(n, ".?AV", 4) != 0)
        return false;
    size_t len = strlen(class_name);
    return strncmp(n + 4, class_name, len) == 0 && n[4 + len] == '@';
}

// While a click is delivered, lend the WidgetManager the window's focus (a test's window may not have it).
struct LendFocus {
    uint8_t& has_focus;
    uint8_t had;
    LendFocus() : has_focus(game::At<uint8_t>(WidgetManager(), kWmOffHasFocus)), had(has_focus) { has_focus = 1; }
    ~LendFocus() { has_focus = had; }
};

}  // namespace

void* WidgetManager() { return game::At<void*>(game::App(), kAppOffWidgetManager); }

void PlaySound(uintptr_t sound_global)
{
    void* app = *reinterpret_cast<void**>(kSoundApp);
    int id = *reinterpret_cast<int*>(sound_global);
    if (app && id >= 0)
        VCall<void(__thiscall*)(void*, int)>(app, kAppSlotPlaySample)(app, id);
}

void SetColor(void* g, const Color& c)
{
    reinterpret_cast<void(__thiscall*)(void*, const Color*)>(kGraphicsSetColor)(g, &c);
}

GAME_CALL void SetFont(void* g, void* font)
{
    __asm {
        mov eax, g
        mov ecx, font
        mov edx, kGraphicsSetFont
        call edx
    }
}

GAME_CALL void FillRect(void* g, int x, int y, int w, int h)
{
    __asm {
        push h
        push w
        push y
        push x
        mov eax, g
        mov edx, kGraphicsFillRect
        call edx
    }
}

void DrawString(void* g, const std::string& text, int x, int y)
{
    game::GameString s(text.c_str());
    reinterpret_cast<void(__thiscall*)(void*, const game::GameString*, int, int)>(kGraphicsDrawString)(g, &s, x, y);
}

int Ascent(void* font) { return VCall<int(__thiscall*)(void*)>(font, kFontSlotAscent)(font); }

int LineSpacing(void* font) { return VCall<int(__thiscall*)(void*)>(font, kFontSlotLineSpacing)(font); }

int StringWidth(void* font, const std::string& text)
{
    game::GameString s(text.c_str());
    return VCall<int(__thiscall*)(void*, const game::GameString*)>(font, kFontSlotStringWidth)(font, &s);
}

void InitWidgetClass(WidgetClass& c)
{
    const auto* vtable = reinterpret_cast<void* const*>(kWidgetVtable);
    c.type_info = vtable[-1];
    memcpy(c.vtable, vtable, sizeof c.vtable);
}

void* NewWidget(const WidgetClass& c)
{
    void* w = game::GameNew(game::kWidgetSize);
    reinterpret_cast<void(__stdcall*)(void*)>(kWidgetCtor)(w);
    *static_cast<void* const**>(w) = c.vtable;
    return w;
}

void* NewImageButton(int id, void* listener, void* image)
{
    return reinterpret_cast<void*(__cdecl*)(int, void*, void*, int)>(kNewImageButton)(id, listener, image, 0);
}

GAME_CALL void* NewCheckbox(int id, void* listener, const char* label)
{
    game::GameString text(label);  // copied into the checkbox
    void* box;
    __asm {
        lea ebx, text
        xor edi, edi
        mov eax, listener
        mov ecx, id
        mov edx, kNewCheckbox
        call edx
        mov box, eax
    }
    return box;
}

bool IsChecked(void* checkbox)
{
    return VCall<bool(__thiscall*)(void*)>(checkbox, kCheckboxSlotIsChecked)(checkbox);
}

void SetChecked(void* checkbox, bool checked, bool tell_listener)
{
    VCall<void(__thiscall*)(void*, bool, bool)>(checkbox, kCheckboxSlotSetChecked)(checkbox, checked, tell_listener);
}

void Resize(void* widget, int x, int y, int w, int h)
{
    VCall<void(__thiscall*)(void*, int, int, int, int)>(widget, game::kWidgetSlotResize)(widget, x, y, w, h);
}

void MarkDirty(void* widget) { VCall<void(__thiscall*)(void*)>(widget, kSlotMarkDirty)(widget); }

void AddToScreen(void* widget, int z_order)
{
    game::At<int>(widget, game::kWidgetOffZOrder) = z_order;
    void* wm = WidgetManager();
    VCall<void(__thiscall*)(void*, void*)>(wm, kSlotAddWidget)(wm, widget);
}

void RemoveFromScreen(void* widget)
{
    void* wm = WidgetManager();
    VCall<void(__thiscall*)(void*, void*)>(wm, kSlotRemoveWidget)(wm, widget);
}

void AddChild(void* container, void* widget)
{
    VCall<void(__thiscall*)(void*, void*)>(container, kSlotAddWidget)(container, widget);
}

void Delete(void* widget) { VCall<void*(__thiscall*)(void*, int)>(widget, 0)(widget, 1); }

void SetVisible(void* widget, bool visible)
{
    VCall<void(__thiscall*)(void*, bool)>(widget, kSlotSetVisible)(widget, visible);
}

void SetDisabled(void* widget, bool disabled)
{
    VCall<void(__thiscall*)(void*, bool)>(widget, kSlotSetDisabled)(widget, disabled);
}

std::string ClassName(const void* widget)
{
    std::string n = Locator(widget)->type->name;
    if (n.compare(0, 4, ".?AV") == 0)
        n = n.substr(4, n.find('@') - 4);
    return n;
}

bool IsA(const void* widget, const char* class_name)
{
    const ClassHierarchyDescriptor* h = Locator(widget)->hierarchy;
    for (uint32_t i = 0; i < h->base_count; ++i)
        if (NameIs(h->bases[i]->type, class_name))
            return true;
    return false;
}

std::vector<void*> Children(const void* container)
{
    struct Node {
        Node* next;
        Node* prev;
        void* value;
    };
    std::vector<void*> out;
    Node* head = game::At<Node*>(container, kContainerOffListHead);
    for (Node* n = head ? head->next : nullptr; n && n != head; n = n->next)
        out.push_back(n->value);
    return out;
}

Rect ScreenRect(const void* widget)
{
    Rect r = {game::At<int>(widget, game::kWidgetOffX), game::At<int>(widget, game::kWidgetOffY),
              game::At<int>(widget, game::kWidgetOffWidth), game::At<int>(widget, game::kWidgetOffHeight)};
    void* wm = WidgetManager();
    for (void* p = game::At<void*>(widget, kWidgetOffParent); p && p != wm; p = game::At<void*>(p, kWidgetOffParent)) {
        r.x += game::At<int>(p, game::kWidgetOffX);
        r.y += game::At<int>(p, game::kWidgetOffY);
    }
    return r;
}

bool Shown(const void* widget)
{
    void* wm = WidgetManager();
    for (const void* w = widget; w && w != wm; w = game::At<void*>(w, kWidgetOffParent))
        if (!game::At<uint8_t>(w, kWidgetOffVisible))
            return false;
    return true;
}

GAME_CALL void* WidgetAt(int x, int y)
{
    void* wm = WidgetManager();
    int lx = 0, ly = 0;
    int* plx = &lx;
    int* ply = &ly;
    void* found;
    __asm {
        push ply
        push plx
        mov eax, y
        mov ecx, x
        mov edx, wm
        mov esi, kWmWidgetAt
        call esi
        mov found, eax
    }
    return found;
}

GAME_CALL void MouseMove(int x, int y)
{
    void* wm = WidgetManager();
    __asm {
        mov eax, x
        mov ecx, y
        mov edx, wm
        mov ebx, kWmMouseMove
        call ebx
    }
}

GAME_CALL void MouseDown(int x, int y, int clicks)
{
    LendFocus focus;
    void* wm = WidgetManager();
    __asm {
        mov ecx, wm
        push x
        mov eax, y
        mov ebx, clicks
        mov edx, kWmMouseDown
        call edx
    }
}

GAME_CALL void MouseUp(int x, int y, int clicks)
{
    LendFocus focus;
    void* wm = WidgetManager();
    __asm {
        mov eax, wm
        push clicks
        push x
        mov ebx, y
        mov edx, kWmMouseUp
        call edx
    }
}

void MouseWheel(void* widget, int delta) { VCall<void(__thiscall*)(void*, int)>(widget, kSlotMouseWheel)(widget, delta); }

GAME_CALL void KeyDown(int key)
{
    void* wm = WidgetManager();
    __asm {
        mov eax, wm
        mov ecx, key
        mov edx, kWmKeyDown
        call edx
    }
}

GAME_CALL void KeyUp(int key)
{
    void* wm = WidgetManager();
    __asm {
        mov eax, wm
        mov ecx, key
        mov edx, kWmKeyUp
        call edx
    }
}

GAME_CALL void KeyChar(char c)
{
    void* wm = WidgetManager();
    int code = static_cast<unsigned char>(c);
    __asm {
        mov eax, wm
        mov ecx, code
        mov edx, kWmKeyChar
        call edx
    }
}

int MouseX() { return game::At<int>(WidgetManager(), kWmOffMouseX); }
int MouseY() { return game::At<int>(WidgetManager(), kWmOffMouseY); }

}  // namespace ui
