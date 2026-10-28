NoX Core
========

NoX Core is a modern client for Nox (Westwood Studios, 2000): the game as you know it, cleaned
up, plus new features, for today's hardware, on Windows and Linux. The bugs bad actors can use
(crashes, cheats, exploits against players and hosts) are fixed; the quirks that have become part
of how the game is played are kept. It is
written from scratch, from our own analysis of how the game behaves; no code from the original
game is included. It speaks the original's network protocol, so NoX Core players and players on
the original game can join each other's games.

You need your own copy of Nox (GOG, Origin or the original CD). NoX Core contains none of the
game's art, sound, music, maps or text; it reads them from your installed game when it runs.

Not affiliated with, endorsed by or sponsored by Electronic Arts or Westwood Studios. Nox is a
trademark of Electronic Arts Inc.

Ready-made Windows builds: https://noxhub.net/

What's here
-----------
  noxcore/     the engine library: the game's file formats, its network protocol, and the game
               simulation that hosted games run on.
  noxsdl/      the NoX Core client (noxcore.exe; on Linux, noxcore), built on SDL3: the game's
               menus, play, hosting your own games, and a map editor.
  videotools/  the NoX Content Editor (noxcontent), for looking through the game's files
               (things, tuning, monsters, strings, sounds...) and changing its pictures
               (video.bag and the HD image pack). How to use it: CONTENT_EDITOR.txt in that
               folder.
  docs/        two published notes, below.

Building
--------
See BUILDING.txt: what you need on Windows and Linux, the commands, and the build options.

Running
-------
Start noxcore.exe (Linux: noxcore). It finds your Nox install by itself: its own folder, the
current folder, then the usual GOG, Origin and CD installs (on Linux, the usual Wine, Lutris and
Heroic prefixes). Point it at another copy with --game-dir DIR, or the NOX_DATA environment
variable. Which folder it chose, and why, is written to noxcore-log.txt beside the program.

Published notes
---------------
  docs/original_bugs.md        bugs found in the original game: the ones bad actors could use
                               are fixed, the ones that are part of how it plays are kept.
  docs/vanilla_deviations.md   the places where NoX Core deliberately behaves differently from
                               the original, and why.

Licence
-------
NoX Core is free software under the GNU General Public License, version 3 or (at your option)
any later version (GPL-3.0-or-later): see LICENSE and NOTICE.txt. The third-party code it uses,
and those licences, are listed in THIRD_PARTY_NOTICES.txt.
