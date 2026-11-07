# Peggle Nights Online

A fan-made mod for the Steam version of **Peggle Nights** that lets two players duel over the internet, and adds new
ways to play:

- **Online Duel**: one player hosts, the other types in the host's address. Both games run the same shared game step by
  step, so every shot plays out the same on both screens. A CHAT button sends set phrases to your opponent.
- **Chaos Mode**: each round brings one of 16 random effects, such as Big Balls!, Wormholes!, Upside Down! and Bullet
  Time!.
- **Sabotage Mode**: one blue peg is red, and whoever hits it sabotages the other player's next turn with Mirage!,
  Silhouette!, Blind Shot! or Lights Out!.
- **Instant Replay** keeps the effects in Chaos and Red Peg duels. Online, its button becomes SAVE, which saves the
  last shot as a replay.
- **Controller Support**: menus and the actual game are now playable with an Xbox, PS4 or PS5 controller, with
  remapping and vibration, and one controller per player in a local duel.
- **Borderless full screen**, and a sharper picture in a window on scaled displays.

Chaos Mode, Sabotage Mode and controller support work in local duels too.

## Requirements

- Peggle Nights from Steam, on Windows. The mod works with that exact version of the game: on any other it stays off
  and the game runs as usual.
- For online play, both players need the same version of the mod. If the host picks a level from a level pack the
  other player doesn't have, the game asks them whether they want it and sends it over.

## Install

1. Download `PeggleNightsOnline.zip` from [Releases](../../releases) and unzip it.
2. Quit Peggle Nights, then double-click `Install.bat` and let it change the game folder.
3. Start Peggle Nights from Steam as usual. Online play is under Duel > Online Play.

`Uninstall.bat` removes the mod, and so does Steam's "Verify integrity of game files". The README.txt in the zip covers
playing online, Chaos Mode, Sabotage Mode, the controller, and what to do if your opponent can't connect: the host's TCP
port 28008 has to be reachable, which the mod arranges with the router through UPnP, or you can forward the port
yourself.

## How it works

The mod is a replacement `bass.dll`. It passes everything on to the game's own BASS audio library (kept as
`bass_orig.dll`) and changes the game in memory while it runs. No game files are modified or included.

## Building

You need the Visual Studio 2022 Build Tools with the C++ x86 compiler (`mod\build.bat` expects them in their default
folder).

```bat
mod\build.bat            :: builds mod\build\bass.dll
mod\build.bat install    :: also copies it into the game folder, keeping the original as bass_orig.dll
mod\package.bat          :: builds the player zip, mod\dist\PeggleNightsOnline.zip
```

| Path | What it is |
|---|---|
| [mod/](mod/README.md) | The mod: source, build and packaging scripts, the player installer, test tools |
| [re/](re/README.md) | How the game works ([NOTES.md](re/NOTES.md)), identified function names, analysis scripts |

## Disclaimer

This is a fan project, not affiliated with or endorsed by PopCap Games or Electronic Arts, who own Peggle Nights. You
need your own copy of the game; none of its files are included.

## License

[MIT](LICENSE)
