# Deliberate deviations from vanilla 1.2b

NoX Core (the noxcore library and the noxsdl client) is a modern client for Nox 1.2b, written from
scratch from our own analysis of how the game behaves; no code from the original game is
included. It is not affiliated with or endorsed by Electronic Arts or Westwood Studios, and it
needs your own copy of the game. Its game logic is meant to behave exactly as the original's does. The original's bugs that bad
actors can use are fixed; those that have become part of how the game is played are kept. Both
are listed in [original_bugs.md](original_bugs.md).

This file lists the places where noxcpp knowingly behaves differently from GAME.EXE: fixes that
don't change the wire format, what the noxsdl client draws and plays, and noxcpp's own options
and game modes. If something odd shows up against vanilla clients or servers, check here first;
each entry says what the original does, what we do and why, and how to revert.

## How NoX Core hosts: a separate server and client

The biggest structural difference, behind several entries below. In the original, the game you
host (including the solo campaign) and the player you play are one and the same: your player is the
server's own player inside the one program, and never connects to its own game. NoX Core always runs
them as separate instances: the game you host (Host Custom, Game Modes, the solo campaign, co-op) is
a server, and your own client joins it over the network like any other player, on the local machine.

What follows from it:

- Hosting, even the solo campaign, uses a network port; your client's join goes through the same
  lobby and join steps as anyone else's (and can be refused the same way).
- Your client learns about the game only from what the server sends, as a remote player's would:
  anything the original's host knows without being told (its own team in the solo game, its state
  after a saved game is loaded) has to come in a message, or be worked out on the client.
- The server and the client run on their own threads, so a busy game doesn't slow the drawing.
- The result for play is meant to be the same as the original's; where it isn't, the entry here
  says so.

## Conventions for entries

- Each entry heading has a stable id, `VD-nn`. A new entry takes the next unused number,
  wherever it goes in the file; ids are never renumbered or reused. The client display, sound and
  similar lists are grouped under one id each; their items are cited by the id and the item's
  bold words.
- Name the original's code by logical name only: the game's own function name
  (`SrvNet_OnPacket`, `Spell_CastMagicMissile`), an OpenNox name without its prefix and address
  (`plrReadVals`), or a few words ("the reliable queue's flush"). No addresses or offsets.
- Say where our version lives (paths under `noxcore/` and `noxsdl/`, or class and function
  names), and refer only to public material: the source and these two documents.

## Transport

### VD-01: Reliable packets are flushed oldest first

- **Vanilla:** the reliable queue is a linked list with new packets added at the head, and a
  flush walks it from the head, so pending packets go out newest first.
- **noxcpp:** `Stream::flush` (noxcore/src/net/transport.cpp) sends them oldest first.
- **Why:** receivers accept only the exact sequence they expect and drop the rest without
  buffering. Newest first means that with seq 5 and 6 pending, 6 arrives first and is dropped,
  and gets through only after the peer's ack triggers a resend, one round trip later. Oldest
  first delivers both.
- **Risk:** only the order within one flush differs; the receiver's check is the same either
  way. A byte-for-byte comparison with vanilla captures would see the reordering.
- **Revert:** iterate `queue_` in reverse in `Stream::flush`.

### VD-02: Reliable messages packed per frame; no MSG_SEQ_IMPORTANT layer

- **Vanilla:** the game queues a player's "important" messages (Net_QueueImportantEx: the reports,
  journal, spell awards, equips...) in its own list; each frame netImportant puts a few of them
  (a per-player count) into that frame's netlist as MSG_SEQ_IMPORTANT
  (0xCC, a u16 per-player number) and resends them on a timer till the client acks them; the
  netlist goes out in one Net_Send. The transport's reliable packets are few.
- **noxcpp:** the server sends each such message transport-reliable (Stream::queueReliable), and
  every message queued for a peer before the next flush joins the same packet while it has room
  (0x400 bytes; noxcore/src/net/transport.cpp). Stock clients take these as any reliable
  messages.
- **Why:** one packet per message put a player's map-load reports (100+ armour equips, journal,
  spells) in 100+ packets at once: with the exe's ack window (original_bugs.md, OB-001 "An ack
  that jumps past 0x20 ...") stale packets were left behind and a later UseMap was lost (the
  client never loaded the map).
- **Risk:** a capture differs from vanilla's (fewer, larger reliable packets; no 0xCC). The
  important layer's per-frame cap and resend rate aren't kept.
- **Revert:** drop the joining branch at the top of `Stream::queueReliable`.

### VD-03: The client doesn't resend its connect packet after SERVER_ACCEPT

- **Vanilla:** the server's accept acks the connect with the connect's own seq (`1F 00`), not the
  next one. The general ack rule then marks that packet for an immediate resend, and the server
  answers the duplicate connect with 0x04 "already joined".
- **noxcpp:** `ClientConnection::control` drops the connect packet (`stream_.forget(0)`) when
  SERVER_ACCEPT arrives, and ignores 0x02-0x04 once connected.
- **Why:** it avoids a pointless duplicate and a refusal message that vanilla has to ignore.
- **Risk:** none known; the server gets one packet fewer.
- **Revert:** remove the `forget(0)` call.

### VD-44: A solo game's socket is on the loopback alone, any free port; a hosted game's port is its own

- **Vanilla:** a solo game opens no network socket at all: the server's start makes its
  listening socket only when the game isn't a solo one (game flag 0x800), its own player being
  the server's in the same program. A hosted game binds its port (18590 unless set) on every
  interface; when the bind fails because the port is in use it tries the next port up, again
  and again, until one binds. The bind isn't exclusive, so on Windows another program can still
  bind 127.0.0.1 on the same port afterwards and take what this computer sends there.
- **noxcpp:** a solo game from the menus (a new campaign, Continue, Load: noxsdl's main.cpp,
  the solo callback) is a GameHost that our own client joins over UDP, so it has a socket: on
  127.0.0.1 alone, on a port the system picks. Every port given to `UdpSocket::open`
  (noxcore/src/net/udp_socket.cpp) is bound exclusively on Windows (`SO_EXCLUSIVEADDRUSE`):
  Windows otherwise lets a socket on every interface share a port with one already on
  127.0.0.1, and the more specific one gets what's sent to it. A hosted game's socket
  (`UdpSocket::openHosting`, from LocalHost) takes the next free port up when its own is in use,
  as vanilla's does, and the console and the log say which port it took (`notePortMoved`).
- **Why:** a solo game hosted on 18590 while another program had 127.0.0.1:18590 (a game or a
  server run on the same computer) bound without error, but our client's join reached the other
  program, which refused it as full: the save never loaded. A solo game on the loopback and
  a free port can't meet another program's game, and nothing outside the computer reaches it,
  as nothing reaches vanilla's.
- **Risk:** a hosted game whose port was taken is on another port, as vanilla's would be; the
  LAN list's broadcast asks 18590 only (vanilla's too). The `--solo` test switch still takes
  `--host-port` and `--host-loopback` as given.
- **Revert:** drop the two `settings.loopbackOnly` / `settings.port` lines in the solo callback,
  and the `SO_EXCLUSIVEADDRUSE` block in `UdpSocket::open`.

## Headless client

### VD-04: KEEP_ALIVE while idle

- **Vanilla:** the client sends MSG_KEEP_ALIVE (0xBE) only while downloading a map
  (`netKeepAliveSocket`, called from the map download code). Otherwise its acks
  ride on whatever it happens to send (input, pongs).
- **noxcpp:** `HeadlessClient::gameFrame` queues 0xBE when nothing else went out for 250 ms. In
  the game that never happens, because the client sends its facing every frame like vanilla, so
  in practice it only covers the time between connecting and CLIENT_READY.
- **Why:** until we're in the game nothing else carries our ack; without it the server would
  resend reliable packets it thinks we lost.
- **Risk:** low; the server decoder accepts 0xBE at any time (it only advances past it).
- **Revert:** remove the keep-alive block in `gameFrame`.

## Client display (no effect on the wire)

These change only what noxsdl shows; every one is off with `--stock-effects` or the matching
Options switch unless noted.

### VD-05: Display, input and HUD

- **Loose particles kept out of the black:** the game's particle pass draws every particle
  without a drawable of its own wherever it is, over the line of sight's black too (image fire,
  sparkles on a drawable behind a wall: original_bugs.md, OB-187). noxsdl draws one only where
  the eye sees its ground point (Effects::drawParticles; 2026-10-14). Always on.
- **Spell-limit marker:** the quickbar dims a spell red while the server would refuse it because
  our last one is still out (spell_limits.cpp). Vanilla fails the cast silently. Always on.
- **Soft puffs:** toxic clouds and smoke blasts as particle puffs (Smooth FX).
- **Per-pixel lighting, smooth effects, glows and bloom:** noxsdl's own (Options' Pixel Light,
  Smooth FX and Bloom); the original draws none of them.
- **Escape closes an open inventory** first (as it does the all-sets bar and the spell book),
  before the in-game menu (2026-10-07).
- **The view is drawn behind the newest snapshot** for smooth motion, by a delay that adapts to
  the connection (one frame plus the measured lateness, ~50 ms on a steady line; interpolator.cpp);
  vanilla draws the newest. Options' Smoothing: Smooth Network (the default: everything), Smooth Movement (the
  camera and us; other players then step against the smooth scroll), Off (as vanilla), Lock to 30
  FPS (Off, a frame drawn as each snapshot comes). --predict-movement (an experiment) draws us and the camera ahead
  of the server instead. (Since 2026-10-07, Smooth Movement eases the camera and every
  drawable to each new snapshot over a server frame, no clock: interpolator.hpp.)
- **An update stream that comes out of order is kept from putting things back** (2026-10-12): a
  datagram whose timestamp is older than one already seen (UDP overtaking, over the internet) still
  adds the objects it introduces, but nothing already placed by the newer frame moves back, and it
  makes no snapshot for the smoothing (World::applyStream's `late`). Vanilla applies it whole: us
  and everything back a frame for a frame, which Smooth Network made a visible kick back.
  F10's counter shows them ("late N").
- **--other-units ahead (an experiment; Smooth otherwise):** with Ahead, other players and monsters are drawn
  where they'll be when our input reaches the server (their velocity times the smoothing delay
  plus the learnt input lag, ~9 frames live), so what we aim at is where a spell will land;
  vanilla draws them where the server last said. Newest draws them so (no smoothing delay).
- **Front walls are always translucent** (vanilla's TranslucentFrontWalls, an option there).
- **The line of sight's drawing at the screen's resolution** (2026-10-13): the region is
  drawBlack's own (noxcore client::SightRegion: the same sweep, polygon, wall bits and spans), but
  the black is a mask over the polygon drawn at twice the screen's resolution (an anti-aliased
  edge at any zoom), not the game's per-row black fill; it fills by the even-odd rule as the
  game's spans do (a fan from the eye whose triangles each invert what they cover; a renderer
  without custom blending falls back to the plain fan). Soft Shadow
  Edge (nox.cfg SoftShadowEdge, on by default as the game's fast-machine default) draws the
  game's ten black lines (alpha 0xD0 down by 0x14, a game unit apart) into that mask as quads
  scaled with the view. Not changed: which floor is seen (but for the screen's last two rows,
  which the game's black fill leaves black: original_bugs.md, OB-181), which walls are drawn
  and how (but for the improved wall culling, the next item).
  Map objects and live drawables go by the exe's drawable sight test (the eye's line against the
  polygon's edges, then the box rule), and so do the effects that are drawables in the exe (sparks, glow orbs, bubbles, beams, tail links,
  explosions), with SIGHT_DESTROY's deletion, while the overlays a drawable's DRAW draws (name
  plates, health bars, team names, friend dots, damage numbers) go with their drawable's sort.
  What still differs there:
  - the effects drawn over the scene rather than sorted into it (sparks, orbs, beams, tail links,
    the drawables' own glows) are sight-tested where they're drawn, blended between game frames,
    each rendered frame (the exe once a game frame, before the spark draws move them); a
    SIGHT_DESTROY one goes when the first rendered frame of a later game frame begins;
  - Smooth FX's soft puffs are seen as the drawable each stands in for (a toxic cloud's GreenPuff,
    SIGHT_DESTROY; a smoke blast's Smoke, lingering and fading), each at its own spot;
  - the sentry rays (their own pass) are cut to the sight by nine samples along them where the
    exe clips them exactly at the polygon's edges (left open);
  - SightMap::visible (a point up to 2 units past an edge counting, one off the view where its
    line leaves the view) is left for a beam whose ends were never both known, and the minimap's
    and the predicted drawables' own uses (as before).
- **Improved wall culling** (2026-10-14; the console's `noxsdl walls classic on` brings back the game's for comparison). The floor's region,
  the drawables' sight test and the blockers reached stay the game's; only which walls are drawn
  and where they're cut differ (noxcore client::SightRegion::setImprovedWalls / wallView, noxsdl
  SightMap::wallRegion):
  - the walls are worked out by a second sight over the view grown by the wall images' reach (2
    cells left and right, 4 rows above and below, the eye where it was): a wall whose base is just
    past the view's edge but whose image reaches in is drawn when a larger screen would see it,
    whole, not cut straight down where its base leaves the screen or left out (the game's cells end
    at the view's edge and its column test cuts there);
  - the column test doesn't cut a wall where its base is hidden only by the wall's own arms (the
    nearest occluder in that direction is the same cell): a corner turning away behind its front
    arm is drawn whole, not cut through its pillar at its hidden half;
  - nor a see-over wall (WALL flag 1 off) where only a sight-blocking object hides its base: the
    see-over walls behind a door's leaf are drawn whole over the door's dark wedge (the floor and
    the things behind the door stay hidden);
  - a flat base (direction 2) is walked column by column like the sloped ones, so the game's quirk
    of drawing one wholly before a span from that span on is gone.
  Still the game's: which walls are reached at all (walls behind other walls stay hidden, and so do
  their cuts at other occluders' shadow edges), a junction drawn as the direction its seen arms
  make, the half images of directions 8 / 10. Walls drawn just off the screen are marked seen for
  the map as drawn walls are. Cost: a second sweep a frame, some 0.04 ms at 240 walls.
- **Fade Objects** (nox.cfg FadeObjects, on by default as the game's fast-machine default;
  2026-10-14): the rule is the exe's (noxcore client/fade.hpp: the scene sort for the general
  list, CliNet_OnPacket's MSG_OBJECT_IN_SHADOWS keeping the drawable gameFPS frames, the generic
  draw's alpha), drawn the GPU way. What differs:
  - the alpha follows the shown frame's fraction (smooth at any frame rate) where the exe steps it
    once a game frame; the sort and its second go by the shown (smoothed) frame, while the word of
    shadows and its expiry go by the server's frame. Both run the same delay behind the server,
    so they keep step; a jittery delay can end a told drawable a frame early or late, at a fade
    of 8 or less;
  - the halving for object flag TRANSLUCENT (0x4000000) isn't done: no stock thing has the flag
    (and noxsdl doesn't draw the flag's 0x80 outside a fade either);
  - nothing lingers in the 3D view (it has no line of sight).
  As in the exe: the map's own objects linger and fade too; what isn't drawn by the generic draw
  stays at full alpha; the Ghost keeps its own alpha; no fireball sparks while a drawable
  lingers; the expiry comes after the frame's messages; a lingering drawable's enchantment
  effects are drawn with it, unfaded.
- **The summon command list stays on screen at the bottom:** vanilla (wndSummonCreateList) moves a list
  that would go off the bottom to the screen's *width* less its height, below the screen on
  any wide mode; summon_window.cpp uses the height. Always on; revert in `SummonWindow::open`.
- **The scoreboard doesn't take the mouse:** vanilla shows the quest list and the full board
  (F9's first views, and the board a game's end opens) modal (wndShowModalMB). Here that left a player unable to move or fight with no sign
  why (seen live, 2026-10-02), so play goes on under it. Always on; revert by breaking out of the
  mouse-button case in `SDL_AppEvent` while `rankBoard->modal()`.
- **The mouse is read raw, a count a game pixel** (as vanilla's DirectInput, nextMouseEvent), with
  Windows' speed and acceleration on top by default (Options' Win Mouse Accel) and a speed
  multiplier (Mouse). Vanilla had neither switch. Off at x1.00 is vanilla's.
- **Options' Screen Resolution can be changed in a game** (2026-10-07): all
  four (640 x 480, 800 x 600, 1024 x 768, 1024 x 576) stay clickable and apply at once, the view
  re-fitted the next frame. Vanilla's in-game Options (initOptionsInGame) leaves only the one in use on.
  The server keeps the view size the join gave it, so a view grown wider than that may show its
  edges without moving things until the next join. Revert: options_window.cpp's
  `setEnabled(w, !inGame || current)`.
- **1024 x 576 in every install, and the first run's resolution** (2026-10-23): an Options.wnd
  without the 1024 x 576 button (only some installs' files and string tables have it) gets a copy
  of the 1024 x 768 one a row below it, labelled "1024 X 576", working as the file's own does.
  Without a nox.cfg (default.cfg's 640 x 480) or a VideoMode in it, the view is 1024 x 576 (was
  1024 x 768), most screens being 16:9. Revert: options_window.cpp's added button and main.cpp's
  default "1024 576 16".
- **Screen Resolution is a drop-down, with an ultra widescreen 1376 x 576** (2026-10-28): the five
  don't fit the panel's box as radio buttons, so a drop-down (as the advanced panel's) takes their
  place, labelled "1024 X 576 (16:9)" and "1376 X 576 (21:9)" for the widescreen two. 1376 x 576 is
  3440 x 1440 screens' shape: 576 high as 1024 x 576 and about the area of 1024 x 768 (792k against
  786k units), more of the world sideways and less up and down than a 4:3 player. A VideoMode wider
  than 1024 is held to 1376 x 576 (app_settings.cpp), the rest to 1024 x 768 as before; the server
  sends what the join's screen size covers, as vanilla's does for any size.
- **Options' advanced panel has two tabs** (2026-10-14): General and More
  along its top, drawn as the Server Settings' tabs (servopts.wnd's UITabs art); More holds the
  rarely changed switches (Soft Shadow Edge, Fade Objects, Render GUI, Frame Rate), General the
  rest. The game's panel (AdVidOpt.wnd) is one list. It opens on General every time.
- **HD images are loaded ahead, and the map waits for them** (2026-10-13; only with the HD
  image pack): an image the pack holds in HD is never drawn as its
  SD original. At start-up the images any map can need (players' bodies, armour and weapons in
  every state, carried and thrown things, spell effects, thing.bin's named images and the spell
  icons: map_preload.cpp sessionImages) are decoded on worker threads and kept for the session
  (cap kSessionCap); as a map loads, its own images (floors, edges, walls, doors, objects,
  monsters) are decoded and kept till the next map (beginMap; caps kPreloadCap / kCompositeShare /
  kTilePreloadCap). The map isn't drawn (the view stays black, the HUD up) till both are in and
  the HD floor in view is composited, at most 20 s (main.cpp holdMap; logged as "map X: shown
  after N ms"). Anything else is read and made in the frame that first wants it; a floor tile's HD
  composite is built on the workers while that frame waits (awaitAsked, 500 ms a round at most,
  then the SD tile). A BC7 pack's images go to the GPU as BC7 on shared pages (gpu_atlas.hpp).
  Vanilla (getImagePixdata) loads each image as it's drawn, through an 8 MB cache
  of video.bag segments that resets only on a colour-depth change, and shows the map at once.
  Without the pack (2026-10-20), a map is held the same way for the few frames it takes to make
  the original images its first view draws (floor tiles and their edges, walls, the map's things
  round the view: MapView::prefetchOriginals, 8 ms of them a frame; ~60-80 ms on Estate): made
  all in its first frame they had stopped the window 150 ms, and 2-12 s under heavy machine load.
  `--hd-placeholders` draws the SD original till the workers' HD
  image is in and doesn't hold the map (the previous behaviour); `--hd-decode-sync` decodes on the
  drawing thread with no workers, no preload and no hold.

- **A CTF flag icon's tooltip is worked out as it's shown** (hud.cpp, Kind::Flag): from the
  icon's state and whether it's our team's (framed) at that moment. Vanilla sets the tooltip
  when the flag status arrives (Gui_Ctf_SetFlagStatus), from the frame as it was then,
  leaves it for a state other than 0-2, and doesn't reset it when Gui_Ctf_Show puts every icon
  back home; so its text could lag the picture. Always on.
- **HUD text is TrueType** (text.cpp; as everywhere in noxsdl), sized and placed to match the
  game's 9-row default / small fonts: capitals 7 game pixels tall, their tops where the exe's
  drawString puts the cell's top (Font::drawGameText). Widths are the TrueType font's.

### VD-06: Solo cinematics and briefings (noxsdl/src/cinematics.cpp)

The fades, widescreen bars, map intro, chapter / death screens and the load menu after a death
are the exe's (records, timings, levels, texts, voices, music). Where noxsdl differs:

- **The ending movie and the credits from the game:** EndGame leaves the game and the main menu
  plays movies\<Class>.vqa, then rolls the credits over the menu. noxsdl's main menu rolls the
  credits for its Credits button, but the ending doesn't go through the menu: the movie plays from the game (over
  its black, the game going on unseen) and then the credits (the same screen, text, voice and
  music) roll there, the game left when they end. Revert: leave the game first and run the movie
  and `showBriefing(0xFF, 1)` from the main menu.
- **TrackData is written at once:** EndGame's bit for the class is saved in nox.cfg when it's
  set (the exe keeps it till nox.cfg's next save).
- **The briefing and load menu are scaled to the window** like noxsdl's other 640 x 480 screens;
  the exe draws them at 1:1, centred, at a bigger resolution.
- **The screen's "client frames" run at 30 a second of real time**, independent of noxsdl's
  frame rate (the exe ran one a game frame in a solo game); their darkening is drawn per pixel
  as the exe's 16-bit saturating subtract, on 8-bit channels.

### VD-07: The main menu's Solo (noxsdl/src/menus.cpp)

The screens, their lists, questions and files are the exe's (client/solo_menu.hpp). Where noxsdl
differs:

- **No CD check:** the CD check (the game CD's kind, 2 for the full game's) always passes.
- **--save-dir moves the solo saves too** (noxsdl's option, none in vanilla): a solo game's
  AUTOSAVE / SAVE%04d / WORKING go under the folder named, as the characters do.
- **No movie:** none is played on this way into the game in the exe either (a new character goes
  from the looks screen straight to its class's first map and chapter screen).

### VD-08: The main menu's Solo Quest (noxsdl/src/menus.cpp, cinematics.cpp)

The quest chosen, the host's chat map (So_Druid), OTQuest.rul's startSoloQuest, the quest's
briefing screens (MSG_GAUNTLET 0x0C / 0x0D / 0x0E: their pictures, texts, things, colours, layout,
blinking and timings) are the exe's. Where noxsdl differs:

- **Straight to the character screen:** the exe opens the Online screen and noxworld's game list
  and presses Network, Host and the world map's So_Druid itself; noxsdl skips
  those screens and hosts the quest from the character screen. Revert: run the menus' Online
  path with those presses.
- **No expansion check:** the expansion check (the SKU over 0x24FF, else InformExpansion) always passes;
  every copy noxsdl runs on has the quest's maps.
- **The quest screens are scaled to the window** as the chapter briefings are, in noxsdl's fonts;
  the things on the start screen are drawn from their thing.bin art (ThingArt: their animation by
  the client frame), where the exe makes drawables and runs their draw functions.

### VD-09: A quest's game over, shared keys and saves (noxsdl/src/quest_windows.cpp, main.cpp, noxcore/src/client/solo_saves.cpp)

GGOver.wnd and SKey.wnd (MSG_GAUNTLET 2 / 0x18), the inventory's SharedKeyMode icon, the quest
list's key marks, the locks on quest doors, the client's quit and its keeping of SAVEDATA are the
exe's. Where noxsdl differs:

- **The two windows are centred in the 640 x 480 layout scaled to the window** (as the MOTD's),
  where the exe centres them at their size on its screen.
- **The key marks' ring** is plotted as twelve game pixels (drawCircle's own points for
  radius 2 not read), each scaled with the board.
- **The host leaves a quest at once:** the exe's host quitting saves every quest
  player and sends each its SAVEDATA before it closes; noxsdl's host goes as out of any game.
  Revert: run GameHost's questSaveAndKick for everyone in clientQuit's host branch and wait.
- **Our summon order in a kept save is 4:** a client's own Player record's (set once there,
  nothing else sets it on a client); a host's would be its server Player's SummonOrderAll.
- **No character file, no save kept:** a game joined without one (--connect without --plr) logs
  that the host's save had nowhere to go; the exe always has clientCurSave.

### VD-10: The host's Server Settings (noxsdl/src/server_options.cpp)

The in-game menu's Gameplay Controls opens servopts.wnd for the host as the exe does: the Game
page's name, game type (in a chat map), maps of the type, limits, Defaults, "Form a new game", Go
and Cancel (ServOpts.c). Where noxsdl differs:

- **A Bots tab of its own**, after Game and Server: bots the hosted game runs (noxcore's BotSquad
  joined to it over the loopback): how many of each class, the difficulty, those in the game and
  All Leave. They go when the hosted game ends. Revert: drop addBotsPage and the callbacks.
- **"Form a new game" goes to So_Druid:** the exe goes back to the chat map the host picked on
  noxworld's world map (from switchChatMap); noxsdl's Host has
  no world map yet.
- **Host and Host Quest start on So_Druid:** the exe's Host (0x2712) and Host Quest (0x2713) show
  the world map, whose click picks the chat map; noxsdl hosts So_Druid straight away. As in the
  exe (SrvNet_OnClientReady), the host's arrival in a chat map out of a quest brings the Server
  Settings up; the Sanctuary help (sanctuaryHelp) and a WOL host's connection type window that
  the exe shows first instead aren't done. In a quest the window
  isn't shown: its first load's OTQuest.rul begins the quest at once.
- **The game list's description box shuts on a press outside it (noxcpp, 2026-10-11):** the exe's
  box (serverInfoBlockCheckExp) is modal till Join or Escape; noxsdl's also shuts on a mouse press
  anywhere outside it (the press does nothing else).
- **Host Quest/Delve (noxcpp, 2026-10-11):** the game list's Host Quest button (0x2713) reads
  "Host Quest/Delve" and asks first (dlg.wnd, its Yes and No as Quest and Delve). Quest is the
  stock Host Quest above. Delve hosts Delving Mode here (server/delve_host.hpp, VD-23): a
  network character (as joining a delve server), its generated lobby, the portal's
  run; DEL_1 and DEL_LOB written in the game's maps folder and deleted when the host closes; no
  Server Settings for its host (the director picks its maps).
- **Defaults** puts back the type's limits the game began with (the exe refills the fields from
  the settings); like the exe it deletes the map's user.rul and reads its own .rul.
- **The Server tab's pages are loaded with the window**, hidden, and filled as they're shown; the
  exe makes and destroys each page's window as its tab is chosen. The Teams page's lists are
  refilled twice a second (the exe's are told of each change).
- **The General page is the LAN one** (general.wnd) on WOL too: wGeneral.wnd's connection type and
  update rate (rateUpdate: updates every 1-4 frames, MSG 0x9F to the clients) aren't
  done yet; nor nox.cfg's rate. GameHost::setNetRate has the rest (the change told, MSG_RATE_CHANGE;
  a WOL game's players flushed every `rate` frames), the rate staying 1 till they come.
- **Latency compensation** is set and announced (report.c's lines), but the host's own input isn't
  held back by it yet (ctrlevent_add_ticks: the exe's host client is in-process).
- **The message of the day** window (motd.wnd) also closes with Escape.
- **Access:** the experience limits stay hidden as the file has them; a ban is by name (the exe's
  Ban also keeps the player's serial). "Limit Clients Max Resolution" is shown in the game list
  as the exe's, which nothing enforces.
- **The game list's view of the game** (the discover's SERVER_INFO) is made anew each frame (the
  map, its mode, the name, the Access settings), as the exe makes it for each discover (GameHost
  had kept the first one: a fix).
- **Advanced...** (advanced.wnd: rule sets, spells, weapons, armour) is the exe's, its pages
  loaded with it and shown in turn (the exe makes each page's window as it's chosen). The rule
  sets are listed by their file names; the exe lists the 8.3 short names (cAlternateFileName,
  which Windows leaves empty for a name that is already 8.3, so its list shows blank lines). Save As is
  usable whenever a name is typed that isn't the map's or "user" (the exe checks the name as the
  field is left). The spell and item lists step a font height and a pixel a line, as the exe's
  draw does (the file's 16 for the spell list isn't a line height).
- **The rules are enforced by the server:** the spells a game's rules leave out can't be cast and
  the weapons and armour can't be picked up (GameHost::applyRules at each map load). In the exe
  the masks only go to the clients (MSG_GAME_SETTINGS_2) for them to keep to. The Flag is never
  kept from being picked up, even though Disable All clears its bit (1) as the exe's does.
- Not yet: a guest's read-only view and a quest's settings.

### VD-11: Hosting: the update rate to noxsdl players (GameHost::sendSubstep, MSG_NOXCPP_SUBSTEP)

A game noxsdl hosts can update noxsdl players 60, 90 or 120 times a second instead of 30: the
Server tab's General page has noxsdl's own Update Rate button (30 / 60 / 90 / 120 Hz, kept in
noxcore.cfg as ServerUpdateRate). The game itself stays at 30 frames a second:
logic, timers, scripts and every decision run on the classic frame, and each frame's state is the
stock server's (the substeps' positions are put back at the frame's start). Between frames the
server carries the important objects (players, monsters, missiles) on by their motion and sends
each identified client where those it knows are (MSG_NOXCPP_SUBSTEP 0xF2: the substep, the frame,
the objects' net codes and positions, unreliable), which noxsdl blends in at frame + k/m. One
goes every substep, the header alone when nothing it knows moved, so the client's clock keeps the
rate (Smooth Network then waits about half a frame, not one and a half: measured 17-21 ms at
120 Hz against 49 ms at 30, hosting locally). Every game noxsdl hosts (solo and Solo
Quest too) runs at the setting, 60 unless it's changed.
noxsdl's own host runs on its own thread (LocalHost: each millisecond, the substeps as they're
due): at 60 fps a 120 Hz host had sent its substeps in pairs every 16.7 ms; now about every
8.3 ms. Its lock is its own: the SDL loop's frame never holds it up (a 200 ms frame each second
had left 215 ms gaps between the host's substeps; now 16.7 ms as ever), so a solo game's server
also runs on through a slow client frame, where GAME.EXE's one loop would wait (its map changes
wait for the client's CLIENT_READY, as a network game's do; kept so, 2026-10-14). The host's calls into its own client
are queued in order with the messages it sends it (local_host.hpp). What the server reads of its
own client between those calls (the widescreen on or moving, a briefing up, the map drawn, an
award flying, the inventory grid's room for a pickup check) is the client's view as its last frame
published it, the calls still queued guessed on top: up to a frame late, where GAME.EXE reads its
client there and then.
Between frames the important objects go k / m of the way to where the next frame takes them, but
never into a wall (the frame's own move trace, its body's radius on: they hold at their last clear
substep): their motion, or for a player walking, running or standing the next frame's own move
(predictedFrameMove: Plr_UpdateState pushes it by the state and facing the last frame left, its
new controls only steering the frame after, so a start, run or stop is shown as the frame makes
it). Nothing is carried on while the next frame moves nothing (a solo game paused, a match over,
a map's load asked for). The server reads its players' packets only in the frame, as the exe
does: what comes between frames waits for the next one. The frames
themselves are the classic ones at every rate (tested at 30 and 120 Hz with the controls coming
in between frames).
Identification: before joining, noxsdl sends the game list's extended query (00 00 F0 "NXSD")
from the socket it then connects from; a noxcpp server marks that connection, a stock server drops
the query (its packet handler: no connection yet), and a stock client never asks, so neither ever sees the
message. Checked live against a stock GAME.EXE server on XWIS: the join went as usual.

### VD-12: noxsdl on a noxcpp server: fresh input (HeadlessClient::setFreshInput)

Joined to a server that answered its extended query, noxsdl also sends its facing, moving and
cursor as they change between its 30-a-second input frames (at most every 8 ms; one-off
controls, attacks, jumps and spells, keep to the input frames), so the server's next frame takes
the newest instead of what the client's own timer last sent: on average 35 ms from a move to the
server's frame taking it becomes 19 (test). The server's frames read them as GAME.EXE reads any
client's (only the newest of each move stands, the last facing wins); a stock server is never
sent them.

### VD-13: Hosting: the port opened by UPnP (noxsdl/src/port_mapper.cpp)

A network game noxsdl hosts asks the router to forward its UDP port to this computer (UPnP,
miniupnpc), and removes the mapping when the game ends. Where it can't (no UPnP router, the router
refusing, or behind another NAT), a centred message says so once the host is in, with the port to
forward by hand. GAME.EXE leaves the port to the player. Solo games and Solo Quest don't map.

### VD-14: Centred messages (noxsdl/src/centred_messages.cpp)

- **Sized and placed as the HUD:** the exe draws them in its default font at its resolution;
  noxsdl's line is 10 of the game's pixels tall scaled by the HUD's scale, at the exe's place in
  the view (3/4 down, less 15 scaled).
- **A player joining:** the exe prints cdecode.c:PlayerJoined for every MSG_PLAYER_JOIN (0x2D);
  noxsdl for a player the client hadn't been told of (HeadlessClient's PlayerJoined). Revert:
  print it from the message itself (serverMessageText).

### VD-15: Movies (noxsdl/src/movie_player.cpp, legal_screen.cpp)

When they play and how they're skipped are the exe's (noxcore client/movies.hpp); the VQA
frames and sound are its decoder's (noxcore video/vqa.hpp). Where noxsdl differs:

- **HD movies:** HDVideo\<NAME>.mkv beside noxcore.exe plays instead of the VQA (FFmpeg,
  hd_video.cpp); not in the stock game. Revert: remove the HDVideo folder (or --no-hd-movies).
- **Played in a window too:** GAME.EXE in a window (dxFullScreen off: render flag 0x10 set)
  plays no movie at all (the movie player skips it, the chain goes on); noxsdl plays them
  windowed and full screen alike. Revert: skip the movies when noxsdl isn't full screen.
  An HD movie is timed as its VQA (the VQHD's frame rate and count), and is played when only it
  is there (no VQA): noxsdl's own.
- **Scaled to the window:** the exe switches the display to 640 x 480 for a movie and puts the
  640 x 400 picture at y 40; noxsdl draws that 640 x 480 scaled to fit the window and centred, as
  it draws the menus (smoothly filtered). The pointer isn't drawn, as the exe hides it.
- **Frames by the mixer:** frame n shows once the mixer has played n / 15 s of the movie's sound
  (the exe times frames by its own DirectSound buffer), and none is dropped (the exe
  drops frames only when it falls behind). The sound plays on our mixer at full volume beside the
  held music, instead of the VQA library's own DirectSound with the game's Miles device released
  ; the wait for the music to fade is at most 3 s here.
- **Start-up for automation (noxsdl's options, none in the exe):** --menu-clicks, --mouse-script
  and --screenshot go straight to the main menu (as noxsdl did before the movies); --startup
  plays the start-up anyway; --movie NAME plays one movie first; --no-movies plays none while the
  chains go on (the exe's -safe, engine flag 0x200, does that too, along with a 640 x 480 window,
  no sound and no threads; Shift held as a movie starts skips it in both); --hd-dir DIR looks for
  the HD movies and FFmpeg there. Revert: drop the options.
- **The legal screen:** its 300-frame count runs at 30 a second of time (the exe counts the frames
  it draws); 9999 shows noxsdl's version after "V:" (the exe's own version string); 9998, which
  the exe fills from eight bytes of its own file at offset 0x1C, stays empty.
- **TrackData's 0x80** (the intro seen) is written to nox.cfg once the intro ends, at once (the
  exe sets it when its start-up chain returns and saves nox.cfg at exit).

### VD-34: A summoning's spell-bar slot darkens for want of mana

- **Vanilla:** quickBarWarriorDraw darkens a slot's top by the share its spell's cost exceeds the
  client's mana, the cost read by spellManaCost's ordinary mode (the spell's byte in thing.bin).
  That byte is 0 for every summoning, which the server charges from its own per-creature price
  table instead, so a conjurer's summon slots stay bright however little mana is left
  (original_bugs.md, OB-223).
- **Ours:** for the summonings the spell bar reads the same per-creature price the server charges
  (things::summonManaCost in noxcore/include/noxcore/things/spell_ids.hpp, the one table both use;
  noxsdl/src/quickbar_mana.hpp's slotManaCost), so a summon's slot darkens and refills by the
  exe's own formula exactly as an ordinary spell's does: 15 for a Bat up to 85 for an Ogre
  Warlord. Every other spell, Charm included, keeps its thing.bin cost. The trap tray reads the
  same prices, which matters only for Summon Bomber, the one summoning thing.bin lets into a trap:
  its tray slot darkens below 30 and the slots after it are judged with its 30 paid, as the
  server charges it in a trap. Always on; nothing changes on the wire or at the server.
- **Why:** a deliberate, approved deviation (2026-10-20): the original's behaviour is an unhelpful
  bug, the slot looking castable while the server refuses the cast for want of mana.
- **Revert:** drop the isSummonSpell branch in slotManaCost (the slot then reads thing.bin's 0).

## Sound (no effect on the wire)

The engine's logic is the exe's; these are where the output differs.

### VD-16: Sound output

- **Mixing:** float mixing at the device's rate with cubic resampling, instead of Miles' 16-bit
  mix at 22050 Hz; each channel's gain glides across a render block rather than stepping when
  a ramp moves, and a soft knee above 0.8 rounds off what would clip. Miles' 127-step volume
  and pan become continuous. Revert: none needed for behaviour; the levels are the same.
- **Pan:** Miles' pan law isn't documented; noxsdl uses a balance (centred, both sides at full
  volume; one side falls off as the other stays full). Revert: `balance()` in mixer.cpp.
- **Timing:** server sounds are held until the render clock reaches their frame, so they line up
  with the interpolated drawing (Smooth Motion's delay, about 50 ms, after they arrive). With Smooth Motion
  off they play as they come, as vanilla does.
- **Sample cache:** Miles' 1 MB cache and the eviction of lower-priority sounds when it fills
  aren't copied; decoded samples stay while they're used (a 64 MB budget).
- **Server options changed:** the ServerOptionsChange sound isn't played for a session's first
  game settings (vanilla compares with the game list's copy, which matches anyway).
- **CD fallback:** music and speech are read from the game folder only; vanilla also tries the
  CD, and pauses the music for speech when both stream from it.

## Client: screenshots and gamma (no effect on the wire)

### VD-17: Screenshots and gamma

- **Screenshots (F12):** the game names them nox1.bmp, nox2.bmp... from 1 again each run, writing
  over earlier ones; noxsdl takes the next number not yet used.
- **Gamma (Insert / Delete):** the game set the display's hardware gamma ramp; noxsdl draws the same
  ramp over the finished frame with a shader (GPU renderer only; without it gamma has no effect).
  It keeps the level in noxcore.cfg, starting at 1 (no change), and doesn't read nox.cfg's Gamma:
  on today's Windows the original's ramp often did nothing, so players' files carry levels that
  would brighten everything here (seen: menus too bright for a player).

## Server

### VD-18: A change of the game's mode sends the settings at once

- **Vanilla:** MSG_GAME_SETTINGS goes at a join, at a console "set" (mainloop's
  SrvNet_BroadcastGameSettings after gameSettingsUpdated) and at the game's
  reset; a quest begun (startSoloQuest's countdown, game flag 0x1000) sends none before it dresses
  the players (MSG_PLAYER_RESPAWN), and the host's own client, in the same process, reads the
  flags as they are, so it puts on the quest's weapon (Cli_ResetPlayerDefaultOutfit).
- **Ours:** noxsdl's host client is a separate client that knows the flags only from the
  settings, so GameHost::tellModeChange sends them to every player before a frame's other news when
  that frame changed the flags (& 0x7FFF0). Without it a conjurer's quest bow wasn't drawn till
  it was put on again. Revert: make GameHost::tellModeChange record the flags without sending.

### VD-19: An important message to everyone reaches only those in the game as it's queued

- **Vanilla:** a Net_QueueImportantEx node to everyone (0xFF, or 0x80 | a player's index) is kept
  till each player in the game when it was queued (g_playersInGameMask) has it, and
  netImportant sends it to any ready player that hasn't had it:
  a player that came in (INCOMING_CLIENT) after it was queued gets it too if, at its CLIENT_READY,
  the node still waits on another (one slow to ack; after a map change, one still loading).
- **Ours:** it's held for (or sent to) the players in the game as it's queued, and never sent one
  not in yet (GameHost::sendImportantToAll): the same whenever the others ack before the newcomer's
  ready, and without depending on their timing.
- **Revert:** keep each message to everyone with the players it waits on, and send it at a
  CLIENT_READY while one of them hasn't had it.

### VD-20: The host's own player is known by its client's address

- **Vanilla:** the host's own player is index 0x1F, its client in the same process: it isn't sent
  MSG_UPDATE_GUI_GAME_SETTINGS (Plr_CreateFromJoinData) nor a new MSG_RATE_CHANGE
  (0x9F), and while it loads a map (game flags 0x900000) the main loop doesn't tell the Server
  Settings window's changes.
- **Ours:** noxsdl's own client is a network client with a slot like any other; noxsdl tells the
  host its game socket's address (GameHost::setHostPeer), which stands for 0x1F there (and a solo
  game's slot 0x1F as before). MSG_GAME_SETTINGS (0x9F too) still goes to it (above).
- **Revert:** none needed while the host's client is a network client.

### VD-21: A map that fails to load reloads the current one

- **Vanilla:** the map load failing (the map missing or corrupt) sends MapAccessError to everyone,
  shows the MapCorrupt dialog and ends the game (Game_SetContinueMenuOrHost(0); the tick returns
  before its end).
- **Ours:** GameHost::loadNextMap loads the map that was running again, so a dedicated server keeps
  going (kept so, 2026-10-07).
- **Revert:** end the game in loadNextMap's failure branch instead of reloading.

### VD-22: The map vote (server/map_vote.hpp, GameHost::voteFrame, MSG_NOXCPP_MAPVOTE)

- **Vanilla:** a finished match waits 10 s (300 frames) and loads the map cycle's next map (or
  the same one); the players have no say, and a running match never changes map unless the host
  or a sysop loads one.
- **Ours (on unless switched off: the General page's Map Vote, noxcore.cfg's ServerMapVote;
  never in a quest or a solo game):**
  - At a match's end the server picks up to four maps the game's mode can be played on (the
    cycle's next first, the map just played left out) and announces them in chat as numbered
    lines. Players vote by saying a number; noxsdl players also see a window with each map drawn
    from above (noxsdl/src/map_thumbnail.cpp, map_vote_window.cpp) and click one; an arrow at
    the window's top right folds it to its title line (at the top of the screen) and back. The vote runs
    20 s: the game-over wait is held till it ends (Server::holdGameOver), then the map with the
    most votes is loaded (a tie goes to the first listed; no votes, the cycle's next as stock).
    While that vote holds the match's end, connects are taken and the clients updated, with the
    transfers (GameHost::endVoteHolds; stock refuses connects and sends nothing in its 10 s): a
    client joining or loading then gets in and sees the vote, not a black screen till the next map.
  - In a match any player can call a vote: a stock one by saying "?MapVote" (in any case), a
    noxsdl one with the in-game menu's Vote Map (MSG_NOXCPP_MAPVOTE kCall): 30 s, and the map
    changes at once when more than half the players pick the same one.
    One can be called once every 5 minutes, from the last one called, whatever came of it (the
    refusal says how long is left); a match ending closes it.
  - On a chat map (no match ends there) a called vote offers the other chat maps, the So_ social
    maps, and the game stays a chat game.
  - noxsdl's in-game (Escape) menu: its Vote button is "Vote Map" where a vote can be called,
    which sends MSG_NOXCPP_MAPVOTE kCall. The server tells identified clients whether one can (the vote
    on, not a quest: MSG_NOXCPP_MAPVOTE sub 3) at the join and whenever it changes; elsewhere,
    stock servers included, the button stays off (the stock kick vote isn't done).
  - The number and "?MapVote" lines are still relayed as chat. Stock clients see only chat lines;
    noxsdl clients that identified (the extended query) also get MSG_NOXCPP_MAPVOTE (0xF3: opened,
    the tally, closed) and call and vote with it alone: their chat is only chat (the chat
    commands are the stock client's).
- **Revert:** Options::mapVote = false (or the General page's box off).

### VD-23: Delving Mode (server/delve*, MSG_NOXCPP_MODE_F4)

- **Vanilla:** no such mode. A quest shows its rules, stage and results screens and sends each
  player its quest save at a level's end.
- **Ours (a delve server only: noxsdl's Host Quest/Delve, VD-10):**
  - Generated quest levels from a chat lobby: a run of acts, each a map of rooms with forks
    between them, the party choosing its way as it goes.
  - GameHost::Options::questScreens off: a quest map starts faded in (MSG_FADE_BEGIN {1, 1}) with
    no MSG_GAUNTLET 0x0E / 0x0D / 0x0C. questSaves off: no quest save is sent (the players play
    their network characters, which a stock client would overwrite with it).
  - noxsdl players that identified get the run's map (MSG_NOXCPP_MODE_F4 0xF4: the acts done, the
    act under way and where the party is, its fork's two acts) at their join and with each level
    made, and see it behind a MAP button left of the meters (modes/delve/ui/delve_map_window.cpp).
    Stock players are told the same in text lines.
  - A thing of a map's carried into the next one (a picked-up potion, a chest's armour) is
    numbered afresh there (our extent is its net code; the exe's keep their NetCode across the
    switch), so its holder's client is told: MSG_REPORT_DROP for the old number, then
    MSG_REPORT_PICKUP / _MODIFIABLE_PICKUP, _ITEM_HEALTH, _CHARGES and _EQUIP for the new
    (Server's load, after the carry). Without quest saves nothing else tells it, and the potion
    keys and inventory named things no longer held. (A solo game's carry gets the same.) Not a
    delve server's alone: it runs at any server's map change that carries a map's thing (solo,
    quest or network game), the client ending as the exe's, knowing each thing by the server's
    number.
  - Multiclass for noxsdl players (GameHost::Options::multiclass, on for Delve; a player whose
    client identified with the extended query, PlayerControl::multiclass): any class's spells,
    abilities and guides learned (books) and used, any class's weapons and armour
    (Items::classMay), a conjurer's mana and a warrior's strength (applyStats: a weapon's or
    armour's strength wanted, and a weapon's damage over it, a warrior's), its abilities in a
    book of their own (abilityLevels: a warrior's are
    spells 1-5's levels in the exe). Told the client with MSG_NOXCPP_MODE_F4 kMulticlass at each
    CLIENT_READY; noxsdl then shows one book and bars of spells and abilities (an ability's id
    tagged). Stock clients' players play their classes exactly as stock; nothing reaches a .plr.
  - Alert monsters (GameHost::Options::monstersAlert, set by hookDelveHost): a delve's
    monsters look for enemies all about them, as an ALERT one does (Monsters::look skips its
    "in front, the cosine at least 0.5" test). Same reach (a quest's 640), same line-of-sight
    check, same look timing. They notice a party coming in from the side or behind at once,
    instead of when it crosses their front. A map can't ask for it: spawnFixup takes a placed
    creature's ALERT bit from its type, as the exe does. Off in every other game.
  - Records (DelveRecords, a delve_records.txt kept only where the dedicated servers keep their
    leaderboard; a game hosted from noxcore.exe keeps none): each run's deepest level, party and classes, filed as the run
    ends. Passing the deepest on record is told to everyone. "?record" (also "?records" or
    "?deepest") in chat lists the top five to the one who asked (a stock client's line is
    taken, not relayed). noxsdl gets the top ten as MSG_NOXCPP_MODE_F4 kRecords: a "TOP" button
    in the lobby, and the record to beat on the run's map. The lobby has a records board in its
    middle (SignShop01, the game's largest sign; its own text is a string id, so it can't show
    the table): a player coming within 100 of it is told the top five in chat lines, at most
    every 20 s, not again while it stays there.
  - A fallen party (every one at its quest's game over): the run ends three seconds on. Each
    one's things and gold are gone, and its spells, abilities and guides are cut back to its
    class's first (startQuest's grants; revokes told). Then it's back to the lobby, where its
    respawn gives it its class's clothes again (playerMakeDefItems). In the stock quest a
    game over is answered and played on.
  - A fresh seed for every run after a server's first (DelveDirector::runSeed_).
  - Campfires (DelveDirector::runCampfire): no heal (levels heal between them); each player by
    the fire strengthens one known spell a level, free, once there, to 5 (past a quest's 3;
    not a quest's once-only spells, nor a group's members, which rise with their head). A
    noxsdl player picks from a window (MSG_NOXCPP_MODE_F4 kCampfire to it, kUpgrade back: 0xF4
    now goes client to server too, identified clients only). A stock player is told the
    spells in numbered text lines and answers with a number in chat; GameHost::Options::onChat
    takes that line, so it isn't relayed. The new level goes to the client as
    MSG_REPORT_SPELL_AWARD. It's celebrated with the level-up sound (0x386), a LevelUp glow and
    a rain of coloured sparks (MSG_FX_BLUE/YELLOW/CYAN/VIOLET_SPARKS) for 2.5 s.
  - Barrels and crates (DelveDirector::runBarrels): a stock one breaks with nothing inside
    (CreateObjectDie). A delve's each roll as the level starts: low loot one in three (a reward
    marker of weapons, armour, gold and potions two stages down), a small first-tier creature one
    in seven, a gem one in sixty. Given where it broke.
  - Elites (DelveDirector::makeElite): from depth 4 a fight's few monsters (one every three
    levels deeper, three at most), an elite room's champion, a lone champion and a guardian from
    act 2 get traits: Swift (Hasted), Vampiric (Vampirism), Reflective (ReflectiveShield), each
    for good; Tough (half again its health); Regenerating (a fiftieth of its health a second).
    Told to everyone with the level's line. Each fight elite carries a reward.
  - Mysteries: a room of their own, rolled as the level's made: a stash (two potions), a purse
    (a little gold), a healing shrine, a mana obelisk, a skirmish (three or four of the tier below
    and a small find), 16% each; a campfire (as above) or a lone champion (three times its health,
    a reward three stages up dropped as it dies), 10% each. A campfire mystery only on a way with
    no mystery before it.
  - An act's rooms: any way through it meets one shop at most and two campfires at most (row 5's
    and a mystery's).
  - Blessings (DelveDirector::runAltar, runBlessings): every act has a blessing room (an altar,
    DunMirAltar1), one at most on any way. Each player at the altar takes one of three blessings
    for the rest of the run, once there: Vigor, Wisdom, Might (a quarter more health, mana or
    strength each time: stacks), Swiftness (Hasted for good), Thirst (Vampirism for good),
    Warding (ProtectFromFire, ProtectFromElectricity and ProtectFromPoison for good), Renewal (a
    fortieth of health and mana back every two seconds), Fortune (10 + 5 x depth gold at each
    level's start). Wisdom only to casters, Might only to fighters (multiclass both). A raised
    number put back by the class's (a respawn, a new map) is raised again; health and mana go to
    the client as MSG_REPORT_TOTAL_HEALTH / _MANA. A noxsdl player picks from three cards
    (MSG_NOXCPP_MODE_F4 kBlessing to it, kBless back). A stock player is told them in numbered
    lines and answers with a number in chat (taken, not relayed); "?blessings" lists its own.
    Celebrated as a campfire's pick. The run over (a fallen party, or back in the lobby), the
    raised numbers go back and the enchantments come off.
  - Bonus objectives (DelveDirector::runObjective): a fight from depth 2 has one, told after the
    level's line ("Bonus: ..."): Purge (slay every monster that was there with the party's
    first in, a charmed one counted), Hunt (one of them made an elite of two traits, the
    quarry), Seals (three OrbPoolWithOrb placed, first in the side rooms; one of the party
    within 45 breaks one: it's turned off, MSG_DISABLE_OBJECT, drawn as the empty pool,
    ForceOfNatureRelease's sound), Swift
    (through a gate within 60 s + 10 s a segment: 20 + 10 x depth gold each, paid as the next
    level opens). Done: a reward marker's find a stage up at each one's feet (Swift: the gold).
    Progress is told in text lines; noxsdl draws it beside the MAP button (MSG_NOXCPP_MODE_F4
    kObjective, its clock counted down on the client).
  - Bot bosses (DelveDirector::runBotBoss): from act 2, half the guardians are a champion
    played by a bot of a random class (bot::NetBot in the server's process, joining over
    loopback UDP; the server loops pass the director their port). It's let into the locked game,
    taken out of observing, never quest-active (no lives, no part in the quest's exits, tallies
    or difficulty), put on a team of its own ("Champion": isEnemy makes it the party's foe,
    every client told by MSG_TEAM_MSG), set in the arena with its class's gear, spells (Death Ray
    from act 3) or abilities, health one and the act's number times, a caster's mana three
    times. Its level has no monsters (they'd be its foes too). Fallen, it's kept down (its
    questGameOverAt set) and quits three seconds on; the gates open as a guardian's. Not in
    within 20 s: the monster guardian stands in. It has no "[BOT] " prefix (a champion's name).
  - A wiped party's quest game over cleared as it's sent back and as a run begins.
  - Difficulties (DelveDifficulty, chosen in the lobby by "?difficulty <name>", a name's start
    enough; a run keeps the one it began with): Adventurer (as above), Veteran (monsters' health
    and count x1.25, elites from depth 3, one more every 2 levels, 4 at most), Nightmare (x1.6
    health, x1.5 count, elites from depth 2 of two traits, 5 at most, a champion or guardian a
    trait more, bot bosses play hard), Oblivion (x2.2 health, x1.75 count, elites every level
    from depth 2, 6 at most, as Nightmare's otherwise). Told in each level's line; records file
    it; a lobby sign says how.
  - Raised health kept (DelveDirector::keptHealth_): the difficulty's, Tough's and a
    champion's or guardian's health is put back within a second where the quest's rescale
    (questRescale, as the party's size changes) set an unhurt creature to its stock health
    The rescale itself is unchanged.
  - GameHost::Options::onChat sees a noxsdl player's lines too (Delve's "?" commands: ?record,
    ?blessings, ?difficulty; a campfire's or altar's number). The map vote still reads a stock
    client's chat alone.
  - Optional, off by default (setUpDelveHost's conjurers; the stock client keeps the quickbar
    of the class it joined with, so its spells there show "No Icon", and books follow the forced
    class): every player a conjurer in a run (GameHost::Options::forceClass 2), whatever its
    character: as the run begins (startQuest,
    before the class's start: Charm, Lesser Heal, a Bow) its class becomes 2 and its MSG_NEW_PLAYER
    goes to everyone again (GameHost::syncClasses; the stock client takes its own class from it:
    meters, book, bars, what it may wear: CliNet_OnPacket 0x2D), ahead of the next map's
    MSG_USE_MAP. Only in a quest without quest saves (forcedClass): the lobby isn't a quest, and a
    stock client leaving a non-quest network game writes its .plr's spell bars under its class
    (Game_FinishQuit), so there each keeps its own; back in a game with none forced (the lobby),
    its own class again (what the class can't use taken off, kept). Nothing of it reaches a .plr:
    quest saves are off and a stock client saves none on leaving a quest.
- **Revert:** don't run a delve server; questScreens / questSaves default on elsewhere.

### VD-24: Undead Tide (server/tide*, MSG_NOXCPP_MODE_F5)

- **Vanilla:** no such mode.
- **Ours (a tide server only: noxsdl's Host, as Delving Mode's):** a quest of one generated arena (UTD_1,
  written anew each run) from a generated chat lobby (UTD_LOB), hosted as Delving Mode's is
  (questScreens and questSaves off, its creatures alert: see above). The game's rules play on as
  stock; the director (TideDirector, outside the frame, as Delve's) only drives them:
  - Nothing to use by hand: as a player's unit first stands in the arena, its book, abilities and
    guides are emptied on the server (a cast or an ability by hand refused), but nothing is revoked
    on its client (since 2026-10-19: a revoke takes the spell off the client's bars, and the lobby
    is a network game without the quest flag, so GAME.EXE's Game_FinishQuit (and noxsdl's) writes
    the client's sections, the bars among them, into the .plr on quit: the bars were saved empty).
    Every weapon, wand and quiver it holds is deleted (a respawn's outfit gives a warrior a sword again: taken
    each frame). Worn armour stays. No bare-handed punch either (World::noBareHands: the Action
    with nothing in hand starts no attack, as in a chat game).
  - No extra life: its quest lives 0 at the run's start (QuestGameStartingExtraLives is 2); one
    that leaves and comes back mid-run gets its own lives again (an ankh's kept, no fresh ones).
    Out of lives (the quest's game over) it's out of the run: one that answers the game over's OK
    and comes back is put to observing again (Plr_GoObserver) till the run ends. A stock quest
    plays on with fresh lives.
  - Auto-weapons, kept in the director's own table (never in the player's records), fired by the
    server with the player as their source, whatever state the player is in (stunned, held...):
    a blow all round the player (Fight::attack as a war hammer's area, the loadout's damage, type 0
    with no weapon, a pairing no stock blow makes; yellow sparks, MSG_FX_YELLOW_SPARKS, where it
    lands); stock spells cast at the loadout's level as their power, the caster not knowing them
    (Spells::cast, its facing turned to the foe for the cast alone): Fireball, Death Ray, Meteor,
    Fist of Vengeance and Toxic Cloud at the nearest foe, Pixie Swarm and Earthquake about the
    player, Force Field (Shield) on it while foes are near; one fist and one meteor of its own at a
    time (none cast while the last falls, so the spell's "TooMany" refusal is never told);
    Multishot one more cast at the next nearest foe every two levels; Magic Missile
    (Spells::cast at the loadout's level as its power, the caster not knowing it; its own count
    in the air holds; each missile after the director's chosen foe, where Spell_CastMagicMissile
    takes the one under the player's cursor or the nearest to the cursor); arrows (an
    ArcherArrow made as a bow looses one, no bow or quiver, two, one more a level and one a
    Multishot level, fanned 8 apart; their harm the arrow's by the player's strength); Force of Nature's
    ring (since 2026-10-12): not DeathBalls but MagicEnergy sprites (a plain looping animation: a
    DeathBall's client draw, DeathBallUpdateDraw, sprays sparks each frame and buried the screen),
    up to five (the level; strength past five, Multishot's, turns it faster), placed each frame
    about the player for 4 s, out of their harm's reach of it; each harms as a DeathBall would, by
    the director (Missiles::damageAt from it, DeathBallNearbyDamage of crush out to
    DeathBallOutRadius, from 10 frames on, the player spared, the kill the player's by its owner).
    Being no DeathBalls, DeathBallUpdate's cancel and a Counterspell pass them by; World::heldBalls
    (the earlier rings' exemption) stays empty.
    Each hostile auto-weapon ends invisibility and the map start's invulnerability for
    good, as the stock hostile cast and an attack do (a timed invulnerability stays).
  - The tide: creatures of the biome made (CreateObject) just out of each player's view on a clock,
    their sight 450 (as a map's MonsterXfer may set it). Their health set by the director: the
    tide's own monster.bin's quest health (without the quest's multiplier) a quarter more
    each minute and more late (below: harder throughout); an elite's and the dawn's guardian's the
    network game's, 20% more a minute from the second; 40% more a player in play past the first;
    kept so: the quest's rescale at a change
    of the party's size (questDifficultyTick) is undone for them. The waves each
    minute from the second (middling creatures till the fifth minute, then hard ones); from the
    third, an elite with each, one a player: stock enchantments for good (Hasted, Vampirism, a
    reflective shield, more as the run goes on), three times the health (to five by minute 20);
    slain, a CryptChest
    (ChestCollide: a touch opens it), whose opening gives the player nearest it a free pick. The
    dawn's guardian (the biome's worst: a Lich Lord in the Crypt...) two minutes before dawn
    (TideDirector::Options::bossSeconds; at dawn with dawnWins off), six times the health (three more a player past the
    first), Hasted and Vampirism; slain, the party goes on to the next stage (below). One
    not fighting nor seeing an enemy is sent at the nearest player every 1.5-2 s as a map
    script's Walk sends one (Report + FAR_MOVE_TO); one left 1600 behind deleted. The slain's
    bodies are deleted 3 s after they die (a zombie that got up joins the tide again).
  - Soul gems: a BlueOrb / GreenOrb / RedOrb (lights, not items: no one can pick one up) made
    NO_COLLIDE where one dies, drawn to a player within 70 (more with Magnet) and gone into its
    XP; 2 minutes at most, 400 on the ground. Now and then an Ankh (AnkhCollide's life, as
    stock) or a piece of armour (a fresh RewardMarker of kind 0x10, the quest's own tables); one
    piece in three charmed, told to everyone ("A warded Leather Helm lies there..."), its effect
    the director's while it's worn by a player in play: Warding casts Shield on it every 20 s (the
    stock spell, power 3), Repulsion throws the tide within 140 off it every 8 s (applyForce 60,
    MSG_FX cyan sparks). The item itself stays a stock one.
  - A caster's mana is its XP bar (2026-10-12: the stock client has no window of the
    run's): set each frame to its most mana x its XP / the level's need, so the mana tube fills as
    soul gems come in and empties at a level up; told as any mana change (MSG_REPORT_MANA). Nothing
    in a run spends mana. A warrior has none (the stock client draws it no mana tube).
  - The damage players take: the quest's multiplier (World::questDamage, PlayerDamage's) set by
    the director each frame of a run, 15% at its start to 100% at dawn, a fifth more each stage
    past the first (the quest's own is 50% at
    stage 1: a network game's creatures hit hard, a zombie 20 every 10-30 frames).
  - Levels (the director's, not the player's: Player->Level and its XP stay as stock): each one
    offers three picks as a text line to a stock player ("Level 3! Say 1-3: ..."), answered by the
    number in chat (GameHost::Options::onChat takes the line, not relayed); to a noxsdl player as
    MSG_NOXCPP_MODE_F5 kOffer instead (no line), answered by a click or a key in its window, sent back
    as kPick (GameHost::Options::onTideMessage, read from an identified peer only, for the offer
    open now: a pick naming another offer's level is dropped), or by the number in chat all the
    same; unanswered 10 s, the first taken. A level up makes the player invulnerable 3 s and gives it a fifth of its health
    back, with the level-up sound and a LevelUp glow. The picks: a weapon's level, a new weapon of
    the player's class's (tideWeaponClasses), a passive's level. Vitality raises its most health
    (MSG_REPORT_TOTAL_HEALTH told; put back on after a respawn's numbers), Recovery heals it a
    little each half second, Haste shortens the weapons' waits, Magnet draws gems from farther,
    Multishot adds arrows, balls and casts, Wisdom adds XP. "?tide" / "?me" tells a player its
    loadout.
  - The second set (2026-10-12): 25 more auto-weapons (37 in all, 12-15 new for each
    class), a table in tide.cpp (its classes, spell, aim, reach, enchantment, waits). Stock spells
    cast as the first set's (Spells::cast, the player not knowing them): Burn, Ring of Fire
    (Cleansing Flame: one ring of its own at a time, so the spell's "TooManySpells" is never told),
    Confuse, Fear, Freeze, Slow, Stun, Blind on the nearest foes not under it yet, never the night's
    guardian (Stun the light foes it can hold first: a heavy one is only slowed); Poison and Meteor
    Shower on the nearest; Shock and Vampirism on the player while it's without them; Inversion and
    Turn Undead about it; Doom (Death) on the strongest foe near, never the night's guardian.
    Firewalk is in the table but no class's pick (its stock flames are nobody's and burn the
    party). Only a hostile spell (flag 0x20) ends the player's invisibility, as a stock cast by a
    user, and none while the Invisibility pickup's time runs. The director's own: Shockwave (the tide within reach thrown back by applyForce and
    hurt, impact), Gather (drawn in), War Cry (Held), Harpoon (the nearest foe set at the player's
    side, not the guardian, and hurt), Cleave (a wider sword blow that throws the struck back),
    Crossbow (an ArcherBolt at each of the nearest foes, loosed as a crossbow's); a stock Push or
    Pull would move every player within 600 too. Summonings (Wolves, Bear, Will-o'-wisps):
    summonAt (a stock summoning's end) near the player, its creatures (the player's creature list
    told), four at most in all counted by heads (past the stock control's limit, which counts sizes
    under 5: two wolves, or one wisp), brought back as they fall; the bear a BlackBear, from level 3
    a Bear.
  - Pickups: stock items (potions, Meat, amulets, a CrystalBall, Ruby /
    Emerald / Diamond) dropped by the slain (an elite's 40%, the guardian's always, the rest by
    their worth), NO_COLLIDE, their PICKUP the stock trap's (a thing given only to its owner: none,
    so no hand takes them), Meat made no Food (no creature eats it), and used at once by the
    player in play touching one (within 30), gone a minute on, 25 on the ground at most: health
    back, a cure, Haste, a Force Field (Shield), Vampirism, Invisibility, Invulnerability (6 s),
    every weapon ready, every soul gem's worth drawn in, a blast (150 impact within 350), the tide
    frozen or afraid about the player, XP. Armour drops only what a class in the run may wear
    (modifier.bin's CLASSUSE; drawn again up to six times, else none).
  - More creatures a biome: the same low ones, a fourth or fifth middling and hard one, the hard
    sprinkled into the trickle from the sixth minute (3% a minute, 25% at most), and specials
    (stock creatures of their own ways, each with its monster.bin entry and a way to harm:
    spellcasting wizards, beholders, wisps, mimics, albino spiders, urchin shamans, cherubs) in a
    group (up to four) about each player every 40 s from the third minute, told to everyone.
  - Harder throughout (2026-10-12): the tide's health
    x(1 + 0.25 m + 0.015 m^2) at minute m (12x by minute 20), elites' and the guardian's 20% more a
    minute, 40% more a player past the first; the trickle every 1.5 s from minute 8, 1 + 2m/3 at a
    time, 8 + 10m about a player; waves up to 5 a player; elites 3x health rising to 5x by minute
    20; the guardian 6x (+3x a player); the players' damage taken (World::questDamage) 15% at the
    start to 100% by dawn.
  - Stages (2026-10-12): the guardian's fall doesn't end the run: the tide breaks (no
    more creatures, those standing invulnerable) and 8 s on the arena's map is written again in
    another biome (the same one with TideDirector::Options::biome set) and loaded through the quest (World::exitTo, as Delving Mode's floors), the
    party's runners kept (levels, weapons, passives, kills), those out brought back (their game
    over cleared, back in play, no extra life), the clock anew. Each stage past the first: the
    tide 60% tougher and a quarter more of it, elites from the first minute with a trait more,
    the players' damage taken a fifth more (World::questDamage 4 at most). The quest's rejoin after
    a load (questFrame's, run every frame once past in the exe) is made the load's one-off: the
    director puts its clock away once it has fired, so the run's watchers stay watching. The run
    ends only when the tide has taken everyone
    (its record and results the stage's, the whole run's time). MSG_NOXCPP_MODE_F5 kHud and kResults
    carry the stage as a last byte (an older noxsdl stops before it).
  - Changes of 2026-10-12: no ankh drops at all (no extra lives but the shrine's); the
    night's guardian comes at dawn and only its fall carries the party on (Options::dawnWins off:
    dawn alone no longer ends the stage; a guardian left 1600 behind is set back near the nearest
    player, one gone another way comes again); the arena's scenery (immobile or obstacle things of none of the
    working classes, with no collide of their own: DefaultCollide, NoCollide, AudioEventCollide or
    none) NO_COLLIDE as the run begins, so nothing snags the players or the tide; fewer
    lights and effects: a soul gem falling within 80 of one on the ground is merged into it (its
    worth added, a Blue / Green / Red Orb by the sum), at most 150 on the ground, and one pixie
    swarm and one toxic cloud of a player's at a time (as its fists and meteors); Force of Nature's
    ring drawn as MagicEnergy sprites, not DeathBalls (above).
  - A difficulty (Delving Mode's four and numbers: Adventurer, Veteran, Nightmare, Oblivion):
    the tide's health x1 / 1.25 / 1.6 / 2.2, its numbers (the trickle, its cap, the waves) x1 /
    1.25 / 1.5 / 1.75, elites from minute 3 / 2 / 2 / 1, one more elite trait from Nightmare.
    Set in the lobby by anyone's "?difficulty <name>" (told to everyone; "?difficulty" alone tells
    it; refused once the run has begun), or by a noxsdl player's answer at the gate: a noxsdl
    player stepping on the lobby's gate is asked (MSG_NOXCPP_MODE_F5 kAsk) instead of beginning the
    run, its answer (kChoose) setting it and beginning; walking off puts the question away, and one
    left on the gate unanswered 15 s begins it (an older noxsdl knows no question); a difficulty
    change re-sends the open questions. A stock player on the gate begins at once. On Oblivion the
    first hard wave (its elite) comes at the first minute, not the second. Back to Adventurer after each run; filed with the run's
    record and shown on the board ("on Nightmare").
  - The players' own characters are never written (questSaves off, as Delving Mode's): beyond the
    server's own saves (the quest's level-won save, the 30 s game-over kick's), a client's request
    on quitting or Save (MSG_REQUEST_SAVE_PLAYER 0xC1), in a run or in the lobby (a chat map, where
    the in-game Save is enabled), is answered with an 8-byte SAVEDATA (GameHost 0xC1 case, whenever
    questSaves is off), under the 700 bytes GAME.EXE's client save handler (and noxsdl's keepSaveData) refuse
    without writing, so the .plr stays as it was and the client leaves as after a save (it shows
    "Wol.c:WolApierror" a moment). Stock, the server sends the unit as it is, which here is the
    mode's stripped one: it was written over the player's character (reported 2026-10-11).
  - A player's run is keyed by its WOL account and player name ("account/name"; the name alone with
    no account): two players of one name on different accounts have their own.
  - Revive shrines: a while after a player falls for good (60 s; again after each use while some
    are still out, those still in the game) a SoulGate rises 260-400 from a player in play, its collide set to
    DefaultCollide (the soul gate's look alone: no checkpoint, no warp-vote clock), told to
    everyone. A player in play within 45 of it brings back every fallen player who's watching (its
    game over answered): its team joined and out of observing as the quest's own rejoin does
    (the quest rejoin's joinTeam / leaveObserver), set beside the shrine (nearSoulGate), whole, 3 s
    invulnerable, no extra life; the shrine is spent (deleted). One still at its game over waits
    for the next.
  - Records: each run's end is filed (TideRecords: tide_records.txt, kept only where the dedicated
    servers keep their leaderboard; a game hosted from noxcore.exe keeps none; best
    first: the later stage, then the guardian slain sooner, then dawn reached, then the longest
    stand), "a run for the
    board" told when it places in the top ten. The lobby has a SignShop01 (the game's largest sign)
    at its centre; a player coming within 100 of it is told the best five as text lines (each
    naming four of the party at most, every one of the mode's lines at most 250 characters: the
    message's count is one byte; the file's runs merged in before each write, so two servers on
    one file keep each other's) (a sign's
    own text is a string table id: a stock client could show no other), at most every 20 s, not
    again while it stays there.
  - The run ends when everyone is out (the tide has taken them all); with
    TideDirector::Options::dawnWins on (off by default: tests' short runs) dawn (winSeconds, 20
    minutes by default) or the guardian's fall also ends it, won; the
    survivors invulnerable 11 s while the results are read, then everyone's things (but worn
    clothing) deleted and back to the lobby.
  - noxsdl players (identified clients) are told whether they're in a run (MSG_NOXCPP_MODE_F5 0xF5,
    kState, as it changes); in one, noxsdl's game-over window (GGOver.wnd) says "The Tide Takes
    You", its generators and secrets lines empty, and "You're out of this run. OK: watch the others
    till it ends. Quit: leave the game." in place of the quest's penalty question. Stock clients
    keep the quest's window and words. In a run each noxsdl player is also sent its run every 10
    frames (kHud: level, XP, the clock, the guardian's time and dawn's, kills, each weapon's level
    and wait, the passives), its offers (kOffer) and, at the end, the results (kResults: the best
    dozen players), all reliable; every open offer is closed at the run's end, no pick taken then; noxsdl draws them (tide_window.*: an XP bar and the clock along the top, the weapons'
    icons with their waits swept round, three level-up cards, a results window till OK). The
    MSG_NOXCPP_MODE_F5 op (0xF5) is framed both ways as MSG_NOXCPP_MODE_F4 (0xF4) is.
  - Listed as "Undead Tide" in the lobby and "Tide 3m" in a run (whole minutes: each change sends
    MSG_GAME_SETTINGS to every client).
  - On every noxcpp host, not a tide server's alone: map folders named UTD_* (as DEL_*, Delving
    Mode's) are left out of the Server Settings' map list and the quests' map list
    (GameHost::mapChoices / scanMaps; isTideMapName, isGeneratedMapName). The stock game lists
    every map folder.
- **Revert:** don't run a tide server (and drop the two name filters in game_host.cpp).

### VD-39: The co-op campaign (modes/coop, MSG_NOXCPP_COOP)

- **Vanilla:** the story campaign is one player's (a solo game: game flag 0x800 without 0x2000,
  the host's own player in slot 0x1F); nobody else joins it.
- **Ours (a co-op campaign host only: noxsdl's Host Custom > Co-op Campaign; modes/coop):**
  - The world runs the solo game's flags, maps and scripts unchanged; up to three guests join it
    (GameHost::Options::soloGuests) in their own slots below 0x1F, into team 1 ("COOP") with the
    host, by the host. GetHost stays the host: only the host sets off story scenes.
  - Same class only: the lobby refuses another class (the Access settings' classes refused; the
    game list shows them refused); noxsdl names the campaign's class in its "couldn't join" line.
  - Where a script gives a builtin the host ("the player"): Frozen holds every player, MoveObject
    moves the party (the others at clear points about the host), JournalEntry / Edit / Delete
    are every player's, HasItem is true when any player holds the thing, AwardSpell is every
    player's. On anyone else, and with no guest in, every builtin is the game's.
  - A guest's IsCaller(GetHost()) is false as the game's, and the guest is told "Only the host can
    set this off." (at most once in 10 s for the same trigger). Anyone talks to an NPC at any
    time, as in the game's quest (two at once included), but a guest's click on one whose talk can
    play a scene (MSG_DIALOG 1: its start or end function, or a function either calls or hands a
    builtin such as a timer, calls WideScreen, Blind, ImmediateBlind, StartupScreen or EndGame) is
    refused with "Only the host can start this talk: it can play a story scene." A talk's functions
    serve every stage of it, so such an NPC is the host's at every stage (148 of the campaign's 779
    talks: the story's people and the quest-givers). Each guest is told the rule as it comes in.
  - As the host's scene begins (its bars up), every other living player more than 120 from the
    host is put at a clear point within 50 of it.
  - Kill XP to the killer and gold to whoever takes it, as the game; a script's experience and gold
    for the host (GiveXp, ChangeGold's gain) are every player's in full, a charge (ChangeGold
    below 0) the host's alone.
  - The party's size scales the game: each player past the first adds 0.6 of each monster's and
    generator's own health (a whole one stays whole, a hurt one keeps its share; summoned and
    owned ones aside)
    and 0.15 to the damage players take (the quest's way of it); both go back as the party
    shrinks. Alone: the solo game's numbers. Saves hold the monsters' own numbers (the party's
    scale is put back after the save).
  - One key ring: a door locked with a key takes any party member's key of its kind (used up as
    the toucher's would be); a leaving player's keys go to the host; a chapter's start takes
    every player's keys and journal entries of its kinds, as the game does the host's.
  - A spell book, ability book or field guide used up as it's picked up is put back where it lay
    (the same book) while another of the party hasn't had it yet (a spell book: is below the level
    the taker reached); a put-back spell book can't be taken by one already at that level.
  - A book, ability book or field guide a script hands the host (Pickup(GetHost(), book): Wiz01A's
    Lesser Heal from Horvath, Con02A's Charm and spider guide, the teachers' Inversion, ...; in a
    guest's own talk, the talker) is every player's: each other player below the taker's level of
    that spell now (an ability or creature it doesn't know) gets the same award as its own use of
    the same book (the award's sound, text and book flight on its client, stock or NoX Core), and
    no copy is put back on the floor. Any other thing a script hands the host that way (gold,
    gear, potions, food) is every player's too: each other player picks up its own of the same
    (its modifiers, charges and amount) as the host's pickup goes (gold's amount and line, a potion
    drunk when it doesn't overfill, the class's gear), once each; one too heavy to carry ("carrying
    too much") isn't left anywhere. The host's own full-pack rule (its client's grid: the cheapest
    thing dropped) stays the host's, as a guest's grid isn't the server's to see. Keys (the party's
    ring shares them), War01A's sponsorship letter and the Oblivion staves stay the host's (or
    the talker's).
  - A guest brings its own character: a NoX Core guest's client sends its file's server sections
    when the host greets it (as a quest client's at its join), loaded onto it as a quest's are.
    A character no solo game saved (no GameData of its own: a network character, or a stock
    guest's quest character) comes in with a new solo character's book (no spells, abilities or
    creatures known; not a quest's first spells either), and a NoX Core guest's spell bars start
    empty with it; the file on disk isn't written. One a campaign saved (its "(Co-op).plr", or a
    solo-campaign character) keeps its own, as does a returning player's campaign character.
  - Its saves are the co-op campaigns' own (Save\Coop under the solo saves' root, the same
    layout), never the solo game's. Each save keeps the party beside the game: a Party folder in
    WORKING (copied to the slot with the rest) with each guest's character (the server's sections,
    as a quest player's) and party.txt (who, which map and where, the creatures each owns). A
    guest leaving has its character kept there at once.
  - A player the campaign knows (by account and name) gets its campaign character back as it
    joins (its own file isn't asked for), and every guest gets its own after the host loads a
    saved game (where it stood, its creatures its own again). A new player below the party's
    lowest level is given the experience to it (catch-up, a level at a time), and every new player
    is given the party's book: each spell, creature and ability another player knows, at the best
    level any has it (told without sound or flight, as a character's load tells them: its book
    has them, its spell bars stay as they were). A party that starts together has nothing in it.
  - At each save a NoX Core guest is sent its campaign character and keeps it beside the one it
    joined with, as "<name> (Co-op).plr" (a network character's file); the one it joined with is
    never written (unless it was that co-op file itself). A guest's character is kept only once
    it's its own (given back, or its file in) and of the campaign's class; one down is kept on its
    feet.
  - The host's own client's calls (the bars, fades, black, the map intro, a line's voice, the
    ending) reach NoX Core guests too (MSG_NOXCPP_COOP kCine, with the host as the scene's actor:
    a guest's view follows it while the bars are up); stock guests get the fades and black as
    MSG_FADE_BEGIN and the music as the network game's MSG 0xE5 - 0xE7, no bars.
  - A chapter screen (StartupScreen / DeathScreen) is every player's: NoX Core guests are shown it
    and the world stays paused till each is done reading (the host's own screen's end held), or the
    host types "?skip", or a minute and a half goes by; a stock guest gets MSG_CHAPTER_END itself
    (its own class's chapter: the campaign's) and isn't waited for. Once the host and the NoX Core
    guests are done the world goes on, a slow stock guest's unit standing in play behind its screen;
    a DeathScreen gives a stock guest only its fade to black, till the host's load's map start.
    A NoX Core guest's DeathScreen fades back to the game (no load menu: the host loads).
  - With a guest in, a level-up, an award's flight and the host's menus don't pause the world (a
    level-up's moment ends a second on, its player free), and their unpauses end no other pause; a
    map's load, a fade's and a chapter screen's pauses stand. The host alone pauses as the game.
    The world running meanwhile, a second level-up within another's moment (or during the host's
    award flight) gets no LevelUp effect, sound or text (its level is given).
  - The party gathers at an exit: the first player on it holds it and everyone is told; it takes the
    party when every living player stands on it, 30 s after the first came (told at 10 s left), or
    at the host's "?go"; nobody on it for two seconds gives it up. Exits that lead to the same map
    are one gathering; one to another map waits till it's over. Alone, or the only one alive: as
    the game. An exit the host lands on by a script's MoveObject (a scene's way on, often under
    Blind and out of the map: Wiz03A's after Horvath's talk, the SEG2 switches, MapSwitch, ...)
    takes the party at once, as the solo game's host goes.
  - Every player is told each other player's health as a quest's server tells its players
    (MSG_TEAM_MSG 0x0C, its share, whenever it changes): stock guests (under the quest's flags) and
    NoX Core clients draw the bar over each other player, as the quest does, while their own
    bars are up.
  - The friend dot (the small dot in our team's colour over a creature on our friends list) is
    drawn as in the solo game: never. Team 1 is the party's here, and the friends list holds the
    story's NPCs, the fish and the party's summons alike, so noxsdl draws no dot in a campaign
    game (game flag 0x800), host or guest; outside the campaign the dot is unchanged. A stock
    guest, told the quest's flags, draws its quest's dot (every friend but a shopkeeper).
  - Only a party wipe loses. A player who falls while another lives keeps what it holds (nothing
    dropped), the pending save stands, the map's PlayerDeath functions don't run, and it's back on
    its feet 10 s on (the game's respawn, its things kept) by the party's checkpoint: where the host
    stood as the map first ran; what it wears is told to every client again (MSG_PLAYER_DIED took
    it off). When the last one falls the death is the game's (the save off, the map's PlayerDeath
    functions once: DeathScreen and the host's load menu, even if that player leaves in its dying
    second) but nothing is dropped; the guests are told to wait for the host's load, and nobody
    comes back meanwhile. Players down as the party leaves a map are back at the next one's
    checkpoint.
  - A guest loads each map from its own copy of the map's file (never a visited map's saved copy,
    which is the host's); what that file draws and the world no longer holds is hidden on the
    guest's screen as it comes in (MSG_DESTROY_OBJECT with the map number's high bit), the file's
    breakable walls the copy left out are broken on it, and every MSG_DESTROY_WALL to a guest
    names the wall by its file's id (the copy numbers them without those left out).
  - A guest's character comes without its game: its file's GameData (quest variables, magic walls,
    the briefing, the saved map) isn't applied; the world's stay the host's.
  - A player's talk (the host's or a guest's) is shown to the others: a NoX Core player gets each
    line (kDialog, in its console), a stock one a line "<talker> is talking to <NPC>." as it opens.
    The host's talk in a scene (under its bars, or a talk that can play one) opens the game's own
    dialog window for every other player too (the host's MSG_DIALOG 3: its text, voice, portrait
    and buttons), closed as the host's closes (MSG_DIALOG 4); their answers do nothing (no talk of
    theirs), only the host's go on with it.
  - A guest coming in during a scene gets its bars and is held as the party is.
  - Stock (GAME.EXE) guests: the game is listed as a quest (a stock player brings a quest
    character) and a stock guest is told the quest's network flags (0x1000, 0x2000, 0x10000,
    0x20000) in place of the world's solo mode (no solo menu, no saves of its own); its character
    is read and written as a quest's: a quest character comes in (its level then by the
    campaign's rules, its experience), and each save sends it back as the quest's save flow does
    (MSG_GAUNTLET 0x1C {1}, SAVEDATA, Attrib mode 4) with only what a quest's read takes (no food
    but potions, nothing off the quest's tables: the whole character stays in the campaign's
    Party folder), its Quit's save request answered the same way. A returning stock guest gets
    the campaign's character, not the file it comes with. Its party loss is MSG_GAUNTLET 0x0D's lock screen with the
    chapter's loss picture (never MSG_CHAPTER_END's loss kind: its own load menu would hold it),
    the ending the stock credits (MSG_CHAPTER_END {0xFF, 1}). It has no bars, map intro, chat
    voices or ending movie (no stock message exists), and its screen shows the quest's ankh and
    stage. NoX Core guests are told the world's flags and keep network characters.
  - NoX Core guests get MSG_NOXCPP_COOP (0xF7: the greeting, the party, the notices), framed as
    0xF6; stock guests get the notices as the server's lines (MSG_TEXT_MESSAGE from no one: in
    the middle of the screen for a few seconds, and in the console).
- **Revert:** don't host one (every hook the core gives a mode is a no-op without one).

### VD-42: A mode's dead players keep their things (World::keepThingsOnDeath)

- **Vanilla:** outside a quest a dead player's things go on the ground round it (Plr_Die's
  dropAllItems, all but its street clothes), every client forgets what it wore (MSG_PLAYER_DIED),
  and its respawn strips it and gives the default outfit (playerMakeDefItems, MSG_PLAYER_RESPAWN
  flag 1). A quest's dead player keeps everything and respawns with flag 0.
- **Ours (only a mode that asks: GameHost::Options::keepThingsOnDeath, a script's configure key
  `keepThingsOnDeath = true`):** outside a quest a dead player keeps its things as a quest's
  does. Only a carried flag or crown still drops, on the ground beside it as before. After
  MSG_PLAYER_DIED its worn weapons and armour, all but the street clothes, are reported to
  everyone again (MSG_REPORT_*_EQUIP, as to a joining player), so every client draws it as it
  stands; a worn thing that leaves a dead player's inventory is told to everyone, as in a quest.
  Its respawn sends MSG_PLAYER_RESPAWN flag 0 and gives nothing. In a quest it does nothing. Gold
  is untouched either way (Plr_Die never takes it). A match's end still drops and re-dresses
  everyone. So does a map change outside a quest (everyone's outfit made anew, as always).
- **Revert:** don't set it (off in every game unless a mode turns it on).

### VD-43: A mode's book (World::modeOwnsBook)

- **Vanilla:** outside a quest a network game's player knows everything from its join
  (unitInitPlayer's spellAwardAll1 / 2 / 3): every spell at 3, every creature in its field guide,
  every ability usable whatever its level (Abil_TryExecute's network-game test), and a warrior's
  values read again give all five at 5. Reward books, field guides and spell pedestals add to it,
  and a summoning doesn't ask for the field guide (Spell_SummonStart asks only in a quest or
  co-op). The client's book lists every spell, ability and creature (guiSpellSortList's same
  test). A spell not in the book, cast from the bar or the book, is refused without a word.
- **Ours (only a mode that asks: GameHost::Options::modeOwnsBook, a script's configure key
  `bookIsTheMod = true`):** outside a quest the book is the mode's. A player joins with no
  spells, abilities or field guide creatures and has only what the mode grants (its spells,
  abilities and field guide creatures: nox.grantSpell, nox.raiseSpell, nox.setAbility,
  nox.grantCreature).
  - An ability whose level is 0 is refused as in a quest (MSG_INFORM 2, 3 and the "can't"
    sound); reading a warrior's values gives none.
  - Which casts are taken is as before (they always asked for the spell's level), but a player's
    spell is cast at its level in a versus game too (spellGetPower's 3 for every spell there is
    the stock book's). A spell not granted, cast from the bar or the book, gets the line a spell
    spoken by hand that isn't known gets ("plyrspel.c:SpellUnknown", no sound).
  - Reward books and field guides are refused (the refusal sound, the book kept) and a pedestal
    grants nothing; a summoning wants the creature in the field guide, as a quest's. A map's own
    scripts still grant spells. Wands, staves and other items that cast are as before.
  - NoX Core clients of level 2 that named ModeUi are told (MSG_NOXCPP_MODEUI's kBook) and list
    only what was granted. Stock clients, older NoX Core clients and the bots are sent nothing
    and still list everything, but can use only what was granted (bots: a known limit; the bot
    manager never hosts a mode with the key).
  - In a quest (or a quest game before its quest begins) or a solo game it does nothing.
- **Revert:** don't set it (off in every game unless a mode turns it on).

## Menus: Westwood Online (no effect on the wire)

### VD-25: The WOL connection is let go for a game, and made again after it

- **Vanilla:** WOLAPI's one session lasts from the login through the chat, the game list and the
  game (a host's game is its channel on that session; a guest stays logged in too), till the
  player logs off.
- **Ours:** noxsdl logs off (QUIT, then waits up to 3 seconds for XWIS to close) before it joins
  or hosts a game (Menus::join), since a hosted game posts itself on a connection of its own with
  the same nick (lobby/xwis_host) and the menus aren't run during a game to answer XWIS's pings.
  Back from a game out of the chat, it logs in again (Menus::showGameBrowser) and the game list's
  Back is the chat's again.
- **Revert:** keep `wol_` through the game, pumped from the game loop, and give XwisHost the
  session instead of its own login.

### VD-26: The chat's Clans and Ladder buttons are off

- **Vanilla:** the chat window's buttons 0x786 / 0x787 open http://battleclans.westwood.com/nox/index_%d.html and
  .../westwoodonline/tournaments/nox/rankindex_%d.html (%d: the SKU & 0xF) in the browser.
- **Ours:** off and marked " - WIP" (menus_wolchat.cpp): Westwood's sites are long gone.
- **Revert:** SDL_OpenURL those addresses on the two clicks.

### VD-27: The welcome's message of the day shows as the chat opens

- **Vanilla:** the text WOLAPI collects from the welcome reaches the game's WOL event handler,
  which keeps and shows it only in state 7 (the chat); the chat's set-up shows what it kept each
  time the chat opens.
  Whether the login's welcome arrives while the state is already 7 wasn't settled from the code.
- **Ours:** kept at the login (Menus::chatEvent's Motd) and shown each time the chat opens.
- **Revert:** drop the wolMotdLines loop in Menus::setupChat.

### VD-38: The game list: servers.txt in both lists, the LAN in Westwood Online's, Skip Login

- **Vanilla:** the game list's refresh (refreshServerList) asks one place, by mode. Network (LAN
  play) broadcasts the discovery query on the game port and lists the games that answer, nothing
  else: no internet games and no join by address (the screen's Find is Westwood Online's "Find
  User in a Game"); with none, the list is empty under "Join a server by clicking on its icon.".
  Westwood Online asks the WOL connection for the Nox game channels alone. The login screen's
  Register makes a new account.
- **Ours:** Network is LAN play as in the original: XWIS is never asked
  (ServerBrowser::setXwisListing(false), Menus::refreshGames), and a game hosted from Network is
  not posted to XWIS, even after an earlier login. Beside that (noxsdl/src/browser_screen.cpp,
  noxcore/src/lobby/browser.cpp):
  - Both lists also ping the servers named in servers.txt (beside the program or in the game
    folder) or with --server, marked "list" in the From column; in Network's list they come
    after the LAN's games.
  - Westwood Online's list also has the LAN's games, beside XWIS's.
  - Register is Skip Login (Menus::onLogin): Westwood Online's list without a login, XWIS
    listed through a throwaway listing nick.
  - --browse opens Network's list.
- **Revert:** Options::servers left empty (no servers.txt); in Westwood Online's list,
  Options::lan off; Register's own screen back in place of Skip Login.

## Server: memory safety

### VD-28: The map grid's point cell is clamped to the grid

- **Vanilla:** the grid's link puts an object's point node in its NewPos's cell unclamped (a
  coordinate below 0 its own value, original_bugs.md, OB-195): out of the grid's arrays.
- **Ours:** clamped to the grid (MapGrid::link), so nothing is written past it.
- **Revert:** none wanted (memory safety); no stock map places anything there.

### VD-29: A deleted thing leaves the object list at the frame's sweep (the grid a frame later)

- **Vanilla:** delayedDeleteObject marks it (0x20); at the deleting tick's end the delete sweep
  takes it off the updatable list, and at the next tick's end the free pass frees it (its slaves
  passed on, its owner let go, its things dropped, servFinalizeDelObject: off the grid, the
  object and missiles' lists, MSG_DESTROY_OBJECT to the clients).
- **Ours:** World::sweepDeleted at the deleting frame's end takes it off
  the updatable list and out of the objects (World::dying_), but leaves it linked in the grid and
  the missiles' list till the next sweep, its owner field as it was: the next frame's searches find
  it as the exe's do (several callbacks have no 0x20 test). What still differs meanwhile: a
  walk of every object (ours' objects) doesn't meet it; its owner's lists and its slaves are let go
  of at the first sweep, not the second; the clients are told it's gone the frame it's deleted,
  not the next.
- **Revert:** keep the swept ones in the objects till the next sweep, each walk of them taught to
  skip 0x20 where the exe's does; let go of the owner and tell the clients at the second sweep.

### VD-30: The ray walk ends when a band repeats

- **Vanilla:** mapTraceRay heading below 0 from column or row 0 walks the same band for ever
  (modf chopping -0.1 / 23 to -0: original_bugs.md, OB-198): the game hangs.
- **Ours:** World::traceRay ends the walk, the ray clear, when a band's cell is the last one's
  again (only that case: every other band moves a cell on); the cells left are off the map.
- **Revert:** none wanted (a hang is no behaviour to keep); no stock map gets there.

### VD-31: A waypoint outside the map grid is filed in no cell

- **Vanilla:** waypointMapRegister files it past the grid's arrays (original_bugs.md, OB-199);
  the nearest-waypoint search never reaches it.
- **Ours:** MapGrid::registerWaypoint files it nowhere: the same searches, no memory written.
- **Revert:** none wanted (memory safety).

### VD-37: "Exact Original Maths" switched off: the server's floats rounded to nearest

- **Vanilla:** the game runs its FPU in chop mode (rounding towards zero) at 53 bits for all its
  figuring: every float result is the exact one cut towards zero.
- **Ours:** the same, by default: the server's figuring runs with the processor's own rounding
  set towards zero for its thread while it works (util::RoundingScope,
  `noxcore/src/util/rounding.cpp`), and the figures keep the game's own order of
  operations, stores and constants. The host setting Exact Original Maths (Options, More; saved
  as ExactMaths in noxcore.cfg; the dedicated server's `--nearest-maths` turns it off) applies to games this
  program hosts and to solo, quest and map editor sessions, not to games joined. Switched off,
  the same code rounds to nearest: damage after armour, potion healing, carry capacity,
  experience and wear can then come out a point different now and then, and positions, missile
  paths and sight edges differ in their last bits.
- **Revert:** switch it back on (the default).

## Map scripts

### VD-32: Object variables run (a NoX Core-only feature, off in the tools unless enabled)

- **Vanilla:** a compiled script's function 0 lays out "object variables", words each object is
  meant to carry for the script, and the interpreter reads and writes them through the trigger
  object's script-variable pointer; nothing ever gives an object that storage, so a script that
  touches one crashes the game (original_bugs.md, OB-210). No stock map has one.
- **Ours:** the server's script host keeps each object's words as the interpreter's addressing
  implies (ScriptHost::objectVar / setObjectVar in noxcore/src/server/script_host.cpp): 0 until
  written, gone with the object, never saved; with no trigger (timers, entry points, polygons)
  reads give 0 and stores are lost. A map that uses them therefore needs a NoX Core server.
- **In the tools:** the compiler (`int object.hp;`, `object.hp`) refuses them unless
  `CompileOptions::allowObjectVariables` is set (noxcore/include/noxcore/script/compiler.hpp),
  with an error saying why and how to enable them; the map editor sets it only from its Editor
  Settings option "Allow NoX Core-only script features (object variables)", default off. With the
  option off, a map whose script already has them is shown read-only and its script kept as it is.
  The decompiler always reads them. A map saved with them is marked as needing NoX Core in the
  editor's `.nxe` sidecar (never in the .map, which stays readable by the original game) and the
  map validator warns that it needs a NoX Core server.
- **Revert:** have ScriptHost::objectVar / setObjectVar give no storage (the original game
  crashes there instead, so there is no faithful behaviour to restore); leave the compiler option
  off.

### VD-33: A host's goodbye and a client's leave wait in the background

- **Vanilla:** a host quitting with other players sends them MSG_SERVER_QUIT and sits in a modal
  "Please Wait... / Shutting down server..." box till they have all answered (original_bugs.md,
  OB-230: no limit), then ends its session. A client leaving waits for its OUTGOING_CLIENT's ack in
  a loop of 50 ms sleeps, up to 20 s, the window neither drawn nor taking input meanwhile (the
  whole 20 s when the server has gone).
- **Ours:** noxsdl's host goes back to the menus at once; the game it left keeps running on its own
  thread till every player has acked or 5 s have passed (2 s more at most when the program quits),
  then closes every stream with SERVER_CLOSE (LocalHost::retire, noxsdl/src/local_host.cpp). The
  client keeps its 20 s wait for the ack but its window goes on drawing and taking input.
- **Ours, the leaving notice:** while the client waits for that ack, noxsdl says so in a small
  window drawn like the lag window (discon.wnd's, its buttons left off): "Leaving the game: the
  server isn't answering. Waiting up to 20 seconds for it to confirm..." with the seconds left
  counting down (noxsdl/src/leaving_notice.hpp, DisconWindow::setNotice; the wait so far from
  HeadlessClient::quitWaitedMs). After the lag window's Exit it shows at once; after an ordinary
  leave only once the wait has lasted a second, so a quick leave shows nothing. The wait itself
  and what follows it (the game list, with the Timeout notice after Exit) are as before.
- **Why:** a frozen window on leave is what the player reported as the game hanging; the clients
  see the same messages, in the same order, either way. The exe's window stands frozen through
  the client's wait, saying nothing; ours keeps drawing, so it tells the player why nothing seems
  to happen for up to 20 seconds.
- **Revert:** destroy the host at once in LocalHost::retire after beginQuit (the players then get
  only SERVER_CLOSE, as if every ack were in). For the notice: pass nothing to
  DisconWindow::setNotice in main.cpp's frame.

## Server: hangs

### VD-36: The latency report gives up when no player can be picked

- **Vanilla:** the server's latency report (SrvNet_BroadcastNextPlayerLatency: MSG_REPORT_ALL_LATENCY
  every 500 ms) walks the players round from the last one reported for the next that is the host's
  own player or has a latency over 0. With no host's player in the game (a dedicated server) and
  every latency 0 (as each is till its first five round trips are in) the walk never ends
  (original_bugs.md, OB-226): the server hangs.
- **Ours:** GameHost::reportNextLatency (noxcore/src/server/game_host.cpp) walks the players once
  round and, finding nobody, reports nothing that time; whenever there is someone to report it
  picks the same player the exe's walk does, so pings work as they do on a stock server.
- **Why:** a hung server is no behaviour to keep, and keeping ours was approved. Clients see the
  same reports whenever the exe's search would have ended.
- **Revert:** none wanted (it would hang a dedicated server); to match the exe exactly, loop the
  search until it finds a player.

### VD-40: Text messages are cut at 254 characters

- **Vanilla:** the server's text message (MSG_TEXT_MESSAGE: scripts' Print, PrintToAll and Chat,
  the server's lines, a unit's speech) carries its length with the end in one byte, and the
  clients copy the text into a fixed stack buffer: 255 characters wrap the length and every client
  misreads the rest of the packet; 256 or more crash them (original_bugs.md, OB-342). A host's
  content pack (strings.patch) can make such a text.
- **Ours:** World::textMessage (noxcore/src/server/world.cpp) sends at most the first 254
  characters, so the length byte is at most 255; shorter texts are sent byte for byte as before.
- **Why:** a crash or a misread packet is no behaviour to keep, and cutting it was approved.
- **Revert:** none wanted (a stock client would crash).

### VD-41: A map script's call is stopped after 1,000,000 instructions (a frame's after 2,000,000) or past a nesting depth

- **Vanilla:** the script interpreter runs a function until it returns, however long that takes:
  a map script that loops for ever hangs the server, and every player in the game with it
  (original_bugs.md, OB-400). A custom map carries its own script, so loading a broken or hostile
  one is enough.
- **Ours:** the interpreter counts the instructions it runs (script::Vm in
  `noxcore/src/script/vm.cpp`). One top-level call (a timer, a handler, an entry point such as
  MapInitialize, a dialog's function), with the script functions it calls and the handlers its
  builtins set off while it runs, may run 1,000,000 (Vm::kMaxSteps). All the calls of one server
  frame may run 2,000,000 between them (Vm::kMaxFrameSteps, started afresh at each frame by
  ScriptHost::newFrame). A call that reaches either is stopped at that instruction: it and every
  script call it was in end there, what they had already done stays done, and the server goes on
  with its frame. Once a frame's budget is spent, that frame's later calls stop before their first
  instruction; the next frame has the whole budget again. Each stop is logged (the dedicated
  server's stderr, noxcore.exe's log) once per function and reason on a map, not once a call or a
  frame (ScriptHost::stopLog, setScriptLog). Instructions are counted, never timed, so a stop
  falls on the same instruction on every machine and in every replay. A stopped call is never
  resumed, so the script's value stack is emptied at the stop: the words it left would otherwise
  hold up the map's later handlers, which wait for an empty stack.
- **Why:** a hung server is no behaviour to keep, and the cap was a decision. Measured over the
  stock maps (every map's start and first ten seconds, the solo campaign chains, the quest and
  co-op runs), the largest single call runs 2,057 instructions (G_Mines's MapInitialize) and the
  busiest frame 2,073: the budgets are about 500 and 1,000 times those, so no stock map comes
  near them and every script under them runs exactly as before. A release build runs about 1.2 to
  9 nanoseconds an instruction (more when each turn calls a costly builtin): a looping call is
  stopped within a few milliseconds, and a map that loops every frame costs its server at most
  about 2 to 20 ms of script a frame. A handler set off inside a running call counts towards that
  call's budget, so a loop that sets one off each turn runs out too.
- **Also, the depth:** where the original nests without limit until it runs out of stack and
  crashes (OB-401), a call is stopped the same way when its script functions nest more than 256
  deep (a function that calls itself without end; Vm::kMaxDepth), or the handlers its builtins
  set off while it runs nest more than 32 deep (a generator's onHurt that damages its own
  generator; Vm::kMaxNested). A handler goes on from the depth it was set off at, so both count
  across the whole call. Each script call takes about 256 bytes of the server's stack in a
  release build and each nested handler about 2 KB: at both limits about 128 KB, of the 1 MB a
  server thread has. Stock maps nest at most 3 script calls and 2 handlers.
- **Revert:** raise Vm::kMaxSteps and Vm::kMaxFrameSteps in
  `noxcore/include/noxcore/script/vm.hpp`. No value matches the original exactly: it has no limit
  and hangs. The depth limits (Vm::kMaxDepth, kMaxNested) are best kept: without them a map can
  crash the server.
