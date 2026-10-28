// A Delving Mode campfire's offer (noxcore server/delve_map.hpp kCampfire): standing by the
// fire, a noxsdl player picks one of its spells to strengthen a level, free, once at that fire.
// A panel over the middle of the screen lists them (each spell's icon, name and its level now
// and next); a click on one sends the pick (HeadlessClient::upgradeSpell), and the server
// celebrates it with a rain of sparks. The panel goes when the offer does (picked, or the player
// walked off the fire). A stock player is offered the same in text lines and answers by chat.
// Not in the original game.
#pragma once

#include "sprite_cache.hpp"
#include "text.hpp"

#include "noxcore/server/delve_map.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace noxsdl {

class CampfireWindow {
public:
    using Offer = std::span<const nox::server::delvemap::Upgradable>;

    explicit CampfireWindow(Font* font) : font_(font) {}

    /// A spell's name as the game says it, and its icon (null: none).
    void setSpellName(std::function<std::string(std::uint8_t)> name) { name_ = std::move(name); }
    void setSpellIcon(std::function<const Sprite*(std::uint8_t)> icon) { icon_ = std::move(icon); }

    /// Draws the panel while there's an offer, on a w x h view whose HUD scale is `scale`.
    void draw(SDL_Renderer* renderer, Offer offer, int w, int h, float scale, SDL_FPoint mouse) const;
    /// A click on the panel: `pick` the spell clicked, if one was. Returns whether the event was
    /// the panel's.
    bool handleEvent(const SDL_Event& event, Offer offer, int w, int h, float scale, std::optional<std::uint8_t>& pick) const;

private:
    struct Layout {
        SDL_FRect panel{};
        std::vector<SDL_FRect> rows;
        int text = 0;
        float line = 0;
    };
    Layout layout(Offer offer, int w, int h, float scale) const;

    Font* font_;
    std::function<std::string(std::uint8_t)> name_;
    std::function<const Sprite*(std::uint8_t)> icon_;
};

} // namespace noxsdl
