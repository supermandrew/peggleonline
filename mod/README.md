# Peggle Nights online duel mod

Adds Online Play to Duel mode, Chaos Mode and Red Peg Mode for duels, play with a controller (menus and the whole
game), and a borderless full screen.
Built as a replacement `bass.dll`
that forwards every BASS export to the original (`bass_orig.dll`) and hooks the game in memory. How the game works is
documented in [../re/NOTES.md](../re/NOTES.md).

## Layout

| Path | What it is |
|---|---|
| `src/main.cpp` | Entry point: game-build check, crash logger, per-frame hook |
| `src/online.cpp` | The Duel → Local/Online → Host/Join flow, handshake, level hand-off, leaving |
| `src/lockstep.cpp` | Deterministic lockstep: shared random numbers, tick stream, input replay, synced widgets |
| `src/arena.cpp` | Deterministic allocator for the simulation (address-ordered sets must iterate identically) |
| `src/chat.cpp` | The CHAT button, its phrase list and the box that shows what was said |
| `src/chaos.cpp` | Chaos Mode and Red Peg Mode: their boxes on the Two Player Selection, the rounds, the red peg's hit and whose turn gets its effect, which effect comes next, the announcements |
| `src/chaos_effects.cpp` | The effects (Chaos Mode's 16, the red peg's Mirage and Silhouette) and the red peg: hooks into the game's pegs, balls, physics, bucket, cannon and drawing, recolored copies of the peg pictures, Mirage's blackout, the effects' state of a shot for its replay |
| `src/replays.cpp` | Instant Replay with the modes: the effects' state of each recorded shot, put in place while its replay plays; saved replay files carrying it; online, SAVE in Instant Replay's place |
| `src/pad.cpp` | The controllers (XInput, and `padhid` while XInput has none), together or one at a time, whether they or the mouse are in use, vibration from the game's sounds |
| `src/padhid.cpp` | DualShock 4s and DualSenses read straight from their HID reports (USB and Bluetooth), found on a thread of their own, and their motors |
| `src/controls.cpp` | What the controller does: moves between and presses buttons on every screen, aims and shoots on the board, chat, Instant Replay; a local duel with a controller per player |
| `src/keyboard.cpp` | On-screen keyboard for text boxes |
| `src/padmenu.cpp` | CONTROLLER in the Options dialog: button mapping, vibration, aim speed, which controller plays which player |
| `src/config.cpp` | `mods\netplay.ini` |
| `src/dialogs.cpp`, `src/game.cpp` | Native game dialogs and wrappers around game functions |
| `src/ui.cpp` | The game's drawing, fonts, sounds and widgets, what's on screen, and mouse and key input |
| `src/net.cpp`, `src/upnp.cpp` | TCP connection (port 28008) and router port opening |
| `src/display.cpp` | The window: sized to the monitor's text scaling (the game is made DPI aware), or borderless across its monitor with the picture in shape; the game's mouse remapping; BORDERLESS in the Options dialog |
| `src/present.cpp` | Puts each frame in the window (DirectDraw's own present shows nothing on some PCs): a GDI copy, or scaled by Direct3D 9 in a child window of ours (GDI if that fails) |
| `src/debug.cpp` | Test pipe, only when `mods\debug.txt` exists in the game folder |
| `package/` | What players get: installer, uninstaller, README |
| `tools/` | Test tools (below) |

## Build and install

```bat
mod\build.bat            :: builds mod\build\bass.dll (MSVC 2022 x86)
mod\build.bat install    :: also copies it into the game folder, keeping the original as bass_orig.dll
mod\package.bat          :: builds mod\dist\PeggleNightsOnline.zip for players
```

With Python installed, the build ends with `tools\xmmcheck.py`, which fails it if the compiled code keeps a float in an
xmm register across a call into the game. The compiler assumes an `__asm` block leaves those registers alone, but the
game's code may change them, so a function whose `__asm` calls the game is `GAME_CALL` (never inlined) and assembly
inside a bigger function goes into one of those.

Runtime files go to `mods\` in the game folder: `netplay.ini` (last address, `port=`, the controller settings in
`[controller]`, `[display] borderless=`, `[duel] chaos=` and `red=`), `logs\netplay-<pid>.log`.

## Testing two copies on one PC

The test pipe and diagnostics run only while `mods\debug.txt` exists in the game folder. `duo.py start` creates it and
`duo.py finish` closes both copies and deletes it again, so normal play runs without them.

The game runs one copy at a time, so the second copy (B) is started by `mod\tools\run2.py`, a local helper that isn't
in the repository; without it the tests that need two copies stop at `duo.py start`.

```bat
python mod\tools\duo.py start      :: copy A through the normal launcher, copy B through run2.py, both at the main menu
python mod\tools\duo.py connect    :: A hosts, B joins 127.0.0.1, A picks the first level
python mod\tools\duo.py duel       :: both pick masters, A presses Play Now
python mod\tools\duo.py autoplay 5 :: whoever is in control fires at random spots
python mod\tools\duo.py synced x   :: pause both at the same tick, screenshot both; fails if any pixel differs
python mod\tools\duo.py chat       :: both players chat; clicking away from the phrase list fires no shot
python mod\tools\duo.py scenario   :: chat, duel to results (chat), replay level, new level, joiner leaves
python mod\tools\duo.py full       :: all of the above, plus a local duel on A first
python mod\tools\duo.py pad        :: the online duel again with a controller only (after start)
python mod\tools\duo.py padfull    :: start, then pad
python mod\tools\duo.py padreports :: PlayStation controllers' HID reports in every form (after start; also in pad)
python mod\tools\duo.py local      :: a local duel on A with two controllers (after start)
python mod\tools\duo.py localfull  :: start, then local
python mod\tools\duo.py display    :: A's window and BORDERLESS, on the smallest monitor (after start)
python mod\tools\duo.py displayfull :: start, then display
python mod\tools\duo.py chaos      :: Chaos Mode in a local duel on A: the box, the rounds, each effect (after start)
python mod\tools\duo.py chaosonline :: A hosts, B joins: online Chaos Mode through all 16 effects (after start)
python mod\tools\duo.py chaosfull  :: start, then chaos (chaosonlinefull: start, then chaosonline)
python mod\tools\duo.py chaosreplay :: Instant Replay of a shot under each Chaos Mode effect (after start; or some: 2 13)
python mod\tools\duo.py chaoszen   :: just the rounds and a Zen Ball's shot counting as one (after start)
python mod	ools\duo.py chaospowers :: just masters' powers at the bucket with its effects (after start)
python mod\tools\duo.py redpeg     :: Red Peg Mode in a local duel on A: the box, each red peg effect, with Chaos Mode,
                                   :: bricks and moving pegs (Eiffel Power), a brick turning round a flower (The Guardians)
python mod\tools\duo.py redturning :: just the turning red brick, drawn red in every frame (after start)
python mod\tools\duo.py redpegonline :: A hosts, B joins: an online Red Peg Mode duel (after start)
python mod\tools\duo.py redpegfull :: start, then redpeg (redpegonlinefull: start, then redpegonline)
python mod\tools\duo.py finish     :: close both copies, debug mode off
```

The controllers in tests are pretend ones: the pipe's `pad A+LB 0.5 0 0 0` holds buttons and sticks (left x, y, right
x, y) on controller 1 until the next `pad`, `pad 2 A` plugs in a second one and sets it, `pad 2 off` unplugs it, and
`pad off` hands back to the real controllers. `padstate` reports what the controller is doing (menu, board, chat,
keyboard, replay, players: the highlighted button, the highlight frames, the last vibration's controller), and `board`
the board's state (whose turn, balls, logic steps run, the masters picked). `pad` in duo.py covers Options > CONTROLLER
and remapping, the Online menus, typing the address on the on-screen keyboard, masters, aiming and shooting with the
sticks while the other player's controller does nothing, chat, the Menu, the results (the joiner only reaches Main Menu)
and leaving. `local` plays a saved replay from Replays (if the profile has one), then covers a local duel with two
controllers: each player's own highlight on the selection, every turn answering only to its player's controller (aim,
fast-forward measured in logic steps a second, shot, vibration, the turn's buzz), the Instant Replay panel, the mod
still running every frame with the Menu over the paused board, swapping the controllers in CONTROLLER, and one
controller playing both players.

`chaos` ticks and unticks the CHAOS MODE box (netplay.ini remembers it), plays both players' first shots (no effect)
and two rounds (a new, announced effect each), a Zen Ball's shot (`chaos zen` gives the player in turn one, as Master
Hu's power does: the game chooses the shot in a state of its own) counting as that player's shot of the round, with
Double Shot splitting it (its other shots are aimed: `chaos zen 0` takes away a Zen Ball the power gave), then has
every effect in turn (the pipe's `chaos hold` keeps it through
the rounds, `chaos now <n>` switches) and checks what it does: the ball's size, kicks off pegs, a ball caught in the
second bucket (`chaos dropin`), the bucket's jumps, the purple pegs, the shuffle, the pegs gone from the picture, the
guide left out, the second ball, the portals, the ball coming round from the other side, the bomb going off (`chaos
bombat <x> <y>` makes the peg nearest that point the bomb), the board dark, the ball falling up, the slow motion, and
everything back to normal afterwards. It also makes a shot a Spooky Ball (`chaos spooky` arms the next one): with gravity as
usual it comes back at the top on its way down, upside down at the bottom on its way up. `chaos fx` reports what the effects did.

`chaospowers` (part of `chaos` too) checks masters' powers at the bucket with the bucket's effects. `chaos power <n> [turns] [player]` gives a player a power by the game's number as a turn would (2 Flippers, 4 Pyramid, 12 Fireball, 14 Chain Lightning), and `chaos ballat <x> <y> <vx> <vy>` puts the ball in play there. Claude's flippers go to the top upside down, mirrored, swing down and bat a ball down, and come back. Double Bucket keeps half the track over the turns, half the flippers' narrower one too. The second bucket bounces a ball off its rim and catches one off its middle, and a fireball knocks its lid off. Chain Lightning's arc goes to the nearer bucket, and upside down to the rod under the bucket. A rising ball bounces off the pyramid's side upside down, and the bucket catches upside down.

`chaosonline` checks that the joiner's
box shows the host's, queues the 16 effects in order on both copies (`chaos next ...`) and plays duels one after
another (Replay Level) until all have come up, comparing the two pictures pixel by pixel at the same tick with each
effect, and sets the bomb off on both (`chaos bombat <x> <y> <tick>` moves it inside the game's ticks, at the same
tick on both: changed outside them, one copy at a time, the two would differ meanwhile).

`chaosreplay` shoots under each effect in turn and plays the shot's Instant Replay: the effect must be in place while
it plays, the balls where the shot's were after every step (the pipe's `replay` reports a hash of the balls' positions
after each physics step of the shot and of its replay), and the effect back on afterwards. `replay button` says where
Instant Replay is; Done is the replay panel's bottom button.

`redpeg` ticks and unticks the RED PEG MODE box (above CHAOS MODE; netplay.ini remembers it) and checks that the duel
has one red peg, a blue peg underneath, drawn red. It moves the red peg onto a brick, a moving peg and a still round
one (`chaos redat <x> <y> brick|moving|round`; those the level has) and checks each is drawn red. It hits the red peg
for real ("Sabotage!" shown once: `chaos fx` counts them), so the other player's next turn gets Mirage: the board black
before the fake pegs show (`chaos fx` reports the blackout), some of them in gone pegs' places, the guide stopping at
a fake when aimed at it (`chaos fx` lists the fakes and where the guide last stopped at one), fake pegs gone after
that turn, with the ball glitching one away, and no red peg afterwards; the Instant Replay of that shot has the same
fakes again. The replay of the shot that hit the red peg shows it red without giving the effect again. Then, for each
of Silhouette (no orange left in the picture), Blind Shot (the guide left out) and Lights Out (the board dark), it
moves the red peg back to the top middle (`chaos redat <x> <y>` makes the blue peg nearest that point the red peg, as
if none had been hit), queues the effect (`chaos rednext <effect>`) and hits it, and checks that the effect lasts that
one turn. A second duel with both modes has the round's second shooter hit the red peg, so the next turn gets the new
round's Lights Out and the red peg's Silhouette together (Lights Out, queued first for the red peg too, can't come
twice), both announced, and the next turn after that keeps only the round's. A third, on Eiffel Power (Stage Six: blue
bricks and moving blue pegs; the level screen keeps the stage it last showed, so the test pages there and back),
draws the red peg as a brick and a moving peg, and has Mirage put fakes in gone bricks' and moving pegs' places (those
moving on along their paths), the guide stop at one, and a brick fake glitch. `redpegonline` checks that the joiner's
box shows the host's, that both games have the same red peg, and with the red peg hit (`chaos redat <x> <y> <tick>`
on both) that Mirage (the same fakes in the same gone pegs' places) and then Silhouette are the same on both,
pictures identical at the same tick. Then SAVE (in
Instant Replay's place online): B saves its own last shot as "Dad's shot" (the file ends with the mod's states),
noted in B's chat only, and A's shot while it's in flight once it's over; the games stay in sync. After B leaves,
Main menu > Replays plays the saved shot with its effects, the balls where the shot's were. The test deletes the
files it saved.

`display` compares what the screen shows (a screen grab of the window, so nothing may cover it) with the game's own
frame, sends Windows mouse positions to the window (`winmouse x y`) and checks where the game sees them, and covers
BORDERLESS on the smallest monitor (a virtual one, if there is one), so it takes over that screen for a minute. With
the mouse anywhere over the picture (`cursor x y` makes the game read that position from Windows, so the real mouse
stays put) the game must keep the mouse in its window, and its cursor thread must have the matching point of the
frame; with Custom Cursors ticked for a moment, the game's pointer must show there on the screen.

Every step checks within seconds that it happened (log lines, dialog ids, lockstep state) and stops with `FAILED:` and
screenshots of both copies (`mod\shots\failed_A.png`, `failed_B.png`) when it didn't.

Copy B has most masters locked (run2.py doesn't start it the way Steam does). Clicks are injected through the pipe
because the game ignores posted mouse messages while its window is in the background; the pipe also lends the
window the game's keyboard-focus flag for the click, since buttons only fire on release while it is set. The pipe answers
a click before doing it: some buttons open a dialog that waits until it's closed, and the pipe keeps working meanwhile.

When the two games drift apart, each log (`mods\logs\netplay-<pid>.log`) has the first mismatching tick and a per-tick
trace. In debug mode the mod also writes `allocs-<pid>.txt` and `frees-<pid>.txt` next to the logs: join the two
computers' free records by allocation number (`#n`) to find an object one freed inside the simulation and the other
outside it (see `re\NOTES.md`, section 8).
