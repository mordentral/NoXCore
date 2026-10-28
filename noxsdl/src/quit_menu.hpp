// The in-game menu Escape brings up (QuitMenu.wnd, wndLoadQuitMenu), laid out
// each time it opens as the exe does it:
//   multiplayer  Resume, Save (MultiplayerSaveLabel), Gameplay Controls, Observer Mode, Vote,
//                Options and Quit (below Vote), the box 220 x 330; the game isn't paused.
//   solo         Resume, AutoLoad, AutoSave, Save (SoloSaveLabel), Options and Quit (in Vote's
//                place), the box 220 x 285; the game paused while it's up. It doesn't open while
//                our player is dying or dead (animation state 2, 1 or 0x33), an award flies from the book
//                or the game's paused.
// In a darkened bordered box across the top middle of the screen. Escape or Resume closes it
// (unpaused); Quit asks first (GUIQuit.c:ReallyQuit..., Yes / No) and then leaves the game;
// Observer Mode asks the server to make us an observer; Options opens the game's Options window
// (options_window.hpp); Gameplay Controls, on the game we host, closes it and opens the Server
// Settings (server_options.hpp). Solo (menuGameOnButton): AutoLoad closes it and asks to
// load AUTOSAVE (cinematics.hpp); AutoSave, unless our drawable's flagged DEAD (then the button
// goes off and the menu stays), closes it and asks our host for AUTOSAVE; Save closes it and
// opens the save menu. It takes all input while it's open.
//
// Vote, on a noxcpp server whose map vote can be called (on, not a quest), is "Vote Map": it
// closes the menu and calls noxcpp's map vote. Not yet (" - WIP" on their
// buttons, off): multiplayer's Save (the server's copy of the character to the .plr), and Vote
// elsewhere (the stock kick vote).
// Gameplay Controls is off (greyed) in a game we don't host.
#pragma once

#include "gui.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace noxsdl {

class QuitMenu {
public:
    struct Callbacks {
        std::function<void()> observe;
        std::function<void()> quit;
        std::function<void()> options; ///< Options (this has closed)
        /// A solo game (game flag 0x800: main.cpp's soloGame).
        std::function<bool()> solo;
        /// The open's refusals in a solo game: our drawable dying or dead (2, 1, 0x33), an
        /// award flying; and Game_IsPaused in any game.
        std::function<bool()> refused;
        /// Game_SetPaused on the game we host (1 / 0); empty or not hosting: nothing.
        std::function<void(int how)> pause;
        /// Our drawable's flags have DEAD (0x8000): AutoSave goes off.
        std::function<bool()> deadFlag;
        std::function<void()> autoLoad; ///< 0x2329 (this has closed)
        std::function<void()> autoSave; ///< 0x232A: setSaveFileName("AUTOSAVE"), then the save (1, 0, 0)
        std::function<void()> save;     ///< 0x232B: the save menu
        /// We host the game (Game_HasFlags(1)): Gameplay Controls usable.
        std::function<bool()> hosting;
        std::function<void()> gameplay; ///< 0x2330: guiServerOptsLoad (this has closed)
        /// A noxcpp server that says a map vote can be called here (its vote on, not a quest):
        /// Vote calls it.
        std::function<bool()> canVoteMap;
        std::function<void()> voteMap; ///< Vote on a noxcpp server (this has closed)
    };

    QuitMenu(gui::Resources res, std::filesystem::path windowDir, Callbacks callbacks);

    bool isOpen() const { return open_; }
    /// Escape: opens it (unless refused), or closes it (and any
    /// question it's asking).
    void toggle();
    void close();
    /// A game starting (game_guiInit loads the window anew).
    void newGame();

    void update(std::uint64_t nowMs);
    void draw(SDL_Renderer* renderer, int width, int height);
    /// While it's open it takes every event; returns whether it took this one.
    bool handleEvent(const SDL_Event& event, int width, int height);

private:
    bool onButton(gui::Window& from);
    void ask(const std::string& title, const std::string& text, std::function<void()> onYes);
    void closeDialog();
    void layout(bool solo);

    gui::Gui gui_;
    std::filesystem::path windowDir_;
    Callbacks callbacks_;
    gui::Window* menu_ = nullptr;
    gui::Window* dialog_ = nullptr;
    SDL_Rect voteRect_{};
    std::string saveText_[2]; ///< MultiplayerSaveLabel, SoloSaveLabel
    std::string gameplayText_;
    std::string voteText_; ///< GUIQuit.c:Vote
    bool open_ = false;
    bool solo_ = false;
    std::vector<std::function<void()>> deferred_;
};

} // namespace noxsdl
