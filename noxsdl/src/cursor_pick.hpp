// What the mouse is on in the world (clientEnumHover / clientOnCursorHover) and which cursor that
// gives (cursorUpdate's world part). No SDL here, so the tests take it as it is.
//
//   shape    a thing's EXTENT (a circle of radius r, or a box w x h turned 45 degrees) stood up
//            from its ZSIZE bottom to its top: it's hit inside the footprint at either level, or
//            between the two levels within its sideways span. In whole world units: the cursor's
//            point and the thing's spot whole, the levels, the radius and the box's corners cut
//            toward zero. A type whose thing.bin record has no ZSIZE line stands 0 to 30
//            (nox::things::thingZSize): the NPC, the Wizard and the other creatures without one
//            are hit up their bodies, not only at their feet.
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
//            talks, trades, picks up or uses nothing. None either while the game is paused (a
//            solo game's pause) or our player is dying or dead: the move cursor stays.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace nox::things {
struct ThingDef;
}

namespace noxsdl::pick {

/// A thing.bin record's EXTENT line, read in one place for all that use it: its shape (the word
/// before the first space: CIRCLE r or BOX w h; any other, CENTER, NULL: none) and its numbers,
/// as decimals (the hover's shape: each as strtof reads it from the first space on, 0 from the
/// first that doesn't read) and as whole numbers (the drawing's extent, TypeArt's: each a run of
/// spaces or tabs then an integer, 0 from the first that isn't one, so "12.5 20" reads 12 0).
struct Extent {
    enum class Kind : std::uint8_t { None, Circle, Box };
    Kind kind = Kind::None;
    bool spaced = false;         ///< a space after the word (without, no numbers: TypeArt takes no extent)
    std::array<float, 2> size{}; ///< decimals
    std::array<int, 2> whole{};  ///< whole numbers
};
Extent readExtent(std::string_view line);
/// The record's last EXTENT line read (readExtent); nullopt without one.
std::optional<Extent> extentOf(const nox::things::ThingDef& def);

/// A thing's hover shape: thing.bin's EXTENT and ZSIZE.
struct Shape {
    bool box = false;   ///< EXTENT BOX (width x height), else CIRCLE (radius)
    float radius = 0;
    float width = 0;
    float height = 0;
    float zMin = 0;     ///< ZSIZE's bottom
    float zMax = 0;     ///< ZSIZE's top
};

/// A thing type's hover shape from its thing.bin record: the last EXTENT line's CIRCLE r or BOX
/// w h (radius, width and height read as decimals; CENTER, NULL or none: no shape, never hit),
/// and its ZSIZE as nox::things::thingZSize gives it (0 to 30 without a ZSIZE line).
Shape shapeOf(const nox::things::ThingDef& def);
/// The same with its EXTENT read already (extentOf).
Shape shapeOf(const nox::things::ThingDef& def, const std::optional<Extent>& extent);

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
    bool paused = false;      ///< the game is paused (Game_IsPaused: cursorUpdate skips the world)
    bool dying = false;       ///< our player is dying or dead, or has no drawable (playerAnimCheck: skips it too)
};

/// Whether cursorUpdate skips its world part (no Talk, Trade, pickup or Use cursor, no ground
/// item's name): under a cinematic's widescreen, while the game is paused, or with our player
/// dying or dead. (Its other tests, a play state other than 3 and an observer, are the caller's:
/// not in a game, or not playing, the world's cursors aren't asked for.)
constexpr bool worldSkipped(const Situation& s)
{
    return s.wideScreen || s.paused || s.dying;
}

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
