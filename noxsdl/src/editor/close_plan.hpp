// What closing the map editor does (File > Close Editor, the window's close button), by where it
// stands: alone, with a play-test's game running, in our own edit session or in someone else's.
// main.cpp's editorFrame / closeEditor carries it out; no SDL here, so the tests check it.
//
// - Alone: back to the main menu (or the program ends: --map-editor, or no menus to go back to).
// - A play-test running (its own host, in the Map panel or over the window): its game ended first,
//   then as alone. Never its game left running with no editor round it.
// - Hosting a session (Host Custom > Map Editor, File > Host a Session, --host-edit): asks once (it
//   ends the session for everyone), then the session ends properly (our players told:
//   GameHost::beginQuit's MSG_SERVER_QUIT, its host retired in the background) and the main menu,
//   in one step. The window's close button asks the same before the program ends.
// - In someone else's session: the editor closes and the session's game is in front (F2 opens the
//   editor again); leaving the session is the game's own Quit menu, as ever.
#pragma once

namespace noxsdl::editor {

/// Where the editor stands as it's asked to close (main.cpp's App, reduced to what decides it).
struct CloseSituation {
    bool direct = false;         ///< --map-editor: the editor alone, its close ends the program
    bool menus = false;          ///< the menus are there to go back to
    bool hostingSession = false; ///< our own edit session
    bool joinedSession = false;  ///< someone else's session (its game ours to go back to)
    bool playTest = false;       ///< a play-test's game (its own host) running
};

/// Where the window is once the editor has gone.
enum class AfterClose {
    MainMenu,    ///< the menus' main menu
    Quit,        ///< the program ends
    SessionGame, ///< the joined session's game, over the whole window
};

struct ClosePlan {
    bool confirm = false;     ///< asks first ("End the session?"): it ends the game for everyone in it
    bool endPlayTest = false; ///< the play-test's game and host ended
    bool endSession = false;  ///< our session ended: its players told, its host retired in the background
    AfterClose next = AfterClose::MainMenu;
};

constexpr ClosePlan closePlan(const CloseSituation& s)
{
    ClosePlan p;
    const AfterClose out = s.direct || !s.menus ? AfterClose::Quit : AfterClose::MainMenu;
    if (s.hostingSession) {
        p.confirm = true;
        p.endSession = true;
        p.next = out;
    } else if (s.joinedSession) {
        p.next = AfterClose::SessionGame;
    } else {
        p.endPlayTest = s.playTest;
        p.next = out;
    }
    return p;
}

/// The window's close button while the editor is open: whether it asks first (our session ends for
/// everyone with the program). A test's hidden run never asks.
constexpr bool quitNeedsConfirm(const CloseSituation& s, bool hiddenRun)
{
    return s.hostingSession && !hiddenRun;
}

constexpr const char* afterCloseName(AfterClose a)
{
    switch (a) {
    case AfterClose::MainMenu: return "the main menu";
    case AfterClose::Quit: return "quit";
    case AfterClose::SessionGame: return "the session's game";
    }
    return "?";
}

} // namespace noxsdl::editor
