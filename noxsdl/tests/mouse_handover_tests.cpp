#include "mouse_handover.hpp"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main()
{
    using namespace noxsdl;
    for (bool hidden : {false, true}) {
        const auto menus = windowMouse(MouseOwner::Menus, hidden);
        require(!menus.relative && !menus.grabbed && !menus.confined, "menus let the mouse cross the window edge");
        require(!menus.systemPointer && !menus.heldPointer, "menus draw the Nox cursor at the current mouse position");
        const auto game = windowMouse(MouseOwner::Game, hidden);
        require(game.relative == !hidden && !game.systemPointer, "games capture with the Nox cursor, except in hidden runs");
        const auto editor = windowMouse(MouseOwner::Editor, hidden);
        require(editor.systemPointer && !editor.relative && !editor.confined, "editor keeps its free system cursor");
        const auto panel = windowMouse(MouseOwner::GameInPanel, hidden);
        require(panel.relative == !hidden && panel.confined == !hidden && !panel.systemPointer,
                "captured play-test keeps its panel confinement");
        const auto released = windowMouse(MouseOwner::GameInPanelReleased, hidden);
        require(!released.relative && !released.confined && released.systemPointer && released.heldPointer,
                "released play-test hands the mouse back to the editor");
    }

    // Startup -> play -> Escape/Options -> resume -> focus loss -> refocus.
    require(!gameplayMouseCapture(false, true, false), "startup menus do not capture");
    require(gameplayMouseCapture(true, true, false), "entering gameplay captures");
    require(!gameplayMouseCapture(false, true, false), "opening Escape/Options releases capture");
    require(gameplayMouseCapture(true, true, false), "closing menus restores capture");
    require(!gameplayMouseCapture(true, false, false), "unfocused gameplay does not capture");
    require(gameplayMouseCapture(true, true, false), "refocused gameplay restores capture");
    for (bool gameplay : {false, true}) {
        for (bool focused : {false, true}) {
            require(!gameplayMouseCapture(gameplay, focused, true), "hidden screenshot runs never capture");
        }
    }
    require(pointerSource(false, true, false) == PointerSource::Mouse,
            "leaving the editor for menus must not retain a stale held pointer");
    std::cout << "Mouse ownership and capture transitions passed\n";
}
