// The in-game map (TAB): what minimap.hpp describes, drawn with smooth lines and dots over the screen.
#include "minimap.hpp"

#include "effects_draw.hpp"
#include "team_colors.hpp"

#include "noxcore/map/objects.hpp"
#include "noxcore/map/sections.hpp"
#include "noxcore/things/object_class.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace noxsdl {

namespace {

namespace ObjectClass = nox::things::ObjectClass;

constexpr float kCell = 23;
constexpr int kZoomMin = 500;
constexpr int kZoomMax = 4000;
constexpr std::uint32_t kVisibleObelisk = 0x8; // the "other" SUBCLASS table's VISIBLE_OBELISK
constexpr std::uint32_t kEnchantBlinded = 1u << 2;
constexpr std::uint32_t kEnchantCrown = 1u << 30;
constexpr std::uint32_t kRevealWalls = 0x10000; // game flag: every wall on the map, seen or not
constexpr std::uint32_t kCaptureTheFlag = 0x20;
constexpr std::uint32_t kCarriesFlag = 0x1;     // weapon class bit of the FLAG item

// loadDefColor's colours the map uses.
constexpr SDL_Color kWallColor{115, 115, 115, 255};  // walls
constexpr SDL_Color kMarkerColor{255, 0, 0, 255};    // doors, markers, others
constexpr SDL_Color kFriendColor{0, 255, 0, 255};    // us and teammates
constexpr SDL_Color kObeliskColor{0, 0, 255, 255};   // color_blue
constexpr SDL_Color kWhite{255, 255, 255, 255};
// A map editor session's walls not yet published (noxsdl's own): pending in a clear red, drawn
// bolder; removed but still in the game in a dark red, dashed.
constexpr SDL_Color kPendingWallColor{255, 48, 48, 255};
constexpr SDL_Color kRemovedWallColor{150, 22, 22, 255};

/// The four diagonal neighbours a door looks for a wall in.
constexpr std::array<SDL_Point, 4> kDoorNeighbours{{{-1, -1}, {1, 1}, {-1, 1}, {1, -1}}};

std::uint32_t classesOf(const nox::things::ThingDb& things, std::uint16_t typeId)
{
    if (typeId == 0 || typeId > things.things.size()) {
        return 0;
    }
    return nox::things::thingClasses(things.things[typeId - 1u]);
}

std::uint32_t subclassesOf(const nox::things::ThingDb& things, std::uint16_t typeId)
{
    if (typeId == 0 || typeId > things.things.size()) {
        return 0;
    }
    return nox::things::thingSubclasses(things.things[typeId - 1u]);
}

bool isNamed(const nox::things::ThingDb& things, std::uint16_t typeId, std::string_view name)
{
    return typeId != 0 && typeId <= things.things.size() && things.things[typeId - 1u].name == name;
}

/// The team a net code belongs to (a player's, else a team's member list), 0 for none.
std::uint32_t teamOf(const nox::client::World& world, std::uint16_t netCode)
{
    if (const auto p = world.players().find(netCode); p != world.players().end()) {
        return p->second.team;
    }
    for (const auto& [id, team] : world.teams()) {
        if (team.members.contains(netCode)) {
            return id;
        }
    }
    return 0;
}

/// Draws in the game's pixels of the screen: `cam` maps them to output pixels.
struct Pen {
    SDL_Renderer* renderer;
    const Camera& cam;
    SDL_Color color{255, 255, 255, 255};
    float dash = 0;  ///< > 0: lines drawn as dashes this long with gaps as long (a removed wall)
    float thick = 0; ///< > 0: each line drawn again this far across (a pending wall, bolder)

    void line(float x1, float y1, float x2, float y2) const
    {
        if (thick > 0) {
            fxdraw::line(renderer, cam, {x1 + thick, y1}, {x2 + thick, y2}, color);
            fxdraw::line(renderer, cam, {x1, y1 + thick}, {x2, y2 + thick}, color);
        }
        if (dash <= 0) {
            fxdraw::line(renderer, cam, {x1, y1}, {x2, y2}, color);
            return;
        }
        const float dx = x2 - x1, dy = y2 - y1;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length <= 0) {
            return;
        }
        for (float from = 0; from < length; from += 2 * dash) {
            const float to = std::min(length, from + dash);
            fxdraw::line(renderer, cam, {x1 + dx * from / length, y1 + dy * from / length}, {x1 + dx * to / length, y1 + dy * to / length}, color);
        }
    }
    /// drawPointMB's shapes.
    void point(float x, float y, int size) const
    {
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
        fxdraw::point(renderer, cam, x, y, size);
    }
    /// A circle's outline (video_drawCircle).
    void circle(float x, float y, float r) const
    {
        constexpr int kSegments = 20;
        for (int i = 0; i < kSegments; ++i) {
            const float a = 2 * std::numbers::pi_v<float> * static_cast<float>(i) / kSegments;
            const float b = 2 * std::numbers::pi_v<float> * static_cast<float>(i + 1) / kSegments;
            line(x + r * std::cos(a), y + r * std::sin(a), x + r * std::cos(b), y + r * std::sin(b));
        }
    }
    /// The crown's outline.
    void crown(float x, float y) const
    {
        line(x - 4, y + 6, x - 6, y - 6);
        line(x - 6, y - 6, x - 2, y);
        line(x - 2, y, x, y - 6);
        line(x, y - 6, x + 2, y);
        line(x + 2, y, x + 6, y - 6);
        line(x + 6, y - 6, x + 4, y + 6);
        line(x + 4, y + 6, x - 4, y + 6);
    }
    /// The flag's outline: its pole and cloth.
    void flag(float x, float y) const
    {
        line(x - 2, y + 4, x - 2, y - 4);
        line(x - 2, y - 4, x + 2, y - 4);
        line(x + 2, y - 4, x + 2, y);
        line(x + 2, y, x - 2, y);
    }
    /// A wall's shape in its cell of side `a` from (x, y).
    // Shapes by wall direction: 0 and 1 the two diagonals, 2 both (a cross), 3-6 a diagonal with half of
    // the other (T joins), 7-10 two halves meeting at the centre (corners).
    void wall(float x, float y, int shape, float a) const
    {
        const float h = a / 2;
        switch (shape) {
        case 0:
            line(x, y + a, x + a, y);
            break;
        case 1:
            line(x, y, x + a, y + a);
            break;
        case 2:
            line(x, y + a, x + a, y);
            line(x, y, x + a, y + a);
            break;
        case 3:
            line(x, y, x + h, y + h);
            line(x, y + a, x + a, y);
            break;
        case 4:
            line(x, y, x + a, y + a);
            line(x + h, y + h, x + a, y);
            break;
        case 5:
            line(x, y + a, x + a, y);
            line(x + h, y + h, x + a, y + a);
            break;
        case 6:
            line(x, y, x + a, y + a);
            line(x, y + a, x + h, y + h);
            break;
        case 7:
            line(x, y, x + h, y + h);
            line(x + h, y + h, x + a, y);
            break;
        case 8:
            line(x + h, y + h, x + a, y + a);
            line(x + h, y + h, x + a, y);
            break;
        case 9:
            line(x + h, y + h, x + a, y + a);
            line(x + h, y + h, x, y + a);
            break;
        case 10:
            line(x, y, x + h, y + h);
            line(x, y + a, x + h, y + h);
            break;
        default:
            break;
        }
    }
};

} // namespace

Minimap::Minimap(const nox::things::ThingDb& things)
    : things_(things)
{
    for (std::size_t i = 0; i < things.walls.size(); ++i) {
        if (things.walls[i].name == "InvisibleWallSet") {
            invisibleWall_ = static_cast<int>(i);
        } else if (things.walls[i].name == "InvisibleBlockingWallSet") {
            invisibleBlockingWall_ = static_cast<int>(i);
        }
    }
}

void Minimap::zoom(bool in, bool out, std::uint64_t ms)
{
    if (in == out) {
        zoomMs_ = 0;
        return;
    }
    // A step a game frame (30 a second) while held.
    zoomMs_ += ms;
    constexpr std::uint64_t kFrameMs = 1000 / 30;
    for (; zoomMs_ >= kFrameMs; zoomMs_ -= kFrameMs) {
        zoom_ = std::clamp(zoom_ + (in ? -10 : 10), kZoomMin, kZoomMax);
    }
}

void Minimap::observe(const nox::client::World& world, const Camera& camera, const SightMap* sight)
{
    // marks the obelisks it draws (IMMOBILE with VISIBLE_OBELISK).
    for (const auto& [netCode, d] : world.drawables()) {
        if (seenMarkers_.contains(netCode) || !(classesOf(things_, d.typeId) & ObjectClass::Immobile) ||
            !(subclassesOf(things_, d.typeId) & kVisibleObelisk)) {
            continue;
        }
        const auto x = static_cast<float>(d.x);
        const auto y = static_cast<float>(d.y);
        const bool onScreen = x >= camera.left() && x <= camera.right() && y >= camera.top() && y <= camera.bottom();
        if (onScreen && (!sight || sight->visible(x, y))) {
            seenMarkers_.insert(netCode);
        }
    }
    // The ones the client loaded from the map that the server hasn't sent: drawn (and so marked)
    // where the map put them.
    for (const auto& [netCode, s] : statics_) {
        if (seenMarkers_.contains(netCode) || world.drawables().contains(netCode) ||
            !(subclassesOf(things_, s.typeId) & kVisibleObelisk)) {
            continue;
        }
        const bool onScreen = s.x >= camera.left() && s.x <= camera.right() && s.y >= camera.top() && s.y <= camera.bottom();
        if (onScreen && (!sight || sight->visible(s.x, s.y))) {
            seenMarkers_.insert(netCode);
        }
    }
}

void Minimap::mapLoaded(const nox::map::MapFile& map)
{
    seenMarkers_.clear();
    statics_.clear();
    std::map<std::uint16_t, std::uint16_t> types; // TOC id -> type id
    try {
        for (const auto& e : nox::map::decodeSection<nox::map::ObjectTOC>(map).entries) {
            if (const auto* t = things_.findThing(e.name)) {
                types[e.id] = static_cast<std::uint16_t>(t - things_.things.data() + 1);  // type ids count from 1
            }
        }
        // client_mapSpecialRWObjectData: every record, in order.
        for (const auto& r : nox::map::decodeSection<nox::map::ObjectData>(map).records) {
            const auto type = types.find(r.tocId);
            if (type == types.end()) {
                continue;
            }
            const std::uint32_t cls = classesOf(things_, type->second);
            // Kept: obelisks the client loads itself (IMMOBILE, SUBCLASS VISIBLE_OBELISK or INVISIBLE_OBELISK: 0x18).
            // (The client's CLIENT_PERSIST or IMMOBILE test before it is met by any IMMOBILE one.)
            if (!(cls & ObjectClass::Immobile) || !(subclassesOf(things_, type->second) & 0x18u)) {
                continue;
            }
            nox::map::ObjectXfer x;
            try {
                x = nox::map::ObjectXfer::parse(r.data);
            } catch (const std::exception&) {
                continue;
            }
            if (x.header.extent > 0x7FFFu) {
                continue; // (no net code reaches it)
            }
            const auto code = static_cast<std::uint16_t>(x.header.extent | 0x8000u);  // a static object's net code: its extent with bit 15 set
            statics_[code] = {type->second, x.header.x, x.header.y};
            // Its record: past version 0x3C, the mana word and the byte; 1 lists it.
            if (x.xferVersion > 0x3C && x.extra.size() >= 5 && x.extra[4] == 1) {
                seenMarkers_.insert(code);
            }
        }
    } catch (const std::exception&) {
        // no objects
    }
}

bool Minimap::marked(std::uint16_t netCode, const nox::client::World& world) const
{
    if (seenMarkers_.contains(netCode)) {
        return true;
    }
    const auto d = world.drawables().find(netCode);
    return d != world.drawables().end() && (d->second.monitored || d->second.interestKinds != 0);
}

void Minimap::draw(SDL_Renderer* renderer, const nox::client::World& world, const MapView& view, std::uint16_t us,
                   const Camera& camera, int w, int h, float scale)
{
    // Not drawn while hidden, or while we're blinded.
    const auto me = world.drawables().find(us);
    if (!shown_ || (me != world.drawables().end() && (me->second.buffs & kEnchantBlinded))) {
        return;
    }
    // The minimap group of the polygon the view's centre is in (the one it was in tested first),
    // 1 outside them all (currentPolygonByte130, as polygonDrawColor sets it).
    const std::uint8_t group = view.minimapGroup(camera.x, camera.y);

    // The frame, in the game's pixels of this screen (the HUD's scale).
    Camera cam;
    cam.zoom = scale;
    cam.viewWidth = w;
    cam.viewHeight = h;
    cam.x = static_cast<float>(w) / (2 * scale);
    cam.y = static_cast<float>(h) / (2 * scale);
    const float gameW = static_cast<float>(w) / scale;
    const float gameH = static_cast<float>(h) / scale;
    // A square a sixth of the screen's width, against its left edge, centred up and down.
    const float size = std::floor(gameW / 6);
    const float top = std::floor((gameH - size) / 2);

    SDL_BlendMode blend = SDL_BLENDMODE_NONE;
    SDL_GetRenderDrawBlendMode(renderer, &blend);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    const SDL_FRect box{0, top * scale, size * scale, size * scale};
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 128); // the half-dark fill
    SDL_RenderFillRect(renderer, &box);
    Pen pen{renderer, cam};
    // One of the edge's three lines, `inset` pixels outside the box: top, right and bottom (the left is the screen's edge).
    const auto outline = [&](float inset, std::uint8_t alpha) {
        pen.color = {0, 0, 0, alpha};
        const float x0 = -inset;
        const float y0 = top - inset;
        const float x1 = size + inset - 1;
        const float y1 = top + size + inset - 1;
        pen.line(x0, y0, x1, y0);
        pen.line(x1, y0, x1, y1);
        pen.line(x1, y1, x0, y1);
    };
    outline(1, 0x5A);
    outline(2, 0x3C);
    outline(3, 0x28);

    // What follows is clipped to the square (the previous clip put back at the end).
    SDL_Rect clipWas{};
    const bool clipped = SDL_RenderClipEnabled(renderer);
    SDL_GetRenderClipRect(renderer, &clipWas);
    const SDL_Rect clip{0, static_cast<int>(top * scale), static_cast<int>(size * scale), static_cast<int>(size * scale)};
    SDL_SetRenderClipRect(renderer, &clip);

    // World to map: `zoom` world units per 100 of the game's pixels, centred on the view.
    const float span = static_cast<float>(zoom_) * size / 100;
    const float left = camera.x - span / 2;
    const float upper = camera.y - span / 2;
    const float perUnit = 100.0f / static_cast<float>(zoom_);
    const auto mapX = [&](float x) { return (x - left) * perUnit; };
    const auto mapY = [&](float y) { return (y - upper) * perUnit + top; };
    const int dot = zoom_ < 1201 ? 4 : zoom_ < 1750 ? 3 : 2; // minimapDrawPoint
    const int bigDot = dot + 2;                             // (ourselves among the markers)

    // Walls seen so far, in their group.
    const bool reveal = (world.game().gameFlags & kRevealWalls) != 0;
    const float cellPixels = kCell * perUnit; // 0x8FC / zoom
    // The wall cells inside the map's square.
    const int firstX = static_cast<int>(std::floor(left / kCell));
    const int lastX = static_cast<int>(std::floor((left + span) / kCell));
    const int firstY = static_cast<int>(std::floor(upper / kCell));
    const int lastY = static_cast<int>(std::floor((upper + span) / kCell));
    // A map editor session's pending walls (MapView::setPending's plan, noxsdl's own) in their
    // look: pending red and bolder, a removal dark red and dashed; "As final" draws them as walls,
    // "Hidden" has no plan. Outside a session there's no plan and nothing changes.
    const bool pending = view.hasPending();
    const auto wallPen = [&](std::uint8_t look) {
        pen.color = look == 1 ? kPendingWallColor : look == 2 ? kRemovedWallColor : kWallColor;
        pen.dash = look == 2 ? std::max(1.5f, cellPixels / 4) : 0;
        pen.thick = look == 1 ? 0.75f : 0;
    };
    const auto drawWall = [&](const nox::map::Wall& wall, std::uint8_t look) {
        const float x = mapX(static_cast<float>(wall.x) * kCell);
        const float y = mapY(static_cast<float>(wall.y) * kCell);
        if (zoom_ > 2000) {  // zoomed far out: a dot per wall cell
            pen.point(x, y, look == 1 ? 2 : 1);
        } else {
            pen.wall(x, y, wall.direction & 0x7F, cellPixels);
        }
    };
    pen.color = kWallColor;
    for (const auto& wall : view.walls()) {
        if (wall.x < firstX || wall.x > lastX || wall.y < firstY || wall.y > lastY ||
            wall.material == invisibleWall_ || wall.material == invisibleBlockingWall_ ||
            (wall.minimapGroup != 0 && wall.minimapGroup != group) || !(reveal || view.wallSeen(wall))) {
            continue;
        }
        std::uint8_t look = 0;
        if (pending) {
            if (view.pendingHidesWall(wall.x, wall.y)) {
                continue; // (the plan has the cell: its own wall, or none)
            }
            look = view.pendingWallLook(wall.x, wall.y);
            wallPen(look);
        }
        drawWall(wall, look);
    }
    if (pending) {
        // The plan's own walls, once their cell has been in sight (fog keeps the rest off the map).
        for (const auto& pw : view.pendingWalls()) {
            const auto& wall = pw.wall;
            if (wall.x < firstX || wall.x > lastX || wall.y < firstY || wall.y > lastY ||
                wall.material == invisibleWall_ || wall.material == invisibleBlockingWall_ ||
                (wall.minimapGroup != 0 && wall.minimapGroup != group) || !(reveal || view.pendingWallSeen(wall.x, wall.y))) {
                continue;
            }
            wallPen(pw.look);
            drawWall(wall, pw.look);
        }
        wallPen(0);
    }

    // Doors, once a wall beside them has been seen.
    pen.color = kMarkerColor;
    for (const auto& door : view.doors()) {
        const int cx = static_cast<int>(door.x / kCell);
        const int cy = static_cast<int>(door.y / kCell);
        if (cx < firstX - 1 || cx > lastX + 1 || cy < firstY - 1 || cy > lastY + 1) {
            continue;
        }
        // (Every wall, drawn or not: the view's walls by cell.)
        const auto seenAt = [&](int x, int y) -> std::optional<bool> {
            const auto* found = view.wallByKey(MapView::wallKey(x, y));
            if (!found) {
                return std::nullopt;
            }
            const auto& wall = *found;
            return (wall.minimapGroup == 0 || wall.minimapGroup == group) && (reveal || view.wallSeen(wall));
        };
        for (const auto& n : kDoorNeighbours) {
            // The diagonal neighbour, else the two cells beside it that share the door's row or column.
            std::optional<bool> seen = seenAt(cx + n.x, cy + n.y);
            if (!seen) {
                seen = seenAt(cx + n.x, cy);
                if (!seen || !*seen) {
                    seen = seenAt(cx, cy + n.y);
                }
            }
            if (seen && *seen) {
                const SDL_Point v = doorLeaf(door.angle); // (its facing as a vector 32 units long)
                const float x = mapX(door.x);
                const float y = mapY(door.y);
                pen.line(x, y, x + static_cast<float>(v.x) * perUnit, y + static_cast<float>(v.y) * perUnit);
                break;
            }
        }
    }

    // Whether a drawable's where the map is showing: in a polygon of our group, or outside them all
    // while ours is 1.
    const auto inGroup = [&](float x, float y) {
        const auto g = view.minimapGroupAt(x, y);
        return g ? *g == group : group == 1;
    };
    const std::uint32_t ourTeam = teamOf(world, us);
    const bool haveTeams = !world.teams().empty();
    const bool ctf = (world.game().gameFlags & kCaptureTheFlag) != 0;
    const bool crownCarried = std::ranges::any_of(world.drawables(), [&](const auto& kv) {
        return (classesOf(things_, kv.second.typeId) & ObjectClass::Player) && (kv.second.buffs & kEnchantCrown);
    });

    // The markers (the client's "interesting" list).
    if (markerTypes_.size() != things_.things.size() + 1) {
        markerTypes_.assign(things_.things.size() + 1, 0);
        for (std::size_t id = 1; id < markerTypes_.size(); ++id) {
            const auto typeId = static_cast<std::uint16_t>(id);
            markerTypes_[id] = isNamed(things_, typeId, "Crown") ? 1 : isNamed(things_, typeId, "GameBall") ? 2 : 0;
        }
    }
    for (const auto& [netCode, d] : world.drawables()) {
        const std::uint32_t cls = classesOf(things_, d.typeId);
        const std::uint8_t marker = d.typeId < markerTypes_.size() ? markerTypes_[d.typeId] : 0;
        const bool crown = marker == 1;
        const bool ball = marker == 2;
        const bool obelisk = seenMarkers_.contains(netCode);
        if (!(crown || ball || (cls & ObjectClass::Flag) || obelisk || d.monitored || d.interestKinds != 0)) {
            continue;
        }
        const auto x0 = static_cast<float>(d.x);
        const auto y0 = static_cast<float>(d.y);
        if (!inGroup(x0, y0)) {
            continue;
        }
        const float x = mapX(x0);
        const float y = mapY(y0);
        // (cliDrawMinimap: blue for IMMOBILE with VISIBLE_OBELISK, else the markers' red.)
        pen.color = (cls & ObjectClass::Immobile) && (subclassesOf(things_, d.typeId) & kVisibleObelisk) ? kObeliskColor : kMarkerColor;
        if (crown) {
            if (!haveTeams && crownCarried) {
                continue; // its carrier shows it
            }
            pen.color = teamColor(world, teamOf(world, netCode)).value_or(kFriendColor);
            pen.crown(x, y);
        } else if (ball) {
            pen.color = teamColor(world, teamOf(world, netCode)).value_or(kWhite);
            pen.circle(x, y, 3);
        } else if (cls & ObjectClass::Flag) {
            if (d.enabled) {
                // White, or the colour of the team whose id is the flag's material colour
                // (the colour's index taken as a team id by Team_GetByID: teams 1 and 2 are red
                // and blue as Go makes them).
                std::uint8_t colour = 0;
                if (modifiers_ && d.modifiers) {
                    if (const auto* m = modifiers_->modifier((*d.modifiers)[1])) {
                        colour = flagMaterialColor(m->name);
                    }
                }
                pen.color = teamColor(world, colour).value_or(kWhite);
                pen.flag(x, y);
            }
        } else if (cls & ObjectClass::Player) {
            if (ctf) {
                const auto p = world.players().find(netCode);
                if (p != world.players().end() && (p->second.weaponEquip & kCarriesFlag)) {
                    // The other team's flag.
                    const std::uint32_t team = teamOf(world, netCode);
                    pen.color = team ? teamColor(world, team == 1 ? 2 : 1).value_or(kWhite) : kWhite;
                    pen.flag(x, y);
                }
            } else {
                pen.point(x, y, netCode == us ? bigDot : dot);
            }
        } else {
            pen.point(x, y, dot);
        }
    }
    // The obelisks listed by the map that the server hasn't sent yet: the exe's client loaded
    // them itself, so they're drawn where the map puts them.
    for (const auto& [netCode, s] : statics_) {
        if (!seenMarkers_.contains(netCode) || world.drawables().contains(netCode) || !inGroup(s.x, s.y)) {
            continue;
        }
        pen.color = (subclassesOf(things_, s.typeId) & kVisibleObelisk) ? kObeliskColor : kMarkerColor;
        pen.point(mapX(s.x), mapY(s.y), dot);
    }

    // The players we may see: ourselves, teammates, a crown's carrier; everyone when observing.
    const auto self = world.players().find(us);
    const bool observing = self != world.players().end() && (self->second.status & 1);  // status bit 0x1: observing
    for (const auto& [netCode, d] : world.drawables()) {
        if (!(classesOf(things_, d.typeId) & ObjectClass::Player)) {
            continue;
        }
        const auto p = world.players().find(netCode);
        if (p == world.players().end() || (p->second.status & 1)) {
            continue; // an observer isn't on the map
        }
        const bool carriesCrown = (d.buffs & kEnchantCrown) != 0;
        const std::uint32_t team = p->second.team;
        const bool ally = ourTeam != 0 && team != 0 && ourTeam == team;
        if (!(ally || carriesCrown || netCode == us || observing)) {
            continue;
        }
        const auto x0 = static_cast<float>(d.x);
        const auto y0 = static_cast<float>(d.y);
        if (!inGroup(x0, y0)) {
            continue;
        }
        pen.color = (netCode == us || ally) ? kFriendColor : kMarkerColor;
        if (carriesCrown) {
            pen.color = teamColor(world, team).value_or(pen.color);
            pen.crown(mapX(x0), mapY(y0));
        } else {
            pen.point(mapX(x0), mapY(y0), dot);
        }
    }

    SDL_SetRenderClipRect(renderer, clipped ? &clipWas : nullptr);
    SDL_SetRenderDrawBlendMode(renderer, blend);
}

} // namespace noxsdl
