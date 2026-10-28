// The conjurer's summon panel (guisumn.c, guiSummonCreatureLoad): the creature
// cage in the top right that comes down with our first summoned or charmed creature and goes
// back up with the last, and the command list it opens.
//
//   panel      0x57 x 0x73 at (W - 0x5F, y), y sliding from -0x91 down to 0 and back, 20 a frame
//              (guiDrawSummonBox): CreatureCageBottom, the creatures, CreatureCageTop over them.
//   cage       a 76 x 76 box at panel + (5, 0x26), 2 x 2 cells of 38: each creature's
//              CreatureCage<name> picture (a grey dot if it isn't a guide creature) at its cell
//              + 2, a small one 34 x 34, a medium one 34 x 72, a large one 72 x 72, with a 2-wide
//              health bar down its right edge (dark red, the health red from the bottom; dark
//              green and green while poisoned). Clicking one opens the command list for it.
//   button     the standing order's button (CreatureCageGuard / Escort / HuntButton, lit under
//              the mouse) at panel + (0x1B, 0xC), 34 x 34: the command list for all of them.
//   commands   (wndSummonCreateList) Banish, Observe, Guard, Escort, Hunt, centred on the click, as wide as
//              the widest + 8, on a darkened box with a dark blue frame; a line each, black-edged:
//              yellow under the mouse, else for one creature white (Escort and Hunt grey for a
//              carnivorous plant), for all of them blue (no Observe; Escort and Hunt grey if
//              they're all plants). A click sends MSG_TRY_CREATURE_COMMAND; the list goes when
//              the mouse leaves it.
//   tooltips   a creature's name; the button's order (ccs:GUARD / ESCORT / HUNT).
// Pointing at a creature in the cage (or at the button: all of them) draws it in the world in
// blue (highlighted(), drawable flag 0x40000000).
#pragma once

#include "sprite_cache.hpp"
#include "text.hpp"
#include "thing_art.hpp"

#include "noxcore/client/world.hpp"
#include "noxcore/strings/string_table.hpp"
#include "noxcore/things/thing_db.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace noxsdl {

class SummonWindow {
public:
    SummonWindow(const nox::things::ThingDb& things, const nox::strings::StringTable* strings, SpriteCache& sprites,
                 Font* font);
    void setSound(std::function<void(std::uint32_t)> play) { playSound_ = std::move(play); }

    /// Shows or hides the panel by whether we have creatures; steps its slide (a game frame a
    /// step); closes the list when the mouse has left it or its creature has gone.
    void update(const nox::client::World& world, std::uint32_t frame, float mx, float my, int w, int h);
    void draw(SDL_Renderer* renderer, const nox::client::World& world, int w, int h, SDL_FPoint mouse);

    /// Mouse in output pixels; whether the panel (or the list, which takes every click while
    /// it's up) took it. `command` sends MSG_TRY_CREATURE_COMMAND (net code 0 for all).
    bool mouseDown(float x, float y, int w, int h);
    bool mouseUp(float x, float y, int w, int h, const nox::client::World& world,
                 const std::function<void(std::uint16_t, std::uint8_t)>& command);
    bool contains(float x, float y, int w, int h) const;
    std::string tooltip(float x, float y, int w, int h, const nox::client::World& world) const;
    /// The creatures the world draws highlighted (drawable flag 0x40000000, guiDrawSummonBox): the one in
    /// the cage cell under the mouse, or all of them while it's on the order's button.
    std::set<std::uint16_t> highlighted(float x, float y, int w, int h, const nox::client::World& world) const;

private:
    struct Rect {
        float x = 0, y = 0, w = 0, h = 0;
        bool in(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    };
    struct Menu {
        Rect box;
        std::optional<std::uint16_t> creature; ///< the slot's creature, else all of them
        float itemH = 0;                       ///< a line's height (the font's + 3)
        float step = 0;                        ///< from one line to the next (the font's + 4)
    };

    float panelX(int w, int h) const;
    Rect boxRect(int w, int h) const;
    Rect buttonRect(int w, int h) const;
    /// The creature in the cage cell under game point (gx, gy).
    std::optional<std::uint16_t> creatureAt(const nox::client::World& world, float gx, float gy, int w, int h) const;
    /// The list's line under game point (gx, gy): 0 banish, 1 observe, 3 guard, 4 escort, 5 hunt.
    std::optional<int> itemAt(float gx, float gy) const;
    void open(float gx, float gy, std::optional<std::uint16_t> creature, int w, int h);
    void close();
    std::string text(const std::string& id) const;
    const nox::things::ImageDef* image(const std::string& name) const;
    void drawNamed(SDL_Renderer* r, const std::string& name, float gx, float gy, float s) const;
    bool isPlant(const nox::client::World& world, std::uint16_t netCode) const;
    void sound(std::uint32_t id) const
    {
        if (playSound_) {
            playSound_(id);
        }
    }

    const nox::things::ThingDb& things_;
    const nox::strings::StringTable* strings_;
    SpriteCache& sprites_;
    Font* font_;
    std::map<std::string, const nox::things::ImageDef*, std::less<>> images_;
    std::function<void(std::uint32_t)> playSound_;
    int state_ = 0;   ///< 0 up (hidden), 1 coming down, 2 down, 3 going up
    int y_ = -0x91;   ///< the panel's top
    std::uint32_t lastFrame_ = 0;
    bool buttonDown_ = false;
    std::optional<Menu> menu_;
    int lastHovered_ = -1; ///< the line last under the mouse (its sound plays once)
};

} // namespace noxsdl
