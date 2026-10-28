// noxcpp's map vote (noxcore server/map_vote.hpp) on screen: the candidates as cards, each the
// map drawn from above (MapThumbnails) with its number and name under it and the votes it has,
// and the time left. A click on a card votes for it (again to change it); typing its number in
// chat does the same, as players of the stock game do. Our own pick is outlined in gold.
//
// At a match's end the cards are large, two by two in the middle of the screen; a vote called
// in a match (the in-game menu's Vote Map, or a stock player's "?MapVote") puts them small in a row at the top, out of the fight's way. Once
// it closes, the result shows for a few seconds. An arrow at the panel's top right folds it to its
// title line and back (each new vote opens unfolded). Not in the original game.
#pragma once

#include "map_thumbnail.hpp"
#include "text.hpp"

#include "noxcore/client/headless_client.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace noxsdl {

class MapVoteWindow {
public:
    MapVoteWindow(MapThumbnails& thumbnails, Font* font) : thumbnails_(thumbnails), font_(font) {}

    using State = nox::client::HeadlessClient::MapVoteState;

    /// Follows the vote (each frame): asks for the candidates' pictures as one opens, keeps the
    /// result up a while once it's closed.
    void update(const State& vote, std::uint64_t nowMs);
    bool shown() const { return shown_; }

    /// Draws it on a w x h view whose HUD scale is `scale`; `mouse` lights the card under it.
    void draw(SDL_Renderer* renderer, const State& vote, int w, int h, float scale, SDL_FPoint mouse);
    /// Takes a left click over it: on a card, the card's index in `choice`. Returns whether the
    /// click was over it (so goes no further); everything else goes on to the game.
    bool handleEvent(const SDL_Event& event, const State& vote, int w, int h, float scale, std::optional<int>& choice);

private:
    struct Layout {
        SDL_FRect panel{};
        SDL_FRect toggle{};           ///< the fold / unfold arrow
        std::vector<SDL_FRect> cards; ///< the pictures (none folded); name and votes go under each
        float text = 0;               ///< the text's pixel size
    };
    Layout layout(const State& vote, int w, int h, float scale) const;

    MapThumbnails& thumbnails_;
    Font* font_;
    bool shown_ = false;
    bool folded_ = false; ///< only the title line shown (the arrow)
    bool wasOpen_ = false;
    std::uint32_t version_ = 0;
    std::uint64_t resultUntil_ = 0; ///< the closed vote's result shows till then
    std::uint64_t nowMs_ = 0;
};

} // namespace noxsdl
