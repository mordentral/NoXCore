// The window's mouse as each owner of it wants it, and where the drawn pointer comes from. The
// menus and a game hide SDL's pointer (they draw their own, from the mouse's place: app.cpp's
// mousePos) and take SDL's relative mode as wantRelative says (applyMouse): always in a game being
// played and in fullscreen, never in a windowed window's menus, whose pointer can leave the window;
// the map editor has the system's pointer; a game in the editor's Map panel has the mouse kept on
// its area while it has the input, and, when the editor has it, its drawn pointer held where it was
// left. Every way out of the editor or a play view puts the menus' (or a game's) back whole
// (app_settings.cpp's handMouseTo): nothing of the panel's left behind.
// No SDL here, so the tests check it.
#pragma once

#include <optional>

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
    bool relative = false;      ///< SDL relative mode held on (raw counts: the panel's game with the input; never in a hidden run)
    bool byPlay = false;        ///< relative mode or not as applyMouse's wantRelative says (the menus, a game over the window)
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
        m.byPlay = true; // (wantRelative: a hidden run never relative)
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

/// The menus' and a game's mode (applyMouse): relative (raw counts, a count a game pixel, the
/// pointer kept in the window) or absolute (the desktop's pointer at the desktop's speed, free to
/// leave the window, SDL showing the desktop's own outside it). `playing`: app.cpp's beingPlayed (a
/// game, no Escape menu, Options, key bindings, briefing or movie over it); `windowed`: Options' Display
/// is Windowed; `pointerInWindow`: the pointer is over the window (SDL_GetMouseFocus); `hiddenRun`:
/// a --screenshot run, whose hidden window never takes the desktop's mouse.
/// Fullscreen and fullscreen windowed are relative throughout, the menus too (as before: no drift
/// onto another monitor, no click outside minimising an exclusive fullscreen). Windowed, the menus
/// are absolute, and a game being played is relative only with the pointer over the window: back
/// from the menus with it outside (Escape closed by its key), it's taken when it comes back over
/// the window, so it never jumps and no click is lost. Once relative, SDL keeps the mouse's focus
/// on the window while it has the keyboard's, and ends relative mode itself when that goes.
constexpr bool wantRelative(bool playing, bool windowed, bool pointerInWindow, bool hiddenRun)
{
    if (hiddenRun) {
        return false;
    }
    if (!windowed) {
        return true;
    }
    return playing && pointerInWindow;
}

/// applyMouse's mode as last set on the window; Unknown where something else may have changed it
/// (the map editor, a handover), so the next frame sets it whatever it is.
enum class MouseMode {
    Unknown,
    Relative,
    Absolute,
};

/// The mode applyMouse sets this frame, nothing if it's set already: SDL is called only on a
/// change (and a failure to set it logged once, not tried again every frame).
constexpr std::optional<MouseMode> mouseModeChange(MouseMode applied, bool relative)
{
    const MouseMode want = relative ? MouseMode::Relative : MouseMode::Absolute;
    if (want == applied) {
        return std::nullopt;
    }
    return want;
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
