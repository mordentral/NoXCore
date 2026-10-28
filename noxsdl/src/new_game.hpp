// A client's state of one game only, put back as a fresh join finds it at every join (main.cpp's
// startLive: the menus' Join and Host, a solo game, each play-test in the map editor, an edit
// session's game), and what's held let go whenever a game takes the keyboard and mouse.
//
// Nothing of the last game's may reach the next one: the scoreboard and its game-over banner came
// up at the join after a game had ended (the World's game-over count starts again at 0, ours
// didn't), and the kill feed's lines, a spell armed for a target, an award still flying to the
// bars, the trap bar's slide, inverted spell targets and the NPCs' picks were the last game's.
// Templates over main.cpp's App, so the tests check them on a stand-in without SDL.
#pragma once

namespace noxsdl {

/// The game has the keyboard and mouse now (a play-test started or clicked into, the session's
/// Play, F2 back to a full-window test): no key or button counts as held (a press the editor or the
/// menus took never reaches the game as one), and nothing aimed or dragged waits for its release.
template <class App>
void gameTakesInput(App& app)
{
    if (app.input) {
        app.input->releaseAll();
    }
    // (A walk under way stops by itself: the next frame sees no move held and tells the server.)
    app.armed.reset();
    app.barDrag.reset();
}

/// A new game's client state, as the first join after start-up has it (the App's own defaults).
template <class App>
void freshGameState(App& app)
{
    gameTakesInput(app);
    app.running = false; // (the new game's client has never been told of a move)
    // The scoreboard opens when the World's game-over count moves; a new World's is 0.
    app.gameOversSeen = 0;
    app.gameWasOver = false;
    app.banner.reset();
    app.kills.clear();
    app.flight.reset();
    app.flightFrame = 0;
    app.npcPicks.clear();
    app.animTracks.clear();
    app.boulderRolls.clear();
    app.anchors.clear();
    app.centred.clear();
    app.muted.clear();
    app.hovered.reset();
    app.hoveredUseable.reset();
    app.cursorHovered.reset();
    app.cursorUseable.reset();
    app.pickupTarget.reset();
    app.pickupSentMs = 0;
    app.lastSlot = -1;
    app.lastSlotFrame = 0;
    app.barFlash = -1;
    app.barFlashMs = 0;
    app.barsInverted = false;
    app.trapOpen = false;
    app.trapArt = false;
    app.trapSlide = 0;
    app.trapSlideDir = 0;
    app.trapSlideFrame = 0;
    app.trapHitMs = 0;
    app.lastTargetFlag = false;
    app.inGameSinceMs = 0;
}

} // namespace noxsdl
