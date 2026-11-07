# Peggle Nights — reverse-engineering notes

Addresses are virtual addresses in the game executable (the `popcapgame1.exe` the Steam build runs; base 0x400000, relocations stripped, so they never move).
"Decompiler offset" means the `Board_data.field_0xNNN` names Ghidra prints; raw offset = 0x8C + NNN for `Board`.

## 1. How the program is packaged

- `PeggleNights.exe` is PopCap's launcher. It runs the real game as `popcapgame1.exe` (`2`/`3` if busy) with `-changedir="<install dir>"`.
  **The running game is a separate process named `popcapgame1.exe`.** A mod must load into that process.
- Game build: MSVC 2005, x86, 2008-09-26. Built with LTCG, so many internal functions take `this` in **EAX or EBX** instead of ECX. Virtual methods use normal `__thiscall`.
- Engine: PopCap "Sexy" framework (newer internal version than the public 1.3 source), DirectDraw/D3D7, BASS audio (`bass.dll`, loaded by name at runtime), `j2k-codec.dll` for `.jp2` images. Internal codename "Thunderball".
- Command-line switches present: `-record` `-play` `-demofile` `-recnum` `-playnum` (framework demo recorder), `-replay=<file.pegn>`, `-changedir`, `-crash`, `-version`, `-screensaver`.

## 2. Assets

`main.pak` / `levelpacks/*.pak` are PopCap paks (whole file XOR 0xF7, magic `0xBAC04AC0`). `main.pak` holds 1,203 files:
level geometry (`levels/*.dat`), `levels/stages.cfg` (adventure stages), `characters/characters.cfg` (11 masters and their powers),
`levels/trophy.cfg`, `properties/resources.xml`, images, sounds, fonts.

Masters whose power takes player input mid-shot (relevant for sync): Claude (Flippers), Kat Tut (Pyramid). Warren (Lucky Spin) opens a slot-machine dialog.

## 3. Game loop and timing

- `WidgetContainer::UpdateAll` (0x4E92B0) calls each widget's `Update` (vtable +0x58, slot 22) once per WidgetManager update count, so game logic runs on fixed ticks.
- `Board::Update` (0x40E460) is the game tick. `Board` has **no** variable-rate `UpdateF` override. Per-board tick counter at raw `Board+0x8D4`.
- **The app's update counter is a second clock the simulation reads.** `SexyAppBase::mUpdateCount` at **App+0x484** is incremented once per update by `0x546450` (before the WidgetManager update), whether or not anything else moves on. Animation curves measure time with it: start count at `curve+0x48`, value from `0x435280` as `(count - start) * rate + start_value`, capped at `curve+0x70`. `Board::Update` skips its logic while its fade-in curve (`Board+0x138`, progress from `0x435410`) is below 0.2, and `0x45C2C0` acts on `count % 10`. Two computers running the same board ticks can therefore still diverge if their counters advance differently.

## 4. Randomness

Two generators feed gameplay, and both must be shared for the two computers to stay identical:

- **Sexy's MT19937** (`Sexy::Rand`): state `0x704268`, index `0x704C28`. A static initializer seeds it with 4357, then
  `SexyAppBase::Init` reseeds it with `GetTickCount()`. Every caller goes through the 5-byte `JMP` stub at **0x56B510**
  (→ 0x566C40 → `MTRand_Next` 0x566C50, which returns `tempered & 0x7FFFFFFF`). About 150 call sites.
- **The C runtime's `rand()`** at **0x636203** (MSVC LCG, per-thread state at `_getptd()+0x14`). 91 call sites, including effects
  that run every tick (the orange, green and purple peg choices use `Sexy::Rand`).

Effect particle counts depend on whether 3D acceleration is on (getter `0x54FD60`: `app->mDDInterface+0x38`), so the number of
random draws per tick differs between a 3D and a software-rendering PC unless that query is pinned during the simulation.

Pegs live in sets **ordered by memory address** (e.g. `LogicMgr+0x41C`, a `std::set`/`map` keyed by the peg pointer; the purple
peg is "the k-th candidate"). Identical random numbers therefore pick different pegs on two computers unless the simulation's
allocations get identical addresses. The game's CRT allocators: `malloc` 0x63A0E5, `free` 0x637FE8, `calloc` 0x63A057,
`realloc` 0x63949E, `_msize` 0x6439E3, `operator new` 0x634E7A, `operator delete` 0x634D4B.

Wall-clock time is not used by gameplay (only the level editor and app setup call `GetTickCount`/`timeGetTime`).

## 5. Key objects

Offsets are raw byte offsets from the object start.

**App (`ThunderballApp`, global pointer at `0x6CBE10`)**

| Offset | Field |
|---|---|
| `+0x320` | `WidgetManager*` |
| `+0x343` | windowed (registry `ScreenMode` 0) |
| `+0x350` | main `HWND` |
| `+0x36C` | `DDInterface*` (`+0x60` primary surface, `+0x68` 800x600 draw surface, `+0xCE0` HWND; surfaces are `IDirectDrawSurface` v1) |
| `+0x4CE` | app active (`WM_ACTIVATEAPP`) |
| `+0x7F4` | game mode: 1 Adventure, 2 Quick Play, **3 Duel**, 4 Challenge, 6 demo |
| `+0x7F8` / `+0x7FC` | stage / level index chosen on the level screen |
| `+0x864` | current `Board*` |
| `+0x870` | `LevelScreen*` (a new one each time it opens: ctor `0x4B8820`; its stage page `+0xA8`) |
| `+0x934` | player profile (`+4` name, `+0x2C` stage reached, `+0x38` the level screen's stage page, `+0x3C/+0x40` last duel masters, `+0x48` "play the computer", `+0x4C` difficulty) |

Vtable `0x68670C` (slots: 25 NewDialog, 49 game-already-running, 74 DoDialog, 75 GetDialog, 77 AddDialog, 79 KillDialog,
93 DrawDirtyStuff `0x547260`, 94 Redraw). The main loop (`0x54C650`) calls DrawDirtyStuff once a frame after the update
steps; it draws the widgets that changed and only then calls Redraw (else once a second), so a still screen such as the
in-game Menu over a paused board gets one Redraw a second. Redraw is also called from `WM_PAINT`. DialogListener vtable `0x6868C0` (slot 1: `DialogButtonDepress(dialogId, buttonId)`, buttons 1000 yes/OK, 1001 no/Cancel).

**Board** (`sizeof 0x988`, Widget vtable `0x6864A4`, ButtonListener vtable `0x6865C0` at `+0x88`: button 0 Menu, 1 Instant Replay)

| Offset | Field |
|---|---|
| `+0x10` | `WidgetManager*` |
| `+0x57..+0x59` | widget flags (focus, down, mouse-over), polled by `Update` |
| `+0xB0` | `App*` |
| `+0xB4` | game mode |
| `+0x700` | `AIMgr*` |
| `+0x71C`, `+0x728` | overlay widgets |
| `+0x720` | `LogicMgr*` |
| `+0x72C` | results dialog (`EndLevelDialog`, buttons: 1000 Main Menu, 1001 Replay Level, 0 Play New Level, 1 Change Character) |
| `+0x730` | Lucky Spin `SlotMachineDialog` (own `Update`, applies the result after 140 frames; `MouseDown` skips/unpauses) |
| `+0x734`, `+0x738` | Menu and Instant Replay buttons |
| `+0x788` | pause counter, changed by `0x407010` (ESI = board, stack bool): menu, focus loss, Space, Lucky Spin |
| `+0x8D4` | update counter |

Widget vtable slots: 3 AddWidget, 4 RemoveWidget, 17 MarkDirty, 22 Update, 26 Draw, 31 SetVisible(bool), 37 SetDisabled(bool),
40 Resize(x,y,w,h), 47 KeyChar, 48 KeyDown, 49 KeyUp, 50 MouseEnter(), 51 MouseLeave(), 52 MouseMove(x,y), 54 MouseDown(x,y,clicks),
56 MouseUp(x,y,clicks), 58 MouseDrag(x,y), 59 MouseWheel(delta), 68 Contains(x,y). (Ghidra's `vfunctionN` is slot N-1.)
A plain Widget is 0x88 bytes: constructor `0x4EABC0` stdcall(this), vtable `0x68FABC` (70 slots).
Going back to the main menu (`0x42FBA0`) dynamic_casts every widget on screen (looking for an OptionsDialog), so a widget
with a vtable of our own needs the RTTI pointer that MSVC keeps in the slot before the vtable, or the cast faults.
Widget `+0x08` head node of the children list (`{next, prev, value}`), `+0x10` WidgetManager (null when not on screen),
`+0x14` parent (children are placed relative to it), `+0x30/+0x34/+0x38/+0x3C` x/y/w/h, `+0x40` has alpha (what is
underneath is redrawn first), `+0x50` z-order (siblings are kept sorted by it; dialogs have 0, and everything under an open
dialog ignores the mouse), `+0x54/+0x55/+0x56` visible / takes the mouse / disabled, `+0x57..+0x59` focus / down / mouse over,
`+0x60` colors (vector data). Every class keeps its RTTI, so a widget's class (and its bases) can be read from its vtable.
Dialogs keep their buttons and text boxes as top-level widgets next to them, not as children.
ButtonWidget `+0x88` id, `+0x8C` label, `+0xAC` font, `+0xC0` image cells (Rect[4]: normal, mouse over, pressed, disabled),
`+0x104` ButtonListener; colors 0/1 are the label and the highlighted label. Round image buttons like Menu come from
`0x4299E0` cdecl(id, listener, image, bool); the board's own Menu and Instant Replay don't draw themselves (`+0x148` = 0).
Checkbox `+0x88` listener, `+0x8C` id, `+0x90` checked. Slider `+0x88` SliderListener (slot 0 `SliderVal(id, double)`),
`+0x90` value (double 0..1), `+0x98` id, `+0xA4` dragging, `+0xB0` horizontal. ListWidget `+0x8C` id, `+0x94` scrollbar,
`+0xA0/+0xA4` lines (vector of 0x1C-byte strings), `+0xC0` first line shown (double), `+0xD0` highlighted, `+0xD4` selected,
`+0xD8` listener, `+0xEC` line height; lines start 4 pixels down; a click selects the line under the mouse (set by MouseMove).
EditWidget `+0x8C` text, `+0xF0` max length; slot 70 `ProcessKey(key, char)` is what KeyDown and KeyChar feed, and a
character also needs a glyph in the box's font and the listener's AllowChar.

**Options dialog** (`OptionsDialog`, Widget vtable `0x68EFAC`, ButtonListener `0x68F0E0`, built by `0x4C2D20`): its `Resize`
(slot 40, `0x4C3860`) lays everything out again, relative to each other (`0x4EB760`, `Widget::Layout`), whenever it moves; it
slides in and out. Sliders and checkboxes go top down; the rows of buttons go upwards from the bottom one (Done / Back To Game,
`LAY_Above`), so a taller dialog (`GetPreferredHeight`, slot 73) moves them down and leaves a free row under the checkboxes.
Checkboxes at `+0x18C` Fullscreen (id 3), `+0x190` Custom Cursors, `+0x194` 3D Acceleration (id 5), `+0x198` Colorblind Mode,
2 columns of 182x34, rows 34 apart. Buttons below the checkboxes: 7 Help, 8 Demo (main menu), 9 Main Menu, 10 New Level,
11 New Challenge, 12 All Clear Mode, shown per mode, the last row one button across the dialog. Closing it (`0x42FDD0`, from
Done and the other buttons) applies the settings: the app's slot 70 `SwitchScreenMode(windowed = !Fullscreen, 3D, force)`.
Checkboxes come from `0x429E40` (ECX id, EAX CheckboxListener*, EDI style 0, EBX label std::string*; the width follows the
label); `Checkbox` slot 70 `SetChecked(bool, bool tell)`, 71 `IsChecked()`, `+0x90` checked; a click calls the listener's
slot 0 `CheckboxChecked(id, checked)`. **EditDialog** (the game's name box, `0x4ADB70`): its EditListener sits at `+0x178`
(vtable `0x68E7B8`); slot 2 AllowChar (`0x4AE000`) takes only letters, digits, space and `_-()`, as for player names.

**Drawing**: `Graphics` `+0x20` clip rect, `+0x30` color, `+0x40` font, `+0x44` draw mode, `+0x48` colorize images.
SetFont `0x51AD80` (EAX g, ECX font), SetColor `0x51AD90` thiscall(g, const Color&), FillRect `0x51AE20` (EAX g,
stdcall(x, y, w, h)), DrawString `0x51B750` thiscall(g, const string&, x, baseline y). Font slots: 1 ascent, 4 height,
6 line spacing, 7 StringWidth(const string&), 10 DrawString. Resources sit in globals filled at startup: fonts `0x6CA4EC`
PLAIN, `0x6CA6F4` OVERLOAD12 (the round buttons' lettering); sounds `0x6CA8C0` BUTTON1, `0x6CA544` SPEECH_BUBBLE_POPUP,
played with PlaySample(id), slot 56 of the app at `0x6CBE04`; image `0x6CAAA0` DLG_PURPLEBUTTON (the Menu button without
its lettering, three cells: dark as Menu looks on the board, lit, bright; unused by the game). The loader in `global/00420000.c`
assigns each global right after naming it, so the rest can be read off there (CANNONSHOT `0x6CAA44`, PEGHIT `0x6CAA28`,
FREEBALL `0x6CA388`, FEVERHIT `0x6CA954`, ...; the ones the controller's vibration uses are in `mod/src/pad.cpp`). Every
sound the game starts goes through `DSoundManager::GetSoundInstance(id)` (vtable `0x696DAC` slot 8).

**WidgetManager**: `+0x84` window has the keyboard focus (ButtonWidget::MouseUp only fires while it's set), `+0x88` focus widget,
`+0x8C` widget the button went down on, `+0x90` widget under the mouse, `+0xE0/+0xE4` mouse x/y, `+0xE8/+0xEC` button masks
(bit 1 = right button: 8× physics sub-steps while held), `+0xF4 + key` `bool` key down (held Up / Down turn the aim every
physics step). Input entry points: MouseMove `0x4EA970` (EAX x, ECX y, EDX wm), MouseDown `0x4EA860` (ECX wm, push x, EAX y,
EBX clicks), MouseUp `0x4EA7B0` (EAX wm, push clicks, push x, EBX y), KeyDown `0x4EAB30` / KeyUp `0x4EAB70` / KeyChar `0x4EAAE0`
(EAX wm, ECX key or char; to the focus widget), GetWidgetAt `0x4E9A80` (EAX y, ECX x, EDX wm, stdcall(int* local_x,
int* local_y); what a click there reaches, null if disabled or under an open dialog). Vtable slot 8: SetFocus(widget).

**Mouse pointer**: the app's `EnforceCursor` is vtable slot 12 (`0x54C2D0`), always called through the vtable; the pointer it
wants is at app `+0x4B0` (11 = none, which hides both the system pointer and the game's drawn one).

**Mouse remapping**: every mouse message passes `WidgetManager::RemapMouse` (`0x4E9DB0`, ECX wm, x and y by pointer) on its
way in: a point in `from` (wm `+0xCC`, x y w h, client pixels) lands at the matching point of `to` (wm `+0xBC`, the game's
800x600). `0x4EA0D0` sets both; the display setup (`0x54CDE0`) fills them from a presentation rect at app `+0x5DC` (centered
when the flag at `+0x5D9` is set and the window is bigger than the picture; PopCap's unused widescreen letterboxing, unscaled).

**Mouse in the window**: every WM_TIMER (10 ms, `ProcessDeferredMessages` `0x54A980`) the game decides whether the mouse
is in its window: `WindowFromPoint(GetCursorPos())` is the window and the point is inside `ClientToScreen` of (0, 0) and of
the DDInterface's size (`+0xA0/+0xA4`, 800x600, loaded at `0x54B137`/`0x54B141`). App `+0x4CC` holds the answer; going
out it sends the WidgetManager MouseExit and calls EnforceCursor. Every mouse message (`0x54B2xx`) sets it again. When
the client area is bigger than 800x600 (the mod's scaled window and borderless) the two disagree beyond 800x600 and the
flag flips with each message: the pointer flickers and clicks get lost, so the mod measures the client area there.
The **cursor thread** (`0x54BFF0`, started at launch) polls `GetCursorPos` + `ScreenToClient` (the call at `0x54C050`)
every 10 ms into DDInterface `+0xD0C/+0xD10` (the next frame's pointer; `+0xD14/+0xD18` where it is drawn, `+0xD1C` its
image, set while app `+0x500` Custom Cursors is on and the mouse is in). While the game isn't drawing, it draws the
pointer straight onto the primary (`0x4F8450` -> `0x4F7960`: a small Blt to the screen, the area under it saved and put
back); otherwise the next Redraw composes it into the frame it presents. App `+0x501` makes DrawDirtyStuff draw a frame.

**Display**: the game process is DPI unaware, so on a scaled desktop Windows stretches its 800x600 window (1000x750 at 125%).
In windowed mode the frame goes to the screen by a Blt to the DirectDraw primary, which shows nothing on some PCs; a Direct3D 9
device presenting to the game window is invisible the same way, but one presenting to a child window of it shows.

**LogicMgr**

| Offset | Field |
|---|---|
| `+0x04` | state: 9 choosing masters, 1 aiming, 2 ball in flight, 3 hit pegs clearing, 6 turn passing, 5 duel over (also an Instant Replay that has finished), 10 a Zen Ball choosing its shot (between 1 and 2) |
| `+0x1A8` | two-player flag |
| `+0x1AC` | **current player index** |
| `+0x208 + 4·i` | balls left for player i |
| `+0x298 + 4·i` | Zen Balls to come for player i (Master Hu's power): firing with one goes to state 10, where `0x4766A0` chooses the shot and launches it at `0x476B15`; a normal shot launches at `0x4770CE` (both call `0x46C230`) |
| `+0x210 + 4·i` | master for player i |
| `+0x2D8 + i` | player i is computer-controlled |
| `+0x41C` | peg set (address-ordered) |

## 6. Input path

The WidgetManager calls the board's input slots; the board forwards to `LogicMgr` (`MouseMove` 0x472190, `MouseDown` 0x472240,
`MouseUp` 0x472810, `MouseDrag` 0x4721D0, `MouseWheel` 0x4728F0). `KeyDown`: Enter is a click (slot 54 at the mouse position,
and `KeyUp` releases it), Left/Right one wheel notch each. `KeyChar`: Space toggles pause; typed letters feed a secret-word
matcher. `Update` also polls the mouse position, button masks, key array and the board's widget flags directly.

Aiming: `MouseMove` aims the cannon at the mouse (sets `LogicMgr+0x20`, unless `+0xF4` counts down). `MouseWheel(delta)`, only
while aiming, turns it by delta × a step in degrees: 0.1 for |delta| ≥ 2; for single notches the step adapts to their pace
(between 0.01 and 0.1; the cannon's `+0x17C` is the time since the last one, `LogicMgr+0x7C` the last step), and each wheel
event blocks mouse aiming for 30 ticks (`+0xF4`). The aim angle (`LogicMgr+0xEC`) is in radians; a larger value is further
right. A left click while aiming only sets the fire flag (`+0xF9`); it doesn't aim at the click. Left button down/up also
raise and drop Claude's flippers (`0x472060`). The board ignores clicks with the mouse over its bottom corners (Menu, Instant
Replay; worked out in `Update`) and inside the rectangles listed at `Board+0x38`.

Some buttons open a dialog that runs a nested update loop until it's closed (the main menu's Replays). Input delivered from
the app's `DrawDirtyStuff` comes back to `DrawDirtyStuff` inside that loop, so whatever delivers it mustn't hold a lock
across it, and the nested frames have to work normally (the mod delivers its input last in each frame).

## 7. Duel flow

Main menu Duel button (`MainMenu` ButtonListener vtable `0x68D824`, slot 2, id 0x66) sets mode 3, checks for a saved duel
(`0x430380`), then `0x4C1EA0` (EBX = MainMenu) opens the **level screen**: on the profile's page (`+0x38`, kept between sessions
and written on every turn of the page, not the app's stage `+0x7F8`). Picking a level (`0x4BA370`) stores stage/level in the
app and calls `NewBoard_Other(1, 0)` (`0x42E1D0`) at `0x4BA407`. The board is created and the **Two Player Selection** dialog
(`CharacterDialog`, id 1, ctor `0x4AA800`, opened by `0x4067C0`) appears over it: ButtonPress ids 0–11 left column, 12–23 right
(11/23 Random), 100/101 edit names, 1000 Play Now (`0x4ACC60` applies the masters, then `SetupPlayers` and the coin flip).
Name strings at dialog `+0x1C0`/`+0x1DC`; per-player master vectors at `+0x22C`/`+0x23C` (12-byte entries, `selected` at +4).
Widget pointers from `+0x190`: the two name EDIT buttons, Play a Friend, Play Computer, then the computer's Easy, Normal,
Hard and Master (ThunderCheckboxes). Masters past the profile's progress are locked.
The duel ends on the results dialog (`Board+0x72C`).

## 8. Online multiplayer (implemented in `mod/`)

Deterministic lockstep, as built:

| Piece | How |
|---|---|
| Loading | Proxy `bass.dll` (original kept as `bass_orig.dll`). The launcher loads `steam.dll` from PATH and runs the game as `C:\ProgramData\PopCap Games\PeggleNights\popcapgame1.exe -changedir=<install dir>`, which loads `.\bass.dll` from the install folder. |
| Menus | Native dialogs via the game's own `DoDialog`, `ThunderButton` factory `0x4298D0`, `EditDialog` (`0x4ADB70`). |
| Session | Host picks the level; both create the board inside the synchronized scope with the host's seed and FPU control word. |
| Levels | Level packs (`levelpacks/*.pak`) add stages after the 12 of `stages.cfg`, so a stage number means nothing to the other computer and the level may not exist there: a board for a stage that doesn't exist stays empty and black, in state 0, with no selection. After Hello each game sends the names of all its levels (`kLevelList`; LevelManager `app+0x910`, stages `+0x10`, a stage's levels `+0xA8`, a level's name `+0x1C`), `Level` carries the name and the joiner finds its own stage and level by it. The level screen's Random (button 0x67, `0x4BA370` with no level given) picks again among the shared levels up to the profile's stage. |
| Level packs | A level needs its `.dat`, its `.jp2` background, the pack's shared images and `<pack>.cfg`, so a level the joiner lacks comes over as its whole pack, one `.pak` (magic, version, per file flags, name length, name, size, time, then the files, all XOR 0xF7; flag 0x80 ends the table). The host finds the pack whose table holds `.../<level>.dat`. The joiner is asked (`kPackOffer`, `kPackReply`); the pack goes in 16 KB pieces on a thread of its own, so the game thread's messages wait for one piece at most, into `<name>.pak.part`, which the game doesn't see. The table and a CRC are checked, it's renamed, and `0x47A940` (stdcall(LevelManager)) loads it at once, with no "New levels installed!" dialog: the main menu calls the same thing about once a second and only shows that when it finds something new. The joiner sends its level list again, then `kPackDone`, and the host starts the level it picked. |
| Hosting | A forwarded port gets callers that aren't an opponent, so the host keeps listening until one says hello within 5 s; anything else is dropped without the session noticing (one such connection used to end the hosting). When UPnP fails the worker asks a public service for the address and connects to it: a connection that comes back through the router proves the port is open (not every router loops back, so none proves nothing). UPnP-capable routers can advertise and still never answer: SSDP replied here while `GET` on the advertised TCP port stayed silent, and NAT-PMP (UDP 5351) didn't answer either, so a hand-forwarded port is the only way in on such a router. |
| Control | Decided from synchronized state only: the host until Play Now and for everything between duels (state 8 level intro, 9 choosing masters, 5 duel decided and results), otherwise the player whose turn it is. Replay Level goes 5 → 8 → 9, i.e. back through the intro to the master selection. Dialogs can't decide it: they close on each computer's own clock as they slide away (relying on that deadlocked Replay Level). The controller runs ticks freely and streams input events, polled input and tick markers; the other replays them. |
| Shared clock | While a duel runs, the app's update counter is held at a shared start + the duel's tick. The host picks the start above both counters (each side sends its counter in Hello), so neither runs backwards. |
| Shared randomness | `Rand` stub and CRT `rand()` redirected inside the simulation; 3D query pinned to "on". |
| Shared addresses | A deterministic allocator at a fixed base serves the simulation's allocations under 64 KB on the game thread (bigger ones are buffers, pixels or sound, and go to the normal heap). Small blocks (≤ 1 KB, the game's objects) freed by the simulation are reused; blocks freed outside it are not (their timing differs per computer), and blocks over 1 KB are never reused, since they are mostly images the game frees on its own clock (purged after drawing, freed on Replay Level on one computer and earlier on the other). std::string buffers (allocated at `0x402295`) stay on the normal heap: nothing orders by them, and their sizes depend on the computer, since image paths hold the game's folder. Two players with the game in different folders differed from the first tick until then. Blocks of an earlier duel aren't reused either: a computer that has played frees some of them while it builds the next board, one that has just started has none, and their free lists (and so everything after) would differ from tick 1. An object freed inside the simulation on one computer and outside it on the other leaves different free lists, and a later allocation lands at a different address. |
| Presentation | Presentation code the simulation calls runs with the synchronized scope paused (own random numbers, ordinary heap): the level music playlist `0x4082B0` (shuffled with `rand()` on the session's first board), sound instances (`DSoundManager::GetSoundInstance`, vtable `0x696DAC` slot 8; an instance releases itself once played, on the sound manager's clock or in the next request), animation curves set from text `0x434630` (parsed once per session into a global map, 32 KB per curve), the board frame skin `0x4634A0` (rebuilt only when the level theme changes) and the intro title card `0x40C0A0` (EBX = board). Each of these made one computer allocate, free or draw random numbers where the other didn't, typically when one player had already played a level that session. |
| Per player | What differs per player mustn't reach the simulation: `NormalizeProfile` zeroes the fields board creation reads, and "show the Duel Rules" (profile `+0x10C`, which opens a dialog before the selection in a player's first duel). The duel's end (`0x40A440`, called at `0x46BEC7`) updates the player's records, saves the profile if anything in it changed (profile `+0xEC`) and builds the results (`0x4B15D0`), which depend on the profile: a trophy waiting (`0x41B270`) puts other strings on them. All of that allocates per player, so it runs with the synchronized scope paused. Before that every online duel between two real players differed on the first tick of state 5 (a trophy waiting on one side reproduces it; two test copies sharing a profile never showed it). |
| Local dialogs | `app+0x32C` is the number of open dialogs (the size of the app's map of them, `+0x328`), and the simulation reads it: `0x473490` holds the intro's countdown to the selection while any is open, `Board::Update` two things more. One computer's Menu, Leave Game and the mod's own dialogs don't exist on the other, so during a tick they're taken off the count. A message box over the level start used to stall one game's intro for as long as it stayed up: a real divergence, and the mod's own OUT OF SYNC box caused it. |
| Screen transition | A new board holds its intro back (no curve steps, so the fade-in and then the logic start later) while it is in the app's screen-transition list (`app+0x920`; `Board::Update` asks `0x4338C0` at `0x40E499`, EAX = list, stdcall(widget) → AL). That list follows each computer's own frames: the wipe from the previous screen ended a tick apart on the two computers now and then, shifting the whole duel by a tick. The online board counts as in transition for exactly its first 20 ticks instead (about the native wipe). |
| Setup input | The board is built with a neutral input state (mouse at 400,300, nothing pressed): the cannon starts out aiming at the mouse. The Instant Replay button's mouse-over flag is part of the polled input, since holding the right button fast-forwards only when the mouse isn't over it. |
| Other widgets | Lucky Spin slot machine ticks inside the board tick and its clicks are streamed; results buttons from the host are streamed. Local pauses (menu, focus) are ignored; Instant Replay and the menu's level-changing buttons are disabled online. |
| Chat | The CHAT button and the chat box are the mod's own top-level widgets (z-order 1000, above dialogs, so chat works over the results too). Their clicks never reach the board, so they stay out of the stream; a phrase travels as a message of its own (`kChat`, an index into the list). While the phrase list is open a whole-screen widget takes the mouse, so a click beside the list closes it instead of firing. The board only notices the pointer leaving it, which the stream carries like any other move. |
| Controller | XInput, or while that has none a DualShock 4 or DualSense read straight through HID (below), read per frame and turned into the input a mouse makes, so the duel streams it like any other. Menus: everything a click would reach (GetWidgetAt) of a button, checkbox, slider, text box or list class (by RTTI) is a target; the pointer moves onto the highlighted one, so the game shows its own hover look. Board: the pointer is parked mid-board, and the board's pointer input (enter, move, drag) is dropped before the stream while the controller is in use, since the game's own re-checks of what is under the pointer (a dialog closing) would aim the cannon at it; the sticks send wheel steps, A the left button (shoot, flippers), RT the right one (fast-forward). Online, the selection hides the offline controls and the controller skips buttons that do nothing for that player. Offline (a local duel between two people, two controllers) the board answers only to the current player's controller (`LogicMgr+0x1AC`) and each player gets a highlight of their own on the selection; not part of the stream. |
| Per-tick check | FNV hash of random-number counts, turn state and the allocator's counters after every tick; the first mismatch dumps both sides' recent per-tick trace (random counts, state, allocator counters, aim and cannon angles) to the logs. It's a diagnostic: bookkeeping can differ with the boards still the same, so the players aren't told. After a repair the allocator's counters are left out of it, since from then on they can't match (below). |
| Control | Only the game in control decides that control passes, from its own board, and it says so in the stream (`kItemControl`): the other game takes over at that very point, whatever its own board says. With each game deciding for itself, a board that had gone another way broke the duel. A shot over sooner on the game in control left each waiting for the other for good, with nothing on the screen to say so; over sooner on the other game, it took over early, and the two ran out of step with ticks left over in its buffer and nothing compared. A difference that changes how long a shot lasts is the one a bug makes: any different bounce does. |
| Turn check | As a shot ends (state 6, which every shot comes through on its way to the next turn or the results) and as a turn begins (state 1) the game in control hashes the board's whole copy, the game's own for Instant Replay and saved games (`0x405D00`, stdcall(board) → a DataWriter, about 8 KB: pegs, movers, bucket, scores, balls, powers, LogicMgr), with the random numbers and the modes' state, and puts the result in the stream (`kItemTurn`). The other game compares at that same point of the stream and answers (`kTurnReply`). The one thing left out is the master display's flash (`Board+0x704`, `+0x30`, set to 239 by `0x453BD0`), which runs on each computer's drawing. A game that gets to a check of its own before the stream does applies nothing more until the stream gets there, so a shot that ended sooner on it stops there and can't run on into a turn of its own or the results. The game in control doesn't let the turn pass without an answer: it waits 16 logic steps short of the 150 the passing lasts at the least (`0x4763D0`; fast-forward runs 8 a tick). |
| Repair | When the boards differ the other game asks for the board (`kTurnReply`) and applies nothing until it comes. The game in control, whose player took the shot, sends it in the stream (`kItemBoard`: the board's bytes, then what the modes hold beside it: their state, what's decided for the next turn, Mirage's blackout), as soon as no ball is in play there, so the shot as that player saw it stands, score and all. Both start a clean arena region (the sender picks the address), seed the random numbers alike, take the sender's tick (the clock follows it), set the object number at `0x6CBDD0` to that tick (every PhysObj made takes the next number, a level starts it at 0; it isn't in the board but a new ball's number is, so a game that waited for the board and made fewer objects would otherwise differ at every turn start for the rest of the level; only `AIBall` compares them) and load those bytes (`0x405DB0` with no shot records, inside the synchronized scope; it can go over a ball in flight, as Done does over a replay), then `chaos::LoadState`. The game taking a board logs where its own differed before loading it (`board` and `modes`: offset, both sides' bytes, the bytes before), which is the only place both are held. The next tick checks again. After three in a row with no match between, the game plays on without one until a check matches; the players are never told, it only goes in the log. The load doesn't undo everything a difference did: vectors in what outlives it stay as big as they had grown (`LogicMgr+0xC0`, 16 bytes each, its ball records at `+0xE0`, the physics manager's lists at `+4` and `+0x14`), so after a repair the two games ask for memory at different moments and the allocators no longer match. That changes nothing: addresses only matter in the pegs' sets, which a load builds in the same order on both. |
| Diagnostics | With `mods\debug.txt`: setup's C-runtime `rand()` callers in the log; `mods\logs\allocs-<pid>.txt` (every allocation of setup and the first 1000 ticks, with tick markers) and `frees-<pid>.txt` (every arena free of the same stretch, with tick, context and allocation number); the opening's input events and per-tick trace (up to tick 64) go to the log. Allocation numbers match between the two computers until they diverge, so joining the two free records by number shows an object freed in different contexts. The test pipe's `dump <path>` writes the board, its LogicMgr and cannon for diffing at a held tick. |

Other timing facts found on the way: `Board::Update` skips its logic while the board's fade-in curve (`+0x138`) is below 0.2; holding the right button is the game's fast-forward (`0x40E722`): 8 logic steps per update, unless a ball is in flight (state 2; the 4 steps meant for that need `Board+0x6E7`, which only the constructor writes, as 0), the mouse is over Instant Replay or the trial's upsell screen is up (`0x6CBD79`); during an Instant Replay (`LogicMgr+0x101`, its panel the `ReplayDialog` at `Board+0x728`: Instant Replay, Next, Prev, Save, Done) it is 4 steps with the mouse over the board; and `+0x7A0` queues extra steps (set only by the upsell screen's scripted demo). `Board+0x794` counts the logic steps run. `LogicMgr` state 8 is the level intro; `FUN_00468190` (ESI = LogicMgr, EDI = state) sets the state and builds the title card on entering 8.

## 9. Board objects, physics and drawing (what Chaos Mode builds on)

Everything on the board is a `PhysObj`: the shot ball and round pegs are `Ball`s (vtable `0x688974`; the computer's look-ahead `AIBall` `0x688064`), bricks `Brick`s (`0x688D7C`), walls and decorations `Poly`s (`0x688B3C`), catch areas `Hole`s (`0x688C5C`). `Board+0x760` lists them all (head pointer `+0x764`; nodes `{next, prev, obj}`), `Board+0x76C` the balls in play (head `+0x770`, count `+0x774`).

| PhysObj | Field |
|---|---|
| `+0x04` | reference count (containers hold counted references) |
| `+0x10` | kind: 1 Hole, 2 shot ball, 3 round peg, 4 line, 5 poly or brick |
| `+0x14..+0x20` | bounding box (left, top, right, bottom; floats, playfield pixels) |
| `+0x24` / `+0x25` | collides / alive (on the board) |
| `+0x48` | `Mover*` for objects on a path |
| `+0x5C` | ID from the level file (`std::string`: "wall", "freeball", ...) |
| `+0xD0` | `PegInfo*`: set on pegs only |

Slots: 17 SetPos(x, y), 19 SetVelocity(vx, vy), 22 Update, 23 draw position between steps, 24 shadow and glow, 25 Draw, 26 overlay (the draws take `Graphics*`), 29 GetPos, 30 GetX, 31 GetY. The playfield is `Board+0x778..+0x784` (75, 40, 650 x 560): objects live in playfield pixels, drawn translated by (75, 40). About 100 logic steps a second.

**Balls**: position `+0xEC/+0xF0` (previous `+0xF4/+0xF8`, drawn at `+0x134/+0x138`), velocity `+0xFC/+0x100` in pixels per step, radius `+0x13C` (6; Lord Cinderbottom's fireball 11), `+0x140` in the cannon, `+0x165` fireball, `+0x16D` bounces off other balls; `0x1AC` bytes, constructor `0x491AC0` (ESI this, BL 0 a ball, 1 a round peg). `0x492530` SetRadius (EAX ball, stdcall float; a radius other than 6 draws the stretched `IMAGE_BIGBALL`), `0x492350` teleport (ESI ball, stdcall(x, y); keeps the velocity, resets the trail). Multiball's recipe (`0x46C400`): new, construct, reference, SetPos, SetVelocity, `0x454390`(PhysMgr `Board+0x708`, its `+4` and `+0x14` axis lists), `Board::AddObject` `0x40A6F0` (thiscall), release.

**Physics step** `0x40DE20` (EDI = board, every logic step): each object's Update; `Ball::Update` adds the gravity with `fadd [0x6C77CC]` (0.05) at `0x493B84` (`fsub` is `D8 25`) and caps the speed at 15 pixels a step (`0x493C8C` compares with the double at `0x69A518` = 225, `0x493CB0` loads 15 from `0x69A240`); then each ball moves (`0x4937D0`) and is checked against the playfield's edges from `0x40DF81` (ESI = ball; a ball more than 10 pixels out is removed); then `0x471620` processes the step's hits and catches (the call at `0x40E104`). After every bounce the physics calls the function at `[0x6CBDD4]`, cdecl(ball, obj): the real step installs `0x4546D0` (the immediate at `0x4547E8`), which records the pair; look-aheads install their own. There is no wall code: `levels\_base.dat`, merged into every level, has three "wall" Polys (left centred at x -20, right 669, ceiling y -3).

**Pegs**: `PegInfo` `+0x10` type (1 blue, 2 orange, 3 purple, 4 green), `+0x14` lit, `+0x15` the level lets it change color. `0x4A04D0` SetPegType (ECX peg, EDX type; a brick redraws itself), `0x49E080` flash (EAX PegInfo, ECX 2: became special). LogicMgr `+0x3F4` lists every peg in the level's order (head `+0x3F8`), `+0x40C` and `+0x418` are `std::set`s (by address) of the unhit orange pegs (their count drives the game) and of the rest, `+0x400` the pegs hit this shot, `+0x1FC` the purple peg, `+0x102` Extreme Fever has begun. Set erase `0x47EC20` (ESI set, ECX &peg, EAX the count erased), insert `0x43DFA0` (EBX 12-byte result, stdcall(set, &peg)). Each turn `0x469600` (stdcall LogicMgr) moves the purple peg (it adopts another type-3 peg if there is one). `PegHit` `0x4701B0` thiscall(LogicMgr; ball, peg, indirect): lights, scores, removes from its set. Splork's Space Blast `0x46C610` (EAX peg, stdcall(LogicMgr, ball)) lights every peg within 100 pixels, with its explosion. A hit peg pops at the end of the shot; pegs are never freed during a level. Popping only clears `+0x24` (collides) and `+0x25` (on the board) (`0x49F6C0`): a gone peg stays in the board's list, and its Update (`0x4A06B0`) keeps moving it along its Mover. `0x468DD0` (EAX LogicMgr, EDI font, -1 for 0x3B; stdcall(const std::string*, float x, float y), callee-clean, returns the FloatingText) floats a text up over the board, as when a power's peg is hit (`0x46EFBE`: 40 pixels over the peg, then `+0x60` RGB 0x88FF88, `+0x68` 150 ticks, `+0x6C` 0xFFFFFFDD, `+0x78` 10, `+0x8C` -0.5 pixels a tick); SOUND_POWERUP is `0x6CA8B8`.

**The bucket**: five "freeball" parts from `_base.dat` (a Hole 130 x 20 that catches and draws the body, two invisible lips, two drawn Polys), Kat Tut's pyramid version ("bumperhole" x 5) and the lid ("freeballcover", a Brick). Each part has a Mover (`+0x10` type, its sign the direction; `+0x14` tick offset; `+0x18` radius, int, 260; `+0x1C` period 600; `+0x20` phase 0.25; `+0x54/+0x58` where it is; `+0x5C/+0x60` base): x = base + radius * cos(2 pi f), f from the object's tick plus the offset. Hole centre `+0xF4/+0xF8` (floats), size `+0x104/+0x108` (ints, 130 x 20). A ball whose centre is in a Hole goes to `0x4713C0` (ECX LogicMgr, ESI hole, stdcall(ball); the step's catches call it at `0x471906`), which for "freeball" calls `0x470EA0` and removes the ball; that puts its texts ("Bucket Bonus!", a style shot's) 60 pixels over the Hole's GetX/GetY. Extreme Fever drops the bucket out of view (mover type -11) and raises its own scoring Holes. The track's width eases towards `LogicMgr+0x1B8` (0 when there): the tail of `0x46AA40` sets it to 130 while the player in turn has Flippers, 200 with the Pyramid, else 260, and `NextTurn` runs that for every power each turn; `0x472B10` (stdcall(LogicMgr), called by the logic step at `0x47727D` while it's set) moves every part's radius 1 pixel towards it on odd ticks and clears it once they're all there. Which parts only collide: "freeball" Polys with byte `+0x28` 0 (the lips), "bumperhole" ones with `+0xB0` 0 (the pyramid's two sides, bounce `+0x124` 1.15/1.17). Polys bounce balls by their `+0x34` (0: 0.82) unless the ball comes in slower than `+0x38` (0: 1.4; five times faster against an upright edge), when it slides along, and cap the speed at `+0x3C` (`0x492F90`). The lid (a Brick, 19 pixels over the Hole) is put on by `0x46A9A0` (EAX LogicMgr, stdcall(bool)) for a Fireball turn and when a Spooky Ball peg is hit (`0x46EC40`), off when its returns are used up (`0x472DAE`); a fireball hitting it sets ball `+0x145` and takes it off (slot 33 SetActive(0), not in a challenge whose `Board+0x6F4` has `+0xDD`) with SOUND_FIREBALLBOUNCE `0x6CAAD8` (`0x471893`). A Brick makes its pictures from its shape.

**Cannon** (`Gun`, a Poly at `Board+0xD4`): pivot `+0x114/+0x118` (325, 30), angle `+0x150` in radians (direction (cos a, -sin a): 3 pi / 2 is straight down; turned towards `+0x154` by 0.2 a tick, measured from pi / 2), loaded ball `+0x1C0`. `0x4689D0` aims at the mouse (EAX LogicMgr, stdcall(x, y), result in ST0; allows for gravity), `0x468D00` stores it in `LogicMgr+0xEC`, keeping out of 7..173 degrees (ESI LogicMgr, stdcall float), and the cannon follows each tick through `0x498C50` (EAX gun, stdcall(angle, immediate), the call at `0x476DE9`). Firing: `0x498E70` at `0x477089`, `Board::AddObject` at `0x4770C7`, the shot's bookkeeping `0x46C230` at `0x4770CE` (EBX LogicMgr; the ball at the caller's `[ebp-0x14]`). The guide is a temporary ball flown up to 200 steps (`0x4981C0`, part of the simulation) and drawn by `0x499D00` (ECX gun, EAX Graphics, stdcall(bool)). The flight starts at `+0x158/+0x15C` with the angle `+0x150` and speed `+0x160` (radius: the loaded ball's, or the float at `0x6C6E0C`), records where the ball is after each step in the vector at `+0x1C8/+0x1CC` (x, y floats), and leaves the step of its first hit in `+0x198` (-1: none), the ball's position then in `+0x1A4/+0x1A8` and atan2(vy, vx) after the bounce in `+0x1AC`. The drawing (`0x499140`) puts dots at steps 6, 10, 14... from the vector (step s at `path[s - 3]`), 40 of them (80 with the Super Guide, `+0x194`; `0x499680`), until a step passes the hit's, then the marker `[0x6CAA2C]` at the hit (the Super Guide's turned by 3 pi / 2 minus `+0x1AC`).

**Slow motion and zoom** (the last orange peg's drumroll): `Board+0xDA` on, `+0xE4` ticks it holds, `+0xEC` speed in thousandths; physics steps are skipped while the accumulated speed is under 1000, and drawing interpolates. `0x471B80` (EBX LogicMgr) starts it: 10%, zoom in, SOUND_SLOWMO (not while a saved replay plays, `0x6CBD7A`); `0x408710` (EDI board, AL on, ECX speed) sets it. `0x493700` (ECX ball, EAX peg, stdcall(1.0, margin), result in AL): will the ball touch the peg this step (only geometry, round pegs or a brick's own shape, alive or not: the ball's `+0xEC/+0xF0`, velocity `+0xFC/+0x100` and radius `+0x13C`, so a stand-in works; the first argument is the share of the step). The zoom (`+0x114..+0x124`, `+0x6E5` asks for it) is a Direct3D transform, so it shows only with 3D acceleration on.

**Drawing**: `Board::Draw` `0x40C530` draws background objects, shadows, pegs, then (the call at `0x40C973`: EAX `Board+0x714`, EBX board, stdcall(&g2, -1)) the floating texts' back layer, the balls, foreground objects, peg glows, (the call at `0x40CBA2`: EBX board, stdcall(g, &g1 clipped to the playfield)) the intro title card, the cannon, the HUD frame, the guide, top objects and texts. The HUD draws the Menu and Instant Replay buttons itself (`0x465610`), whatever their visibility. Graphics: `+0x08/+0x0C` translation, `+0x10/+0x14` scale (images and image fonts) about `+0x18/+0x1C` (with the translation in: `0x51B8A0`, drawing at ints, stretches by it; `0x51BE40` doesn't), `+0x20` clip Rect (translated), `+0x30` colour, `+0x44` draw mode (1 additive; `0x51ADC0` EAX g, ECX mode), `+0x48` colorize images, `+0x4C` 3D. `0x51B130` PolyFill (ESI g, ECX count, stdcall(Point*, convex)). Transforms (3D only): `0x4FC180` push (EAX = `[[app+0x36C]+0x30]`, stdcall(matrix); multiplied onto the current one), `0x4FC220` pop; 3 x 3 float matrices: identity `0x5605D0` (EAX), translate `0x5605F0` and scale `0x5606B0` (ESI matrix, stdcall(x, y)).

**Spooky Ball** (Renfield): ball `+0x164` comes back when it falls out, `+0x146` has come back before (the sound);
LogicMgr `+0x290 + 4 * player` how many more times. Each logic step (after the physics; the call at `0x4749A3`)
`0x4731E0` (stdcall LogicMgr) finds balls whose top is below the playfield (`y - radius > Board+0x784`; the physics
removes a ball 10 pixels further on) and calls `0x472CB0` (stdcall(LogicMgr, ball)): a sound, one fewer, the speed
cut to 3, then up to three look-ahead flights from (x, 10) (a temporary Ball, collision callback `0x473100`, up to 400
steps or out of the bottom; the second and third with the x speed nudged by `Sexy::Rand`) until one hits something,
and the ball teleports to (x, 10) with that velocity (the float 10 is read from `0x69A084`, at `0x472EDB` and
`0x47307F`). An Extreme Fever bucket also sends a Spooky Ball back (`0x47157E`).

**Masters' powers** (`0x46E9C0`, thiscall(LogicMgr; ball, peg, power (0: the player's master's, `LogicMgr+0x250 + 4 * player`), bool from Lucky Spin)): 1 Super Guide, 2 Flippers (Claude), 3 Multiball, 4 Pyramid (Kat Tut), 5 Space Blast, 6 Spooky Ball (Renfield), 7 Zen Ball (Master Hu), 8 Shot Extender, 9 Flower Power, 10 Nudge, 11 Time Bomb, 12 Fireball (Lord Cinderbottom), 13 Lucky Spin (Warren), 14 Chain Lightning (Marina, shown as "Electrobolt"). Turns left: `LogicMgr+0x260 + 8 * power + 4 * player`. `0x46AA40` (stdcall(LogicMgr, power, bool on)) switches a power's objects; `NextTurn` (`0x46ACE0`) counts every power down for the new player and calls it for each, then readies the loaded ball: charged with Chain Lightning (`0x498E10`, ESI gun: ball `+0x166`), a fireball (`0x498E50`, EAX gun: ball `+0x165`, radius 11), the pyramid on (`0x46A850`, stdcall(LogicMgr, on)). The look-ahead balls (`AIBall`) update through `Ball::Update` (`0x44D187`), gravity and all.

**Flippers**: two Polys with id "flipper" at the end of `_base.dat`, shown by slot 33 SetActive. Their shape is points about the pivot (`+0xE8/+0xEC`, a std::vector of x, y, the first again at the end), turned by `+0x11C` (`+0x120` its change a step, `+0x128` the angle drawn; the picture centred at the pivot plus `+0xB8/+0xBC`); the Mover (type 9 or -9, the sign the way it turns) sets its `+0x64` to sign * (swing) + `+0x44` and keeps the pivot at its base. `0x472060` (LogicMgr, bool up), from the left button, runs the swing (15 steps). The ball is hit by the general rotating-body bounce. Edges are one-sided: the points go round with a negative shoelace sum (y down), normals (-dy, dx) out; `0x4A2C40` (stdcall(poly)) reverses the points and rebuilds the edges without allocating. The computer flips for balls between y 360 and 510 (immediates 400 at `0x473A60`, 550 at `0x473A6A`, less 40).

**Chain Lightning**: the rod (`LogicMgr+0x74`: `+0` state, `+4` x and `+0xC` x drawn (ints), `+8` y and `+0x10` y drawn (floats)) rises out of the bucket from 586 to 512 while a charged ball exists, swaying round the Hole's x. PegHit's first hit with a charged ball calls `0x46D290` (stdcall(LogicMgr, ball, peg), the call at `0x470E48`), which reads the rod once, zaps the unlit pegs near the line from the peg to it, sets `+0xC` to the Hole's x and state 4 (50 ticks of sparks at `+4/+8`: `0x45D3B0` at `0x438D58`, stdcall(kind, x, y, ...)); the bolt is drawn to the rod each frame. While aiming, `0x46CD10` (ECX LogicMgr, EAX peg, the call at `0x4776C7`) works out the pegs from the guide's first one for the preview. The rod is drawn by `0x4777B0` (thiscall(LogicMgr), stdcall(g), the call at `0x40CA7F`), only below y 600. The computer guesses its height as the Hole's y - 44 (fsub at `0x4394F1`).

**Pyramid**: `0x46A850` turns the bumperhole parts on (the drawn ones not colliding, the sides not drawn) and the bucket's own; `0x45F0E0` sends a ray from the green peg to the part named "bumperc"; touching a bumperhole part sets ball `+0x16C` ("Pyramid Pickup!").

**PlayStation controllers** (`mod/src/padhid.cpp`): Sony's vendor id 054C; DualShock 4 05C4 and 09CC, its adapter 0BA0, DualSense 0CE6, Edge 0DF2. Input reports: sticks (0..255, y down), then either a byte of d-pad (low nibble, 0 up, clockwise, 8 none) and face buttons (10 square, 20 cross, 40 circle, 80 triangle), a byte of the rest (01 L1, 02 R1, 04 L2, 08 R2, 10 share, 20 options, 40 L3, 80 R3), a counter byte, L2, R2 (a DualShock 4; a DualSense over Bluetooth until something asks for more), or L2, R2, a counter and then those button bytes (a DualSense's full report). Report 01 has the sticks at byte 1 (USB, or the short Bluetooth one), 11 at byte 3 (a DualShock 4 over Bluetooth, byte 1's 80 set if it has input), 31 at byte 2 (a DualSense over Bluetooth). The input report's length tells the buses apart: 64 over USB. Motors: DualShock 4 report 05 (byte 1 = 01 motors only, light 4, heavy 5), over Bluetooth 11 (C4, byte 3 = 01, light 6, heavy 7); DualSense 02 (byte 1 = 03, light 3, heavy 4), over Bluetooth 31 (byte 1 = 02, then the same one further on); the Bluetooth ones are 78 bytes ending in a CRC-32 of A2 and the rest. On this PC DSX in PS4 mode is a ViGEm DualShock 4 (USB form, about 250 reports a second, shared with any number of readers), with HidHide keeping the real DualSense out of sight; Xbox mode is an XInput controller, and Steam Input bridges neither to the game.

**Profile progress**: `0x42DC90` (EAX app) gives how far the player has got in Adventure (stage * 5 + level, profile `+0x2C` and `+0x60`). Board setup reads it (purple pegs need 2, green 1), and so do the opening and `LogicMgr::MouseDown`; `0x42DCC0` (EDX app) says whether the player is on their newest level (special pegs then keep their colors). Two players' profiles differ, so inside the online simulation both report 1000, and not on it.

**Peg pictures**: round pegs (Ball slot 25 → `0x49E1B0`, ECX PegInfo) draw a cell (type − 1, + 4 lit) of one
20 × 160 image of 8 stacked cells, the global at `0x6CAC04` (IMAGE_BALLPEG; `0x6CAC20`, the colorblind one, when
`0x6CBDA7` is set), at (x − 11, y − 11) for a radius-10 peg (`0x51BE40` DrawImageF: EAX g, stdcall(image, float x,
float y, Rect* src); `0x51B8A0` the same at ints; `0x51C020` rotated: EAX g, EDX image, ECX Rect* src or null, stdcall(float x, float y, float about_x, float about_y, double radians), about the point counted from x, y; the app's vtable `+0x10C` GetImage (thiscall(const std::string& path, bool commit)) loads a new picture by path, as levels do (`images/levels/twirl`, `twirlp`, `greenring`, `darkswirl2`: the portals in Renfield4 and Tula2, which Wormholes! and Portal Storm! draw too); `0x51DA90` GetCelRect: ECX image, ESI out Rect, stdcall(col,
row)); without 3D acceleration a moving peg draws from per-cell copies at `0x6CBD80`. A peg becoming special
(PegInfo `+0x20/+0x24`) flashes by drawing itself again lit and additive, and its overlay (slot 26 → `0x49E7F0`)
draws a sparkle in its color. Shadows (slot 24): IMAGE_PEGSHADOW `0x6CA858` (22 × 22), colorized black at alpha 0x90,
3 pixels left and down. Only lit pegs glow (`0x49E5D0`: ESI peg, EAX PegInfo, EDI g; from the shadow pass, so under every peg): round ones IMAGE_PEGGLOW `0x6CA9AC` (48 × 48) centred, additive, colorized in their color (orange `0xFF8800` at alpha 0x7F, purple `0xFF55FF` and green `0x00FF00` at 0x9F); bricks their own glow picture, cell 8 (`Brick+0x514`), drawn by `0x497650` (EAX cell, stdcall(brick, g): a brick in a cell's picture) with the picture's offset `+0xB8/+0xBC` from the brick's position scaled by (glow width - 3) / (cell width - 3). Bricks (`0x49E450`) draw a picture of their own per cell,
made the first time it's needed (`0x494A00`, stdcall(brick, cell)) into `Brick+0x4F4 + 4 * cell` (MemoryImages, kept
for the brick's life) and drawn from there with 3D acceleration (`0x4A0810`, ESI image); without it, from copies at
`Brick+0x184 + 0x58 * cell`. Images: width `+0x24`, height `+0x28`, rows `+0x2C`, columns `+0x30`; a MemoryImage
is `0x6C` bytes (constructor `0x525250`, ECX this), virtual slot 25 BitsChanged, 30 Create(w, h), 31 GetBits (ARGB,
not premultiplied; a DDImage reads its surface, a MemoryImage its native alpha or Direct3D texture back if its bits
were purged). The purple peg's chooser `0x469600` considers only type-1 (blue) pegs, or adopts another type-3 one.
SOUND_LOGO_BUZZ_ON (the PopCap logo's neon) is `0x6CA6A0`, SOUND_PEGSPARK `0x6CA71C`.

**Instant Replay**: as a shot is fired its bookkeeping (`0x46C230`) records a copy of the whole board (`0x405F10`,
the call at `0x46C3EB`, EAX board: a DataWriter from the board's serializer `0x404FE0`, which reads and writes
through one DataSync) into the deque at `Board+0x744` (map `+0x748`, size of map `+0x74C`, first `+0x750`, count
`+0x754`; 49 kept; element `0x43DC20`, ESI deque, stdcall(index) → address). Each logic step of a ball in flight
(`0x4742C0`) records every ball's position and speed into `LogicMgr+0xE0` (28-byte records: step `LogicMgr+8`,
ball id `+0xE8`, x, y, vx, vy); at the end of the shot (`0x474A60`) `0x468100` adds the records not yet written to the
latest copy (marking them written: it changes the LogicMgr, so the mod never calls it). A replay (`0x406160`, the
board's button 1 or the panel's Instant Replay) first keeps the live board (`0x405D00` into `Board+0x758`), then loads
the copy (`0x405DB0`: ECX copy, EDX board, stdcall(bool with the records) → AL; loading builds every object anew)
and sets `LogicMgr+0x101`; while it plays, each step puts the balls back where the records say. Done (`0x406080`)
loads `Board+0x758` back. Everything on the board is in the copy, the movers (`0x49CB60`: base, radius, tick
offset) and the objects' collision flags too, but not what lives outside it (a code patch, the mod's own state).
`LogicMgr+8` counts the logic steps of the current state: the turn passing (6) lasts at least 150 of them in a
duel (`0x4763D0`, also waiting for the score count). Replay files (`.pegn` in `<data folder>userdata\replays\`, the
data folder a string at `0x6C7958`): u32 version (0x52), u32 `Board+0x6E0`, u16 count, then per shot u32 size and
the copy's bytes; the reader (`0x406D10`: ECX Buffer, data `+8`, size in bits `+0x14`; stdcall(board)) clears the
deque and stops after the last shot, so bytes after it are ignored. The panel's Save opens the file dialog
(`0x406BA0`), whose writer is `0x4C7D70` (EBX dialog, stdcall(const std::string* path); the call at `0x4C8205`);
with a ball in flight it calls `0x468100` first. Main menu Replays loads a file with `0x406C70` (ESI board,
stdcall(path)).

**Sabotage Mode** (`mod/src/chaos.cpp`, `chaos_effects.cpp`): at the duel's first turn a random blue peg (round if
any) becomes the red peg; it stays blue underneath, drawn from a recolored copy of the blue pictures (crimson), and
is hidden from the purple chooser by reading as green (type 4) while it runs. It's hit when it lights, whatever lit
it; the other player's next turn gets Mirage, Silhouette, Blind Shot or Lights Out (not the round's Chaos Mode effect,
nor Mirage or Silhouette during Invisible Pegs), ended at the turn after. Mirage's fakes are the mod's own drawings
(no physics objects): placed inside the ticks beside real pegs, found out by the ball's path each physics step (from
its previous position, `Ball+0xF4/+0xF8`). Silhouette draws unlit pegs from a gray copy of the blue pictures (every
cell of the round pegs' copy is blue's, so all colors look alike) and leaves out their overlay. The recolored copies
are made while drawing, outside the simulation, and dropped with the board. The next turn's effects are decided as
the turn passes, so that Mirage's blackout (from step 110 of the turn passing) is black before its fakes appear.

**Replays of the modes** (`mod/src/replays.cpp`): the effects act in each physics step (the call at `0x40E104`,
which only `Board::Update` reaches), so a replay, fast-forwarded or not, meets them at the same steps. Each of
`Board::Update`'s steps runs the logic step (`0x4771A0`: first `0x476D60`, which fires a normal shot, then the step
count `LogicMgr+8` goes up and the state's function runs) and then the physics step (`0x40DE20`, not in state 10).
A Zen Ball is launched by state 10's function (`0x4766A0`: the search at its step 130, the launch at 180), after the
count went up, so one physics step comes before the shot's first logic step (state 2, count 0, as `SetState`
`0x468190` (ESI LogicMgr, EDI state) leaves it). A replay puts the balls back from the records with the step count
(`0x4742C0`), matching a record's step plus 1 when `LogicMgr+0x55` is set (it also sends each ball in flight
through `0x473CB0`); a Zen shot's replay sometimes runs that first physics step and mostly not, so the effects'
per-step work in it is a step behind or level with the shot (the balls are where the records say either way). Not
fixed: the mod's replay tests take their shots without a Zen Ball (`chaos zen 0`). Also open: with Teleporting
Bucket, about one normal shot in ten replays a physics step longer than it was (its jumps come again with the same
amounts from the same places; the cause isn't known yet). With each
recorded copy the mod keeps the effects' state (pegs by their place in the level's order and position, the effects'
random numbers); loading a copy puts that state in place (and the code patches), loading the live board back the
live state. Saved files get the states after the game's data (count, size, total, "PNRS" at the very end), matched
to the shots by their bytes. Online, SAVE writes a one-shot file itself from the latest copy (only reading it), after
the shot in flight if there is one.

**Chaos Mode** (`mod/src/chaos.cpp`, `chaos_effects.cpp`): the rounds, and each effect's start and end, are decided after each board tick (inside the simulation online); effects that change the game hook the places above and act only inside the ticks, with random numbers of their own seeded from the level's seed. What they draw is timed by the ticks too, so two online games draw the same picture at the same tick. Online the host's Play Now carries the setting (`kItemPlayNow`'s fourth byte). A replay plays a shot with the effects it had (below). Upside Down replaces the Spooky Ball's check with its mirror image (out of the top: `y + radius < 0`) and has `0x472CB0` read its height from the mod (550, 10 pixels above the bottom), so the ball comes back at the bottom on its way up. It mirrors Claude's flippers (pivot, shape, turn, swing, all in the copies) and the bucket's colliding parts' shapes, draws the flippers mirrored about their pivots and the rod turned round, mirrors the rod's height where it's used (the arc, its sparks, the drawing; its own motion stays), moves the catch's texts under the bucket, and turns the computer's flipper and rod guesses over. Double Bucket halves the track's goal as the game eases towards it; the copy collides with the bucket's colliding parts 260 pixels along (not where the bucket's own are), keeps the lid while the bucket has it (a fireball knocks it off), and Chain Lightning's arc goes to the nearer bucket (the rod moves there for it).
