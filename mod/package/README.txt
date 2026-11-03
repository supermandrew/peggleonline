PEGGLE NIGHTS - ONLINE DUEL
===========================

Adds online play to Duel mode, so two people on different computers can duel each other, adds Chaos
Mode and Red Peg Mode to duels, lets you play the whole game with a controller, and adds a
borderless full screen.
Both players need Peggle Nights on Steam and this same version of the mod.


INSTALL
-------
1. Quit Peggle Nights if it's running.
2. Double-click Install.bat. Windows asks for permission to change the game folder: click Yes.
3. Start Peggle Nights from Steam as usual.

To remove the mod, double-click Uninstall.bat.
(Steam's "Verify integrity of game files" also removes it.)


PLAY
----
Click Duel, then Online Play. One player hosts, the other joins.

Hosting:
  - Click Host Game. The game shows the address your opponent should type in.
  - The first time, Windows Firewall asks whether to allow "popcapgame1.exe". Click Allow.
  - When your opponent connects, pick a level. Each player picks their own Master, then the host
    clicks Play Now.

Joining:
  - Click Join Game, type the host's address, and click OK.
    The game remembers the last address you typed.

Whoever's turn it is controls the game; the other player watches the shot live.
To say something to your opponent, click CHAT (top left) and pick a phrase. Messages show at the
bottom left for a few seconds; click CHAT again to see the recent ones.
Instant Replay can't play during an online duel, so its button is SAVE instead: it saves the last
shot as a replay (named after whoever shot it, e.g. "Dad's shot"), and your chat box says so (only
you see that). Watch it later from Replays on the main menu. Pressed while a ball is in flight, it
saves that shot once it's over.
The Menu button still works for volume and so on, but it doesn't pause an online game.
On the results screen the host picks what happens next (Replay Level or Play New Level).
Either player can leave with Main Menu.


CHAOS MODE
----------
Tick CHAOS MODE on the Two Player Selection (the middle column, above Play Now) before you press
Play Now, in a local or an online duel. Online, the host's box decides; the other player sees it.
Both players' first shots are normal. After that, each round (each player shoots once) brings a
new chaos effect, named in the middle of the screen with a gong, until the next round's replaces it:

  Big Balls!           balls are 50% bigger
  Pinball Madness!     pegs kick the ball harder (and it may go faster than usual)
  Double Bucket!       two buckets side by side, each moving along half the bottom
  Teleporting Bucket!  the bucket jumps somewhere else every few seconds
  Purple Party!        three purple pegs at once
  Peg Shuffle!         the remaining pegs swap colors (as many orange ones as before)
  Invisible Pegs!      pegs show up only once the ball hits them
  Blind Shot!          no aiming guide
  Double Shot!         every shot fires two balls, a little apart
  Wormholes!           two portals: in one, out of the other, just as fast
  Wrap Around!         no side walls: out one side, in the other
  Bomb Peg!            one glowing peg blows up the pegs around it when it's hit
  Lights Out!          the board goes dark but for two sweeping spotlights
  Upside Down!         the cannon at the bottom, the bucket upside down at the top, the ball falls up
                       (Renfield's Spooky Ball comes back at the bottom)
  Portal Storm!        six portals: three pairs, by color
  Bullet Time!         slow motion (and a zoom) whenever the ball gets close to an orange peg

Instant Replay plays a shot with the chaos effect it had (in a local duel; online it's SAVE), and so
do replays you save. With 3D acceleration off (Options), Upside Down's bucket stays the right way up
and Bullet Time doesn't zoom in.


RED PEG MODE
------------
Tick RED PEG MODE (just above CHAOS MODE) before you press Play Now. One blue peg of the level is
red: any of them, round or a brick, still or moving. Whoever hits it ("Sabotage!") gives the other
player an effect for their next turn only, named in the middle of the screen like a chaos effect.
After that there is no red peg for the rest of the game.

  Mirage!              fake pegs, first where pegs were hit earlier (moving on as those did), then
                       among the real ones; the guide stops at them as at real pegs, but the ball goes
                       through them and they glitch away (the board goes black for a moment as the
                       turn passes, while they appear)
  Silhouette!          every peg is gray until the ball hits it
  Blind Shot!          no aiming guide
  Lights Out!          the board goes dark but for two sweeping spotlights

You can tick both modes. A red peg's effect then comes on top of the round's chaos effect, never the
same one twice (and not Mirage or Silhouette during Invisible Pegs, which would hide them anyway).
Instant Replay works as in Chaos Mode. With 3D acceleration off, red and gray bricks (not round pegs)
keep their own colors (a red brick still glows red).


SCREEN
------
Options (or the in-game Menu) has a BORDERLESS box under the other checkboxes: the game then fills
your whole screen without changing its resolution, kept in shape with black bars at the sides, and
you can still Alt+Tab away as with any window. It sits on the monitor the game window was on. It
and Fullscreen exclude each other: ticking one unticks the other.
In a window the game is as big as Windows' scaling setting makes other programs, drawn sharper than
Windows would.


CONTROLLER
----------
Plug in a controller and use it any time; the mouse pointer hides while you do, and comes back when
you move the mouse. Xbox controllers work as they are. A PlayStation or other controller needs to
look like an Xbox one to the game: Steam Input does that (Steam's controller settings), or a tool
such as DS4Windows or DSX.

  Left stick / D-pad   menus: move between buttons     game: aim (D-pad: one small step)
  Right stick          game: fine aim
  A / Cross            press the highlighted button; shoot
  B / Circle           back / cancel
  Y / Triangle         CHAT (online): up/down picks a phrase, A sends it
  X / Square           Instant Replay, when it shows (online: SAVE)
  LB, RB / L1, R1      flippers (Claude's power)
  RT / R2 (hold)       fast-forward, like holding the right mouse button (the game speeds up
                       Instant Replays and everything but a ball in flight)
  Start / Options      the in-game Menu

During an Instant Replay, up/down and A use its buttons (Save, Done...), X watches it again and
B is Done.

To change the buttons, turn vibration off, or make the left stick aim slower or faster, open
Options (or the in-game Menu) and choose CONTROLLER. Pick an action, then press its new button.
Typing (the Join Game address, a new player's name) brings up a keyboard on screen: A types the
highlighted key, B deletes, X is space, Y switches capitals, Start is done.

Two controllers in a local duel (Duel, then Local Play, Play a Friend): controller 1 plays
player 1 and controller 2 plays player 2. On the Two Player Selection each player has their own
highlight (player 2's is blue) on their own side. In the game only the controller of the player
whose turn it is does anything, and it buzzes when that player's turn starts. Menus answer to
both. If the controllers are the wrong way round, open CONTROLLER and change "LOCAL DUEL:
CONTROLLER 1 = PLAYER 1". With one controller it plays both players, like the mouse.


IF YOUR OPPONENT CAN'T CONNECT
------------------------------
The game opens the hosting port (TCP 28008) on your router automatically, using a router feature
called UPnP. If the game says PORT NOT OPENED:
  - Turn on UPnP in your router's settings, or
  - Forward TCP port 28008 to the hosting computer in your router's settings, or
  - Let the other player host instead.
