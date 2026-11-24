// What the mouse is on in the world and which cursor that gives (cursor_pick.hpp).
#include "cursor_pick.hpp"

#include "noxcore/things/object_class.hpp"
#include "noxcore/things/thing_db.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <system_error>

namespace noxsdl::pick {

namespace {

/// A float to whole units as the game converts them: cut toward zero.
int chop(float v)
{
    return static_cast<int>(v);
}

/// A box's corners from its centre: w / (2 sqrt 2) and h / (2 sqrt 2), as the shape keeps them.
constexpr float kHalfDiagonal = 0.35355339f;
/// The point-in-box test's scale (1 / sqrt 2): the sides' distances, signed.
constexpr float kSideScale = 0.70710677f;

} // namespace

Extent readExtent(std::string_view line)
{
    Extent e;
    // The hover's: the game's client reader takes a line that starts with CIRCLE or BOX as that
    // shape ("CIRCLES 9" a circle too), then its decimals after the first word ("%*s %f %f": the
    // word ends at any white space), each as strtof reads it, 0 from the first that doesn't read.
    e.hoverKind = line.starts_with("CIRCLE") ? Extent::Kind::Circle : line.starts_with("BOX") ? Extent::Kind::Box : Extent::Kind::None;
    if (e.hoverKind != Extent::Kind::None) {
        const auto white = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
        std::size_t at = 0;
        while (at < line.size() && white(line[at])) {
            ++at;
        }
        while (at < line.size() && !white(line[at])) {
            ++at;
        }
        const std::string s(line.substr(at));
        const char* p = s.c_str();
        for (float& v : e.size) {
            char* end = nullptr;
            v = std::strtof(p, &end);
            if (end == p) {
                v = 0;
                break;
            }
            p = end;
        }
    }
    // The drawing's: the word before the first space, CIRCLE or BOX whole.
    const std::size_t space = line.find(' ');
    const std::string_view word = line.substr(0, space);
    e.kind = word == "CIRCLE" ? Extent::Kind::Circle : word == "BOX" ? Extent::Kind::Box : Extent::Kind::None;
    if (e.kind == Extent::Kind::None) {
        return e;
    }
    e.spaced = space != std::string_view::npos;
    if (!e.spaced) {
        return e; // (no numbers)
    }
    // The whole numbers: after the space, each spaces or tabs then an integer (from_chars: no
    // sign but '-', no fraction), stopping at the first that isn't.
    const std::string_view rest = line.substr(space + 1);
    const char* q = rest.data();
    const char* const qend = rest.data() + rest.size();
    for (int& v : e.whole) {
        while (q < qend && (*q == ' ' || *q == '\t')) {
            ++q;
        }
        if (q >= qend) {
            break;
        }
        const auto [next, ec] = std::from_chars(q, qend, v);
        if (ec != std::errc{}) {
            break;
        }
        q = next;
    }
    return e;
}

std::optional<Extent> extentOf(const nox::things::ThingDef& def)
{
    if (const auto extent = def.lastAttribute("EXTENT")) {
        return readExtent(*extent);
    }
    return std::nullopt;
}

Shape shapeOf(const nox::things::ThingDef& def)
{
    return shapeOf(def, extentOf(def));
}

Shape shapeOf(const nox::things::ThingDef& def, const std::optional<Extent>& extent)
{
    Shape shape;
    if (extent && extent->hoverKind == Extent::Kind::Circle) {
        shape.radius = extent->size[0];
    } else if (extent && extent->hoverKind == Extent::Kind::Box) {
        shape.box = true;
        shape.width = extent->size[0];
        shape.height = extent->size[1];
    }
    const nox::things::ZSize z = nox::things::thingZSize(def);
    shape.zMin = z.bottom;
    shape.zMax = z.top;
    return shape;
}

bool under(const Shape& shape, int x, int y, int z, int cx, int cy)
{
    // The two levels: the footprint at the bottom (y - ZSIZE bottom - z) and at the top
    // (y - ZSIZE top - z).
    const auto fz = static_cast<float>(z);
    const int top = chop((static_cast<float>(y) - shape.zMax) - fz);
    const int bottom = chop((static_cast<float>(y) - shape.zMin) - fz);
    if (!shape.box) {
        const int r = chop(shape.radius);
        const std::int64_t r2 = chop(shape.radius * shape.radius);
        // Between the two levels: within the radius sideways.
        if (cy <= bottom && top <= cy) {
            return x - r < cx && cx < x + r;
        }
        // Below the bottom or above the top: inside the circle at that level.
        const std::int64_t dx = cx - x;
        const std::int64_t dy = cy - (cy <= bottom ? top : bottom);
        return dx * dx + dy * dy < r2;
    }
    // A box turned 45 degrees: its top, left and right corners from the centre.
    const float f1 = shape.width * kHalfDiagonal;
    const float f2 = shape.height * kHalfDiagonal;
    const float topX = f2 - f1, topY = -f1 - f2;
    const float leftX = -f1 - f2, leftY = f2 - f1;
    const float rightX = f2 + f1, rightY = f1 - f2;
    const auto qx = static_cast<float>(cx);
    const auto qy = static_cast<float>(cy);
    // Inside its four sides with the centre at (x, level).
    const auto inBox = [&](int level) {
        const auto px = static_cast<float>(x);
        const auto py = static_cast<float>(level);
        const float a = (((topY + py) - (topX + px)) + qx) - qy;
        const float b = (((leftY + py) - (leftX + px)) + qx) - qy;
        const float d = ((((rightY + py) + rightX) + px) - qx) - qy;
        const float e = (((topY + py) + (topX + px)) - qx) - qy;
        return a * kSideScale < 0 && 0 < b * kSideScale && 0 < d * kSideScale && e * kSideScale < 0;
    };
    if (inBox(bottom) || inBox(top)) {
        return true;
    }
    // Between the levels: the left half from the left corner to the centre, the band between the
    // levels moved by that corner's height; the right half likewise from the centre to the right
    // corner.
    const int lx = chop(leftX);
    const int ly = chop(leftY);
    if (x + lx < cx && cx < x && top + ly < cy && cy < bottom + ly) {
        return true;
    }
    const int rx = chop(rightX);
    const int ry = chop(rightY);
    return x <= cx && cx < x + rx && top + ry < cy && cy < bottom + ry;
}

bool nearCursor(int x, int y, int cx, int cy)
{
    // The drawables' grid: a square for every 128 units (the division cut toward zero).
    constexpr int kCell = 128;
    constexpr int kReach = 96;
    const auto within = [](int v, int c) {
        const int lo = std::max(0, (c - kReach) / kCell);
        const int hi = (c + kReach) / kCell;
        const int at = v / kCell;
        return lo <= at && at <= hi;
    };
    return within(x, cx) && within(y, cy);
}

int depth(const Shape& shape, int y, int z)
{
    return chop((static_cast<float>(z) + static_cast<float>(y)) + shape.zMin);
}

WorldCursor worldCursor(const Target& t, const Situation& s)
{
    namespace oc = nox::things::ObjectClass;
    constexpr std::uint32_t kShopkeeper = 0x8; // a monster's SUBCLASS SHOPKEEPER
    constexpr std::uint32_t kUseable = 0x80;   // an immobile thing's SUBCLASS USEABLE
    constexpr std::uint32_t kTalks = 0x10;     // X_STATUS: something to say
    constexpr std::int64_t kTalkReach = 0x3D08; // 125 squared, less one (15624)
    constexpr std::int64_t kReach = 0x15F8;     // 75 squared, less one (5624)
    if (worldSkipped(s)) {
        return WorldCursor::None; // (cursorUpdate: no world cursor under the widescreen, paused or dying)
    }
    const bool monster = (t.classes & oc::Monster) != 0;
    const bool talker = monster && (t.xStatus & kTalks);
    const bool shopkeeper = monster && (t.subclasses & kShopkeeper);
    if (!(t.classes & (oc::Immobile | oc::Monster | oc::Player)) || talker || shopkeeper) {
        if (!(t.classes & oc::Pickup) || s.dragging) {
            // Within 125, not in a dialog or a shop: the Talk hand, else a shopkeeper's Trade.
            if (t.dist2 > kTalkReach || s.busy) {
                return WorldCursor::None;
            }
            return talker ? WorldCursor::Talk : shopkeeper ? WorldCursor::Trade : WorldCursor::None;
        }
        if (t.dist2 > kReach) {
            return WorldCursor::PickupFar;
        }
        return s.canUse ? WorldCursor::Pickup : WorldCursor::Caution;
    }
    // A useable immobile thing (levers, switches, buttons), or a Mimic still a chest: the Use hand
    // within 75, enabled, X_STATUS bits 4 and 8 clear.
    const bool blocked = (t.xStatus & 0xC) != 0;
    const bool useable = (t.classes & oc::Immobile) && (t.subclasses & kUseable) && !blocked;
    if ((useable || t.mimicAsleep) && t.dist2 <= kReach && t.enabled && !blocked) {
        return WorldCursor::Use;
    }
    return WorldCursor::None;
}

std::uint8_t itemClassUse(std::uint32_t classes, std::uint32_t subclasses, std::uint8_t record)
{
    namespace oc = nox::things::ObjectClass;
    if (classes & (oc::Weapon | oc::Wand | oc::Armor)) {
        return record;
    }
    if (classes & oc::Food) {
        return static_cast<std::uint8_t>(~((subclasses >> 5) & 7) | 0xFE); // (only bit 0, the warrior's, can drop out)
    }
    return 0xFF;
}

bool classMayTake(std::uint8_t use, int playerClass, bool glyph, bool soloOrQuest, bool anyClass)
{
    if (soloOrQuest) {
        return true;
    }
    if (glyph) {
        return playerClass == 1; // (a wizard's alone: pickupAnyClass doesn't reach a glyph, its trap's own pickup)
    }
    return anyClass || ((use >> playerClass) & 1u) != 0;
}

std::uint8_t facing(float fromX, float fromY, float cursorX, float cursorY, std::optional<int> frontY)
{
    // (The game's x87: the angle from FPATAN, the turn, the units a turn and the half its float
    // constants; the sum cut toward zero.)
    constexpr float kTurn = 6.2831855f;     // 2 pi
    constexpr float kPerTurn = 40.743664f;  // 256 / (2 pi)
    const double dx = static_cast<double>(cursorX) - fromX;
    const double dy = (frontY ? static_cast<double>(*frontY) : static_cast<double>(cursorY)) - fromY;
    int f = static_cast<int>((std::atan2(dy, dx) + kTurn) * kPerTurn + 0.5f);
    if (f < 0) {
        f += static_cast<int>((0xFFu - static_cast<unsigned>(f)) & 0xFFFFFF00u);
    }
    if (f > 0xFF) {
        f -= static_cast<int>((static_cast<unsigned>(f) >> 8) << 8);
    }
    return static_cast<std::uint8_t>(f);
}

} // namespace noxsdl::pick
