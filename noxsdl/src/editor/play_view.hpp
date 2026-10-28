// The play-test as the editor shows it: play-in-editor, the game drawn in the Map panel with the
// editor working round it (Unreal's PIE), or over the whole window when "Play in Full Window" is
// on. The toolbar's Play, Pause and Stop and the play state; the Play menu's Pause, Release Mouse
// and Play in Full Window; the Map panel's game frame, the click that gives the game the keyboard
// and mouse, and the key that takes them back.
//
// main.cpp owns the game and its input routing (play_test.hpp's hooks); this is the editor's side.
#pragma once

#include "editor/play_test.hpp"

#include <SDL3/SDL.h>

#include <cstdint>

struct ImVec2;

namespace noxsdl {

class MapEditor;

class PlayView {
public:
    explicit PlayView(MapEditor& editor);

    /// Its commands (play.pause, play.release, play.fullWindow), before the user's bindings are read.
    void registerCommands();
    /// The running test's state this frame (none running: an empty view).
    editor::PlayTestView state() const;
    bool running() const;
    /// The game plays in the Map panel now (an in-panel test is running).
    bool inPanel() const;
    /// Before ImGui's frame: ImGui leaves the mouse alone while the game has it.
    void beforeUi();
    /// The Map panel's content while the game plays in it: its frame, the click that gives it the
    /// keyboard and mouse, the hints.
    void panel(const ImVec2& origin, const ImVec2& size);
    /// The toolbar's Play / Pause / Stop / Reload and the play state.
    void toolbarButtons();
    /// The Play menu's items (appended to the Script panel's Play menu).
    void menus();
    /// A key pressed while the game has the keyboard: true when it's one of the Play commands'
    /// (Stop, Pause, Release Mouse), run here and kept from the game.
    bool gameKey(const SDL_Event& event);

private:
    MapEditor& ed_;
    bool wasCaptured_ = false;
    std::uint64_t capturedMs_ = 0; ///< when the game last took the input (its hint shown a while)
};

} // namespace noxsdl
