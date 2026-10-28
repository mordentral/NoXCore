// The window's mouse as each owner of it wants it, and where the drawn pointer comes from. The
// menus and a game run SDL's relative mode with SDL's pointer hidden (they draw their own, from the
// mouse's place: main.cpp's mousePos); the map editor has the system's pointer; a game in the
// editor's Map panel has the mouse kept on its area while it has the input, and, when the editor has
// it, its drawn pointer held where it was left. Every way out of the editor or a play view puts the
// menus' (or a game's) back whole (main.cpp's applyWindowMouse): nothing of the panel's left behind.
// No SDL here, so the tests check it.
#pragma once

namespace noxsdl {

/// Who has the window's mouse.
enum class MouseOwner {
    Menus,              ///< the menus (and the Options window over them)
    Game,               ///< a game over the whole window (a full-window play-test, a session's game)
    Editor,             ///< the map editor's panels
    GameInPanel,        ///< a game in the editor's Map panel, with the input
    GameInPanelReleased ///< a game in the editor's Map panel, the input the editor's
};

/// The window's mouse state for an owner.
struct WindowMouse {
    bool relative = false;      ///< SDL relative mode (raw counts; on in the menus and games, off in a hidden run)
    bool systemPointer = false; ///< SDL's pointer shown (else the game's own is drawn)
    bool confined = false;      ///< kept on a rectangle (SDL_SetWindowMouseRect: the panel's game area)
    bool grabbed = false;       ///< the window's mouse grab
    bool heldPointer = false;   ///< the drawn pointer held where a play view left it (not the mouse's place)
};

/// `hiddenRun`: a test's --screenshot run, whose hidden window never takes the desktop's mouse.
constexpr WindowMouse windowMouse(MouseOwner owner, bool hiddenRun)
{
    WindowMouse m;
    switch (owner) {
    case MouseOwner::Menus:
    case MouseOwner::Game:
        m.relative = !hiddenRun;
        break;
    case MouseOwner::Editor:
        m.systemPointer = true;
        break;
    case MouseOwner::GameInPanel:
        m.relative = !hiddenRun;
        m.confined = !hiddenRun;
        break;
    case MouseOwner::GameInPanelReleased:
        m.systemPointer = true;
        m.heldPointer = true;
        break;
    }
    return m;
}

/// Where the drawn pointer's place comes from.
enum class PointerSource {
    Test,   ///< a test's --mouse / --mouse-script point
    Held,   ///< where the panel's game had it (the editor has the input)
    Mouse,  ///< the mouse's own place (SDL_GetMouseState)
};

/// `testPoint`: a test's point is set; `held`: a held place is kept; `heldShown`: a game is in the
/// editor's Map panel without the input now (the only time a held place is drawn: anywhere else a
/// stale one froze the pointer while the buttons still lit under the real mouse).
constexpr PointerSource pointerSource(bool testPoint, bool held, bool heldShown)
{
    if (testPoint) {
        return PointerSource::Test;
    }
    if (held && heldShown) {
        return PointerSource::Held;
    }
    return PointerSource::Mouse;
}

constexpr const char* pointerSourceName(PointerSource s)
{
    switch (s) {
    case PointerSource::Test: return "test point";
    case PointerSource::Held: return "held";
    case PointerSource::Mouse: return "the mouse";
    }
    return "?";
}

} // namespace noxsdl
