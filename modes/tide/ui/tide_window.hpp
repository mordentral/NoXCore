// noxcpp's Undead Tide on a noxsdl (noxcore server/tide.hpp tidemsg): the run's own windows.
//   - along the top: the XP bar, the level, the run's clock (the guardian's time and dawn's under
//     it), the slain; under them the auto-weapons' icons, each with its level and its wait swept
//     round it as it readies (run down here between the server's word), then the passives';
//   - at a level-up: three cards (an icon, a name, new or its level, a line of what it does), taken
//     by a click or the keys 1-3, the seconds left till the first is taken for the player;
//   - at the run's end: the results (won or lost, the time, the guardian, each player's level,
//     slain and weapons), till OK.
// A stock player has the same in chat lines. Not in the original game.
#pragma once

#include "sprite_cache.hpp"
#include "text.hpp"

#include "noxcore/server/tide.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace noxsdl {

class TideWindow {
public:
    using Hud = nox::server::tidemsg::Hud;
    using Offer = nox::server::tidemsg::Offer;
    using Results = nox::server::tidemsg::Results;

    explicit TideWindow(Font* font) : font_(font) {}

    /// A weapon's and a passive's icons (null: none, a lettered square instead).
    void setWeaponIcon(std::function<const Sprite*(nox::server::TideWeapon)> icon) { weaponIcon_ = std::move(icon); }
    void setPassiveIcon(std::function<const Sprite*(nox::server::TidePassive)> icon) { passiveIcon_ = std::move(icon); }

    /// The client's word, each frame: a new HUD (its version changed) restarts the weapons' waits
    /// from `now`; an offer's restarts its seconds; new results open their window.
    void update(const std::optional<Hud>& hud, std::uint32_t hudVersion, const Offer& offer, std::uint32_t offerVersion,
                const std::optional<Results>& results, std::uint32_t resultsVersion, std::uint64_t nowMs);

    /// Draws the strip while there's a HUD, the cards while there's an offer, the results while
    /// they're open; on a w x h view whose HUD scale is `scale`.
    void draw(SDL_Renderer* renderer, const std::optional<Hud>& hud, const Offer& offer, const std::optional<Results>& results, int w, int h,
              float scale, SDL_FPoint mouse, std::uint64_t nowMs) const;
    /// A click on a card or a key 1-3 sets `pick` (the offer's index); a click on OK, Enter or
    /// Escape closes the results. Returns whether the event was the window's.
    bool handleEvent(const SDL_Event& event, const Offer& offer, const std::optional<Results>& results, int w, int h, float scale,
                     std::optional<std::size_t>& pick);

    bool resultsOpen() const noexcept { return resultsOpen_; }

    /// The lobby gate's question (the server's kAsk: `ask` the difficulty now, while it's open):
    /// "Begin the run at which difficulty?", the four, each with what it does, and Cancel.
    void drawAsk(SDL_Renderer* renderer, const std::optional<std::uint8_t>& ask, int w, int h, float scale, SDL_FPoint mouse) const;
    /// A click on one (or keys 1-4, Enter for the one now) sets `chosen`; Cancel or Escape sets
    /// `dismissed`. Returns whether the event was the question's.
    bool handleAskEvent(const SDL_Event& event, const std::optional<std::uint8_t>& ask, int w, int h, float scale, std::optional<std::uint8_t>& chosen,
                        bool& dismissed) const;

private:
    struct AskLayout {
        SDL_FRect panel{};
        std::vector<SDL_FRect> rows;
        SDL_FRect cancel{};
        int text = 0;
        int big = 0;
        float line = 0;
    };
    AskLayout askLayout(int w, int h, float scale) const;
    struct CardsLayout {
        SDL_FRect area{};
        std::vector<SDL_FRect> cards;
        int text = 0;
        int big = 0;
        float line = 0;
    };
    CardsLayout cardsLayout(const Offer& offer, int w, int h, float scale) const;
    struct ResultsLayout {
        SDL_FRect panel{};
        SDL_FRect ok{};
        int text = 0;
        int big = 0;
        float line = 0;
    };
    ResultsLayout resultsLayout(const Results& results, int w, int h, float scale) const;
    void drawStrip(SDL_Renderer* renderer, const Hud& hud, int w, float scale, std::uint64_t nowMs) const;
    void drawCards(SDL_Renderer* renderer, const Offer& offer, int w, int h, float scale, SDL_FPoint mouse, std::uint64_t nowMs) const;
    void drawResults(SDL_Renderer* renderer, const Results& results, int w, int h, float scale, SDL_FPoint mouse) const;
    /// An icon in `box` (its aspect kept), or a square with the name's first letter.
    void drawIcon(SDL_Renderer* renderer, const Sprite* icon, std::string_view name, SDL_FRect box, int text) const;
    const Sprite* iconOf(const nox::server::TidePick& p) const;

    Font* font_;
    std::function<const Sprite*(nox::server::TideWeapon)> weaponIcon_;
    std::function<const Sprite*(nox::server::TidePassive)> passiveIcon_;
    std::uint32_t hudVersion_ = 0;
    std::uint64_t hudAt_ = 0;
    std::uint32_t offerVersion_ = 0;
    std::uint64_t offerAt_ = 0;
    std::uint32_t resultsVersion_ = 0;
    bool resultsOpen_ = false;
};

} // namespace noxsdl
