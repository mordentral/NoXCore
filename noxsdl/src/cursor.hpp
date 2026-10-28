// The game's mouse cursor (guicursor.c): which cursor shows (cursorUpdate),
// what's under it in the world (clientOnCursorHover), and drawing it
// (video_cursorDrawImpl).
//
//   images     thing.bin's named animations CursorSelect, CursorGrab, CursorPickup, ... in the
//              exe's order (cursorLoadAll), drawn at the mouse less (64, 64) plus the image's offset.
//              Move, the in-game arrow, picks its frame from the mouse's angle round the screen
//              centre in 32 steps, the running arrows (+32) beyond 100 units or while running.
//   labels     some carry a word in yellow (guicursor.c's PICKUP, SHOPKEEPER, ...).
//   hover      the drawable under the mouse: its footprint (EXTENT circle, or a box turned 45
//              degrees) stood up from ZSIZE's bottom to its top, as a capsule or prism; the one
//              furthest forward (y + z + ZSIZE bottom) wins. Only items, monsters, players and
//              immobile things count, not us, observers, the dead or the unseen invisible.
//   in game    the arrow; over an item within 75 of us the pickup hand (a click picks it up,
//              MSG_TRY_GET, instead of attacking), further away the far one; over a
//              shopkeeper within 125 the trade cursor; over a useable map object (levers,
//              switches, buttons, chain pulls: IMMOBILE, SUBCLASS USEABLE, enabled) within 75 the
//              Use hand, Action then working it with MSG_TRY_COLLIDE (CliNet_SendTryCollide). Over
//              windows, and in the menus, the pointer.
#pragma once

#include "sprite_cache.hpp"
#include "text.hpp"
#include "thing_art.hpp"

#include "noxcore/strings/string_table.hpp"
#include "noxcore/things/thing_db.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace noxsdl {

/// The exe's cursor numbers (index into its cursor table).
enum class CursorType : int {
    Select = 0,
    Grab = 1,
    Pickup = 2,
    Trade = 3,
    Talk = 4,
    Target = 5,
    Identify = 6,
    IdentifyNo = 7,
    Repair = 8,
    CreateGame = 9,
    Busy = 10,
    Buy = 11,
    Sell = 12,
    Use = 13,
    Move = 14,
    PickupFar = 15,
    Caution = 16,
};

/// Whether a drawable at ground (x, y), height z, with this art, is under the world point
/// (cx, cy) (clientOnCursorHover's shape test).
bool underCursor(const TypeArt& art, float x, float y, float z, float cx, float cy);

/// The hover rule's ordering key: bigger is further forward.
inline float hoverDepth(const TypeArt& art, float y, float z)
{
    return y + z + art.zSizeMin;
}

class Cursor {
public:
    Cursor(const nox::things::ThingDb& things, SpriteCache& sprites, Font* font,
           const nox::strings::StringTable* strings);

    /// Draws `type` with its tip at (x, y) in output pixels, `scale` output pixels per game
    /// pixel. For Move, `moveFrame` is the arrow's frame (0-63). `tint` multiplies the image's
    /// colours (the arrow over something: blue, red for an enemy).
    void draw(SDL_Renderer* renderer, CursorType type, float x, float y, float scale, int moveFrame = 0,
              std::optional<SDL_Color> tint = std::nullopt);

    /// The Move arrow's frame for the mouse at (mx, my) on a screen of `width` x `height`, both
    /// in game units (video_cursorDrawImpl case 14).
    static int moveFrame(float mx, float my, float width, float height, bool running);

private:
    const nox::things::ImageDef* image(CursorType type) const;
    std::string label(const char* id) const;

    const nox::things::ThingDb& things_;
    SpriteCache& sprites_;
    Font* font_;
    const nox::strings::StringTable* strings_;
    std::map<std::string, const nox::things::ImageDef*> images_;
};

} // namespace noxsdl
