#pragma once
// The game's own drawing, fonts, sounds and widgets, for things the mod puts on screen (see re/NOTES.md,
// section 5). Graphics calls follow this build's register conventions; virtual methods are ordinary __thiscall.
#include <cstdint>
#include <string>
#include <vector>

namespace ui {

struct Color {
    int r, g, b, a;
};

struct Rect {
    int x, y, w, h;
};

// Fonts, sounds and images the game loads at startup: its resource loader keeps them in these globals.
constexpr uintptr_t kFontPlain = 0x6CA4EC;          // FONT_PLAIN (NeoSans Medium 12)
constexpr uintptr_t kFontOverload12 = 0x6CA6F4;     // FONT_OVERLOAD12, the blocky lettering of the round buttons
constexpr uintptr_t kSoundButton = 0x6CA8C0;        // SOUND_BUTTON1, every button's click
constexpr uintptr_t kSoundSpeechBubble = 0x6CA544;  // SOUND_SPEECH_BUBBLE_POPUP
constexpr uintptr_t kImagePurpleButton = 0x6CAAA0;  // IMAGE_DLG_PURPLEBUTTON: a blank round button (dark, normal, lit)

template <typename T>
T* Resource(uintptr_t global)
{
    return *reinterpret_cast<T**>(global);
}
void PlaySound(uintptr_t sound_global);

// Drawing, into the Graphics a widget's Draw receives. Text is placed by its baseline.
void SetColor(void* g, const Color& c);
void SetFont(void* g, void* font);
void FillRect(void* g, int x, int y, int w, int h);
void DrawString(void* g, const std::string& text, int x, int y);
int Ascent(void* font);
int LineSpacing(void* font);
int StringWidth(void* font, const std::string& text);

// Widgets. A widget class of our own is a plain Widget's 70 virtual methods with some replaced. The game uses
// dynamic_cast on widgets (on everything on screen, when it goes back to the main menu), which reads the class's
// type information from just before the vtable, so a class keeps the Widget's.
constexpr int kWidgetSlots = 70;
constexpr int kSlotDraw = 26;  // Draw(Graphics*)
constexpr int kSlotMouseLeave = 51, kSlotMouseMove = 52, kSlotMouseDown = 54, kSlotMouseDrag = 58;  // (x, y[, clicks])
struct WidgetClass {
    const void* type_info;  // must sit right before the vtable
    void* vtable[kWidgetSlots];
};
void InitWidgetClass(WidgetClass& c);  // a plain Widget's; then replace slots in c.vtable
void* NewWidget(const WidgetClass& c);
// The round in-game buttons (Menu, Instant Replay): an image of stacked cells, the game's click sound.
void* NewImageButton(int id, void* listener, void* image);
// A checkbox in the Options dialog's look (a round toggle, the label on a bar). A click toggles it and calls the
// listener's CheckboxChecked(id, checked), slot 0, thiscall.
void* NewCheckbox(int id, void* listener, const char* label);
bool IsChecked(void* checkbox);
void SetChecked(void* checkbox, bool checked, bool tell_listener);
void Resize(void* widget, int x, int y, int w, int h);
void MarkDirty(void* widget);
// Top-level widgets stay above the game's with a lower z-order. Dialogs have 0, so a widget above them keeps
// working while one is open (the game makes everything under an open dialog ignore the mouse).
void AddToScreen(void* widget, int z_order);
void RemoveFromScreen(void* widget);
void AddChild(void* container, void* widget);  // placed relative to the container, which deletes it with itself
void Delete(void* widget);  // after RemoveFromScreen
constexpr int kSlotSetVisible = 31, kSlotSetDisabled = 37;
void SetVisible(void* widget, bool visible);
void SetDisabled(void* widget, bool disabled);

// What is on screen. Classes are told apart by the C++ type information the game keeps for every class.
void* WidgetManager();
std::string ClassName(const void* widget);             // e.g. "ThunderButton"
bool IsA(const void* widget, const char* class_name);  // that class or one derived from it, e.g. "ButtonWidget"
std::vector<void*> Children(const void* container);    // back to front; the WidgetManager's are the screens
Rect ScreenRect(const void* widget);                   // children are placed relative to their parent
bool Shown(const void* widget);                        // visible, and so is everything it sits in
void* WidgetAt(int x, int y);  // what a click there would reach (nothing under an open dialog, nothing disabled)

// Input, delivered to the WidgetManager the way the game's window messages are.
void MouseMove(int x, int y);
void MouseDown(int x, int y, int clicks);  // clicks: 1 left, 2 double, -1 right, 3 middle
void MouseUp(int x, int y, int clicks);
void MouseWheel(void* widget, int delta);  // straight to a widget (the game's own arrow keys do this)
void KeyDown(int key);                     // virtual-key codes, to the widget with the keyboard focus
void KeyUp(int key);
void KeyChar(char c);
int MouseX();
int MouseY();

}  // namespace ui
