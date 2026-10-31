// What the mouse is on in the world (clientEnumHover / clientOnCursorHover) and which cursor that
// gives (cursorUpdate's world part). No SDL here, so the tests take it as it is.
//
//   shape    a thing's EXTENT (a circle of radius r, or a box w x h turned 45 degrees) stood up
//            from its ZSIZE bottom to its top: it's hit inside the footprint at either level, or
//            between the two levels within its sideways span. In whole world units: the cursor's
//            point and the thing's spot whole, the levels, the radius and the box's corners cut
//            toward zero.
//   picks    of the things hit, the one furthest forward (y + z + ZSIZE bottom, whole units; 0 or
//            less never) wins, the first one offered on a tie. Two picks are kept: the front one
//            (the ring at its feet, the arrow's colour, the aim and the spells' target) and the
//            cursor's (talk, trade, pick up, use, an item's name): a Glyph is the cursor's only
//            for a wizard, and then only while nothing else is under the cursor.
//   cursor   from the cursor's pick, measured from us (whole units): an item, the pickup hand
//            within 75 (Caution for what our class can't use), the far one beyond; a creature
//            with something to say, the Talk hand within 125, a shopkeeper the Trade one (not in a
//            dialog or a shop); a useable map object or a Mimic still a chest, the Use hand within
//            75. While an inventory item is dragged an item gets none. Under a cinematic's
//            widescreen (the letterbox bars) none at all: the arrow stays, so a click there
//            talks, trades, picks up or uses nothing.
#pragma once

#include <cstdint>
#include <optional>

namespace noxsdl::pick {

/// A thing's hover shape: thing.bin's EXTENT and ZSIZE.
struct Shape {
    bool box = false;   ///< EXTENT BOX (width x height), else CIRCLE (radius)
    float radius = 0;
    float width = 0;
    float height = 0;
    float zMin = 0;     ///< ZSIZE's bottom
    float zMax = 0;     ///< ZSIZE's top
};

/// Whether world point (cx, cy) is on a thing standing at (x, y), at height z.
bool under(const Shape& shape, int x, int y, int z, int cx, int cy);

/// Whether a thing standing at (x, y) is one the hover looks at for world point (cx, cy): those
/// in the 128-unit squares of the world that the square 96 units round the point touches.
bool nearCursor(int x, int y, int cx, int cy);

/// The hover's ordering key: bigger is further forward (y + z + ZSIZE bottom, cut toward zero).
int depth(const Shape& shape, int y, int z);

/// How a thing under the cursor may be the cursor's pick.
enum class CursorRole : std::uint8_t {
    Any,        ///< like any other
    None,       ///< never (a Glyph, for anyone but a wizard)
    LastResort, ///< only while it has none; anything after it then takes its place (a wizard's Glyph)
};

/// The two picks, offered every thing under the cursor in turn.
template <class T>
struct Picks {
    std::optional<T> front;  ///< the one furthest forward of all
    float frontDepth = 0;
    std::optional<T> cursor; ///< the cursor's
    float cursorDepth = 0;

    void offer(const T& thing, float depth, CursorRole role)
    {
        if (depth > frontDepth) {
            front = thing;
            frontDepth = depth;
        }
        if (depth <= cursorDepth || role == CursorRole::None) {
            return;
        }
        if (role == CursorRole::Any) {
            cursor = thing;
            cursorDepth = depth;
        } else if (!cursor) {
            cursor = thing;
            cursorDepth = 0;
        }
    }
};

/// The world's cursors (the exe's cursor numbers: CursorType's).
enum class WorldCursor : std::uint8_t {
    None,      ///< the arrow stays
    Pickup,
    PickupFar,
    Caution,   ///< within reach, but our class can't use it
    Talk,
    Trade,
    Use,
};

/// The cursor's pick, as cursorUpdate weighs it.
struct Target {
    std::uint32_t classes = 0;    ///< CLASS bits (nox::things::ObjectClass)
    std::uint32_t subclasses = 0;
    std::uint32_t xStatus = 0;    ///< MSG_REPORT_X_STATUS (0x10: something to say)
    bool enabled = true;
    bool mimicAsleep = false;     ///< a Mimic in its first state (still a chest)
    std::int64_t dist2 = 0;       ///< from us, whole units squared
};

struct Situation {
    bool busy = false;        ///< in a dialog or a shop
    bool dragging = false;    ///< an inventory item on the cursor
    bool canUse = true;       ///< our class may use the item (or a solo or quest game)
    bool wideScreen = false;  ///< a cinematic's widescreen is on (cursorUpdate skips the world)
};

WorldCursor worldCursor(const Target& target, const Situation& situation);

/// itemClassUseMask, as cursorUpdate asks it: the classes (bit n for class n) that may use an item.
/// A weapon, wand or armour its record's CLASSUSE (`record`: 0 without a record or without the
/// line, the game's record byte zeroed); a food ~(SUBCLASS >> 5 & 7) | 0xFE, so only a mana potion
/// (SUBCLASS bit 5) drops a class, the warrior's; anything else every class.
std::uint8_t itemClassUse(std::uint32_t classes, std::uint32_t subclasses, std::uint8_t record);

/// Situation::canUse for an item under the cursor: in a solo or quest game always (the server's
/// pickups don't ask there); a glyph only for a wizard; noxcpp: with a host whose mode lets any
/// class pick anything up (ServerFeature::PickupAnyClass in its extended reply, World::pickupAnyClass:
/// weapons, wands, armour and potions taken, never worn) always; else when `use` has our class.
bool classMayTake(std::uint8_t use, int playerClass, bool glyph, bool soloOrQuest, bool anyClass);

/// The facing sent each frame (control 1), in Nox facing units (0 east, 64 south, clockwise on
/// screen), from (fromX, fromY) to the cursor: to its x at the front pick's own y (`frontY`: where
/// the thing stands, the y MSG_MOUSE carries for it, not where its body is drawn) while there's
/// one under it, else to the cursor's y. The game's sum: the angle plus a turn, times 256 a turn,
/// plus a half, cut toward zero, then wrapped into 0..255.
std::uint8_t facing(float fromX, float fromY, float cursorX, float cursorY, std::optional<int> frontY);

/// How far above the pointer a tooltip's top goes, by the cursor showing (the exe's cursor number,
/// CursorType's), in `line`s of the font's height: video_cursorDrawImpl sets it per cursor, a line
/// and 4 by default (client_drawCursorAndTooltips'). The hands with a word under them (Pickup 2,
/// Use 13) and the far pickup hand (15) put it two lines below the pointer, under that word;
/// Caution (16) a line below; Repair (8) two lines and 4 above. Below the pointer is negative.
constexpr float tooltipLift(int cursor, float line)
{
    switch (cursor) {
    case 2:
    case 13:
    case 15: return -2 * line;
    case 16: return -line;
    case 8: return 2 * line + 4;
    default: return line + 4;
    }
}

} // namespace noxsdl::pick
