# NoX Core

**A modern client for Nox** (Westwood Studios, 2000): the game as you know it, cleaned up, with
new features, for today's hardware, on Windows and Linux.

NoX Core plays just like the original and speaks its network protocol, so NoX Core players and
players on the original game join each other's games, on the original's servers and on ours. Bugs
that cheaters and troublemakers could use are fixed; the quirks players have come to rely on are
kept. It is written from scratch, from our own analysis of how the game behaves; no code from the
original game is included.

- **Website and ready-made Windows builds:** https://noxhub.net/
- **Everything it adds:** https://noxhub.net/features.html
- **What changed lately:** https://noxhub.net/patch-notes.html
- **Video walkthroughs** (map editor, scripting, mods): https://noxhub.net/walkthroughs.html
- **macOS:** a community-maintained fork by Avrutskiy, with its own builds and support:
  https://github.com/Avrutskiy/NoXCore-Mac

> You need your own copy of Nox (GOG, Origin or the original CD). NoX Core contains none of the
> game's art, sound, music, maps or text; it reads them from your installed game when it runs.

## Highlights

- **Co-op Campaign:** play the Warrior's, Wizard's or Conjurer's story with up to three friends,
  original-game players included.
- **New modes:** Delve (endless generated levels) and Undead Tide (survive the night), alone or
  with a party.
- **Map Editor** inside the game: walls, floors, objects, scripts with a reference and
  suggestions, play-testing with F5, and editing together with friends.
- **Hosting** from the menus with the original's full server settings, bots, map voting and
  faster updates for NoX Core players.
- **Graphics:** widescreen, any window size, per-pixel lighting, bloom, and an optional pack of
  HD images.
- **Mods:** game modes written in Luau, loaded from the `mods` folder.
- **The NoX Content Editor** for the game's files, saving changes into content packs.
- **Languages:** NoX Core's own text in Russian and Korean as well as English (machine
  translations for now: corrections welcome), and Korean installs' game text drawn.

## Building

Short version (full details, prerequisites and options in [BUILDING.txt](BUILDING.txt)):

**Windows** (Visual Studio 2022 with "Desktop development with C++"), from a Developer
PowerShell:

```
cmake --preset msvc-release
cmake --build --preset msvc-release
```

The client is `build\msvc-release\noxsdl\noxcore.exe`.

**Linux** (GCC 13+, CMake 3.25+, Ninja, glslang and SDL3's development packages; the Ubuntu
24.04 package list is in BUILDING.txt):

```
cmake --preset linux-release
cmake --build --preset linux-release
```

The client is `build/linux-release/noxsdl/noxcore`.

The first configure downloads the pinned third-party libraries, so it needs a network connection.

## Running

Start `noxcore.exe` (Linux: `noxcore`). It finds your Nox install by itself: its own folder, the
current folder, then the usual GOG, Origin and CD installs (on Linux, the usual Wine, Lutris and
Heroic prefixes). Point it at another copy with `--game-dir DIR` or the `NOX_DATA` environment
variable. The folder it chose, and why, is written to `noxcore-log.txt` beside the program.

**Saved servers:** Network Game's **Add/Remove** saves a hostname or IPv4 address (optional
`:port`, 18590 by default) to `servers.txt` in the game folder, or removes one. The list pings
them on every refresh, beside the LAN's games; one that doesn't answer stays listed ("No response")
and can still be joined. `servers.txt` beside the program is read as well; one address per line,
`#` starts a comment.

## What's in this repository

| Folder | What it holds |
|---|---|
| `noxcore/` | The engine library: the game's file formats, its network protocol, and the game simulation that hosted games run on. |
| `noxsdl/` | The NoX Core client on SDL3: menus, play, hosting, rendering, audio and the map editor. |
| `modes/` | The built-in game modes (Delve, Undead Tide, the co-op campaign) and the Luau scripting that mods use. |
| `mods/` | Mods written in Luau: Deep Delve, an action-RPG mode in development and the example to learn from. |
| `videotools/` | The NoX Content Editor (`noxcontent`). How to use it: `videotools/CONTENT_EDITOR.txt`. |
| `docs/original_bugs.md` | The bugs found in the original game, and which are fixed or kept. |
| `docs/vanilla_deviations.md` | Where NoX Core deliberately behaves differently from the original, and why. |
| `cmake/`, `.github/`, `packaging/` | Build settings and pinned dependencies, the automatic build, and a debug launcher. |

## Contributing and translations

See [CONTRIBUTING.md](CONTRIBUTING.md): how player-facing text is written so it can be
translated, and the rule that keeps every language current with the English.

## Security warning: map scripts in the original game

> **In the original game, a map's script can run any program code on your computer.** The original
> game never checks which built-in function a map script asks for, so a map can make it run machine
> code of its own. That code runs with your Windows account's permissions and can do anything a
> program can. Community maps have used this trick since at least 2019. The ones we examined only
> change the game, but nothing in the original game stops a hostile map.
>
> - **At risk:** anyone who hosts a game or plays alone on the original game, with a map from
>   someone they don't trust. Playing alone also hosts the game on your computer.
> - **Joining someone's game** on the original game doesn't run its script. But the map you
>   download stays in your maps folder and runs if you host it later. The original game's reading of
>   a downloaded map is also unchecked, so a crafted map could in principle harm a player who only
>   joins.
> - **NoX Core is not affected.** It never runs code from a map, and it checks every map script it
>   reads. Maps that rely on this trick don't work in NoX Core.
>
> The details are in [docs/original_bugs.md](docs/original_bugs.md) (OB-434 and OB-435).

## Reporting bugs

Bug reports are welcome as GitHub issues. Please include `noxcore-log.txt` (it's beside the
program) and say whether the game was hosted by NoX Core or by the original game.

## Licence

NoX Core is free software under the GNU General Public License, version 3 or (at your option) any
later version: see [LICENSE](LICENSE) and [NOTICE.txt](NOTICE.txt). The third-party code it uses,
and those licences, are listed in [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).

NoX Core is an unofficial fan project, not affiliated with, endorsed by or sponsored by Electronic
Arts or Westwood Studios. Nox is a trademark of Electronic Arts Inc.
