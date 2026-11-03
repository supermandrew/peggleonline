#pragma once
// Addresses and layouts in the Peggle Nights game image (popcapgame1.exe, PE timestamp 0x48DC3B3E).
// The image has no relocations, so these never move. See re/NOTES.md and re/names.tsv.
// Offsets are raw byte offsets from the start of the object (the decompiler's *_data fields are shifted).
#include <cstdint>
#include <string>

namespace game {

constexpr uint32_t kTimestamp = 0x48DC3B3E;
constexpr uint32_t kSizeOfImage = 0x36E000;

template <typename T>
inline T& At(const void* base, int off)
{
    return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(base) + off);
}

// ---- Globals ----
constexpr uintptr_t kAppPtr = 0x6CBE10;  // ThunderballApp* (SexyAppBase constructor stores `this`)
inline void* App() { return *reinterpret_cast<void**>(kAppPtr); }

// ---- ThunderballApp ----
constexpr uintptr_t kAppVtable = 0x68670C;
constexpr uintptr_t kAppDialogListenerVtable = 0x6868C0;  // slot 1: DialogButtonDepress(dialogId, buttonId)
constexpr int kAppSlotNewDialog = 25;
constexpr int kAppSlotDoDialog = 74;    // Dialog* DoDialog(id, modal, header, lines, footer, buttonMode)
constexpr int kAppSlotGetDialog = 75;   // Dialog* GetDialog(id)
constexpr int kAppSlotAddDialog = 77;   // void AddDialog(id, Dialog*)
constexpr int kAppSlotDrawDirty = 93;   // bool DrawDirtyStuff(), every frame: draws what changed, then Redraws
constexpr int kAppSlotRedraw = 94;      // bool Redraw(Rect*): only after something was drawn (else once a second)

constexpr int kAppOffIsWindowed = 0x343;   // bool (registry ScreenMode == 0)
constexpr int kAppOffDDInterface = 0x36C;
constexpr int kAppOffCustomCursorDirty = 0x501;  // bool: draw a frame even if nothing else changed
constexpr int kAppOffUpdateCount = 0x484;   // +1 per 10 ms update; animation curves measure time with it
constexpr int kAppOffGameMode = 0x7F4;     // 1 adventure, 2 quick play, 3 duel, 4 challenge, 6 demo
constexpr int kAppOffStage = 0x7F8;        // level chosen on the level screen: stage index ...
constexpr int kAppOffLevel = 0x7FC;        // ... and level index within the stage
constexpr int kAppOffBoard = 0x864;        // Board*
constexpr int kAppOffProfile = 0x934;      // current player profile (name: std::string at +4, stage at +0x2C)
constexpr int kModeDuel = 3;

// ---- DDInterface (surfaces are IDirectDrawSurface v1 interfaces) ----
constexpr int kDDOffPrimary = 0x60;      // primary surface (whole desktop when windowed)
constexpr int kDDOffDrawSurface = 0x68;  // offscreen surface each frame is drawn into, 800x600
constexpr int kDDOffHwnd = 0xCE0;        // game window

// ---- Widgets and dialogs ----
constexpr int kWidgetSize = 0x88;
constexpr int kWidgetOffX = 0x30, kWidgetOffY = 0x34, kWidgetOffWidth = 0x38, kWidgetOffHeight = 0x3C;
constexpr int kWidgetOffHasAlpha = 0x40;         // bool: redraw what is underneath first
constexpr int kWidgetOffZOrder = 0x50;           // siblings are kept sorted by it
constexpr int kWidgetOffMouseVisible = 0x55;     // bool: takes mouse input
constexpr int kWidgetOffColors = 0x60;           // std::vector<Color> data (ButtonWidget: 0 label, 1 highlighted label)
constexpr int kWidgetSlotResize = 40;            // Resize(x, y, w, h)
constexpr int kButtonOffLabel = 0x8C;            // std::string
constexpr int kButtonOffFont = 0xAC;
constexpr int kButtonOffRects = 0xC0;            // Rect[4]: image cells for normal, mouse over, pressed, disabled
constexpr int kImageOffWidth = 0x24, kImageOffHeight = 0x28, kImageOffRows = 0x2C;
constexpr int kDialogSlotPreferredHeight = 73;   // int GetPreferredHeight(width)
constexpr int kDialogOffYesButton = 0x94;        // left button (ID 1000: yes / ok / footer)
constexpr int kDialogOffNoButton = 0x98;         // right button (ID 1001: no / cancel)
constexpr int kDialogOffId = 0x13C;
constexpr int kButtonYes = 1000, kButtonNo = 1001;
enum ButtonMode { kButtonsNone = 0, kButtonsYesNo = 1, kButtonsOkCancel = 2, kButtonsFooter = 3 };

constexpr int kEditDialogSize = 0x184;
constexpr int kEditDialogOffEdit = 0x17C;        // EditWidget*
constexpr int kEditOffText = 0x8C;               // std::string
constexpr int kEditOffMaxChars = 0xF0;
constexpr int kEditSlotSetText = 73;             // SetText(const std::string&, bool cursorToEnd)

// ---- Main menu ----
constexpr uintptr_t kMainMenuButtonListenerVtable = 0x68D824;  // slot 2: ButtonDepress(id)
constexpr int kMainMenuButtonDuel = 0x66;
constexpr int kMainMenuListenerOffApp = 0x6C;
constexpr int kMainMenuListenerOffResume = 0xA8;  // "resume saved game" flag the Duel button sets
constexpr int kMainMenuListenerToWidget = -0x88;  // listener sub-object -> MainMenu widget

// ---- Level screen ----
constexpr uintptr_t kLevelScreenNewBoardCall = 0x4BA407;  // call NewBoard_Other after a level is picked

// VC8 std::string as laid out in the game (0x1C bytes). Always build these with the game's own functions so
// the game can copy or free them.
struct GameString {
    uint32_t alloc_proxy;
    union {
        char buf[16];
        char* ptr;
    } bx;
    uint32_t size;
    uint32_t capacity;

    explicit GameString(const char* s = "");
    GameString(const GameString&) = delete;
    GameString& operator=(const GameString&) = delete;
    ~GameString();
    void Assign(const char* s);
    const char* c_str() const { return capacity >= 16 ? bx.ptr : bx.buf; }
};
static_assert(sizeof(GameString) == 0x1C, "VC8 std::string layout");

std::string ReadString(const void* game_string);  // copy a std::string living inside a game object
void AssignString(void* game_string, const char* s);

// ---- Functions in the game ----
void* GameNew(size_t size);
void* DoDialog(int id, const char* header, const char* lines, const char* footer, int button_mode);
void* GetDialog(int id);
void AddDialog(int id, void* dialog);
void ShowDialogAnimated(void* dialog, bool from_top);  // slide-in animation + sound, as the game does it
void CloseDialog(int id);                                // slide-out and remove
void* NewEditDialog(int id, const char* header);         // native text-entry dialog (not yet added)
void ShowLevelScreenFromMainMenu(void* main_menu_listener);  // continue the Duel button past the save check
void NewBoard(void* app);                                // ThunderballApp::NewBoard_Other(true, false)
void ShowMainMenu(void* app);                            // close the board / screens, show the main menu

}  // namespace game
