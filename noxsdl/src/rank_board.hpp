// The scoreboard (guirank.c, guiDrawRank and its draw): ToggleRank (F9)
// steps through its modes: off, the quest list (quest games only), the full board
// (TeamPlayerRank: the teams, then every player), the teams alone (Teams), the top three (Top3),
// and the WOL ranks (WolRank, online games only: the players with their WOL account names, "--"
// for a score under 1), then off again. The quest list (Noxworld.c:Quest) shows the players and
// their lives (MSG_GAUNTLET 4), health and classes, and in the marker column the keys they hold
// (MSG_GAUNTLET 0x16 / 0x17: a white key's shape for a SilverKey, a yellow one for a GoldKey).
// A game's end opens the full board (the quest list in quests) until the next map. The original shows the
// quest list and the full board modal; here they don't take the mouse.
//
// Drawn over the game at the top left with no background, in the default font: the title (green),
// "your rank N / M" (yellow; your team's in the teams mode), the lesson limit (white) and the time
// left (red), then two panels side by side of five columns: player (cut to 80 wide), a marker for
// whoever carries a flag, the ball or the crown, class, score and ping. Sixteen players to a
// panel. Rows take the palette colour of the player's team (two darker once eliminated), light
// grey without a team, green for observers (listed last); a yellow dot marks our row.
// Players are listed by lessons, most first, or in elimination games (flag 0x400) by deaths,
// fewest first; teams by score the same way.
#pragma once

#include "text.hpp"

#include "noxcore/client/world.hpp"
#include "noxcore/strings/string_table.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace noxsdl {

class RankBoard {
public:
    RankBoard(Font* font, const nox::strings::StringTable* strings) : font_(font), strings_(strings) {}

    /// ToggleRank: the next mode. `quest`: a quest game (mode 1); `online`: a WOL game
    /// (mode 5).
    void cycle(bool quest, bool online);
    int mode() const { return mode_; }
    bool shown() const { return mode_ != 0; }
    /// The quest list and the full board, which the original shows modal.
    bool modal() const { return mode_ == 1 || mode_ == 2; }
    /// A game's end (Gui_Rank_ShowOnGameOver): the board opens, keeping the mode it
    /// was in; already on the full board it stays.
    void gameOver(bool quest, bool online);
    /// Each frame (the board's draw, at its start): once the game is no longer over, the mode it had before.
    void update(bool over, bool quest, bool online);

    /// Draws it on a w x h view (the HUD's scale) for the local player `us`.
    void draw(SDL_Renderer* renderer, const nox::client::World& world, std::uint16_t us, int w, int h);

private:
    struct Line {
        std::string text;
        std::uint8_t color = 4;
    };
    using Column = std::vector<Line>;
    using Panel = std::array<Column, 5>;

    std::string text(std::string_view id) const;

    Font* font_;
    const nox::strings::StringTable* strings_;
    int mode_ = 0;
    int savedMode_ = 6; ///< g_guiRankSavedMode: 6 when nothing is saved
};

} // namespace noxsdl
