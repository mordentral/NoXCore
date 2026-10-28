// A quest's two windows over the game, loaded hidden as the game's GUI starts
// (game_guiInit), shown centred, enabled and without the
// focus, the game going on under them:
//   game over    GGOver.wnd (Gui_GameOver_Show, MSG_GAUNTLET 2): "Game Over", the
//                generators destroyed, secrets found and kills from the message ("%d" in each
//                label: GGOver.wnd:GeneratorsDestroyed / NumSecretsFound / Kills), the consecutive
//                and high stages and the (hidden) total score emptied, GeneralPrint:MiscQuestionQDeath;
//                while it's shown (each frame) the timer "<Rules.c:Time> <s>" of the
//                30 s the host waits (updatePlayer), from its coming, at 0 no less; empty for the host's
//                own player. Its buttons (Gui_GameOver_WndProc, each click 0x2FE): OK
//                (0x29CE) MSG_GAUNTLET 3 (back in, an observer) and Quit (0x29CD) the client's quit
//                (client_quit), both hiding it.
//   shared keys  SKey.wnd (the keys shared: MSG_GAUNTLET 0x18 on): "Key" and
//                GeneralPrint:KeySharingOn; OK (0x2A33, a click 0x2FE) hides it, the
//                keys staying shared (the inventory's icon); their going off hides it too.
#pragma once

#include "gui.hpp"
#include "mode_ui.hpp"

#include "noxcore/client/world.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace noxsdl {

class QuestWindows {
public:
    struct Hooks {
        std::function<void()> continueQuest; ///< MSG_GAUNTLET 3 (HeadlessClient::questContinue)
        std::function<void()> quit;          ///< client_quit
    };
    QuestWindows(gui::Resources res, const std::filesystem::path& windowDir, Hooks hooks);

    /// Each frame: a game over or the keys shared since the last call shows its window, the keys
    /// no longer shared hide theirs, and the game over's timer is set (`frame` the client's game
    /// frame; `host`: our player is the host's, index 0x1F).
    void update(const nox::client::World& world, std::uint32_t frame, bool host, std::uint64_t nowMs);
    /// noxcpp: a mode's own words for the game over (ModeUi::gameOver: one's in a run),
    /// in place of a quest's penalty and continuing; none: the game's.
    void setGameOverWords(std::optional<GameOverWords> words) { words_ = std::move(words); }
    void draw(SDL_Renderer* renderer, int width, int height);
    /// A click on a shown window is its; returns whether it took the event.
    bool handleEvent(const SDL_Event& event, int width, int height);

    bool gameOverShown() const { return gameOver_ && !gameOver_->hidden(); }
    bool keysShown() const { return keys_ && !keys_->hidden(); }

private:
    void show(gui::Window* w);
    std::string text(const std::string& id) const { return gui_.text(id); }

    gui::Gui gui_;
    Hooks hooks_;
    gui::Window* gameOver_ = nullptr;
    gui::Window* keys_ = nullptr;
    std::uint32_t overSeen_ = 0;  ///< QuestGameOver::count shown
    std::uint32_t keysSeen_ = 0;  ///< GameState::sharedKeysShown shown
    std::uint32_t overFrame_ = 0; ///< g_guiGameOverShownFrame
    std::optional<GameOverWords> words_;
    std::vector<std::function<void()>> deferred_;
};

} // namespace noxsdl
