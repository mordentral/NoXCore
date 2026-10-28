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

## Reporting bugs

Bug reports are welcome as GitHub issues. Please include `noxcore-log.txt` (it's beside the
program) and say whether the game was hosted by NoX Core or by the original game.

## Licence

NoX Core is free software under the GNU General Public License, version 3 or (at your option) any
later version: see [LICENSE](LICENSE) and [NOTICE.txt](NOTICE.txt). The third-party code it uses,
and those licences, are listed in [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).

NoX Core is an unofficial fan project, not affiliated with, endorsed by or sponsored by Electronic
Arts or Westwood Studios. Nox is a trademark of Electronic Arts Inc.
