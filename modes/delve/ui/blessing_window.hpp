// A Delving Mode altar's offer (noxcore server/delve_map.hpp kBlessing): standing at a blessing
// room's altar, a noxsdl player takes one of three blessings for the rest of the run. Three
// cards over the middle of the screen (each blessing's name, what it does, how many of it the
// player holds); a click on one sends the pick (HeadlessClient::takeBlessing), and the server
// celebrates it. The cards go when the offer does (taken, or the player walked off the altar).
// A stock player is offered the same in text lines and answers by chat. Not in the original
// game.
#pragma once

#include "text.hpp"

#include "noxcore/server/delve_map.hpp"

#include <SDL3/SDL.h>

#include <optional>
#include <span>
#include <vector>

namespace noxsdl {

class BlessingWindow {
public:
    using Offer = std::span<const nox::server::delvemap::BlessingOffer>;

    explicit BlessingWindow(Font* font) : font_(font) {}

    /// Draws the cards while there's an offer, on a w x h view whose HUD scale is `scale`.
    void draw(SDL_Renderer* renderer, Offer offer, int w, int h, float scale, SDL_FPoint mouse) const;
    /// A click on the panel: `pick` the blessing clicked, if one was. Returns whether the event
    /// was the panel's.
    bool handleEvent(const SDL_Event& event, Offer offer, int w, int h, float scale,
                     std::optional<nox::server::delvemap::Blessing>& pick) const;

private:
    struct Layout {
        SDL_FRect panel{};
        std::vector<SDL_FRect> cards;
        int text = 0;
        int title = 0;
        float line = 0;
    };
    Layout layout(Offer offer, int w, int h, float scale) const;

    Font* font_;
};

} // namespace noxsdl
