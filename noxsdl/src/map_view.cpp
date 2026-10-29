// MapView: loads a .map's floor, walls, objects, lights and ambient sounds, and draws the floor and
// the sorted scene (map_view.hpp describes the tile and wall layout rules).
#include "map_view.hpp"

#include "cursor.hpp"
#include "draw_scale.hpp"

#include "pending_look.hpp"
#include "player_art.hpp"

#include "noxcore/audio/sound_names.hpp"
#include "noxcore/io/byte_reader.hpp"
#include "noxcore/map/objects.hpp"
#include "noxcore/map/xfer.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/video/tile_edges.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <map>

namespace noxsdl {

namespace {

constexpr float kCell = 46;
constexpr float kHalf = 23;
// Tile top-left within its cell (tileDrawImpl: 46i - 57 + 69, 46j - 57 + 46 for
// the odd tile; the even tile swaps the axes).
constexpr float kOddX = 12;
constexpr float kOddY = -11;
constexpr int kTilePixels = 46;
constexpr float kWallCell = 23; ///< a wall cell's size, world units

/// Whether the tile cell with its corner at (x, y) reaches into the camera's view, widened by
/// `margin` each way.
bool cellInView(float x, float y, const Camera& camera, float margin = 0)
{
    return !(x + kCell < camera.left() - margin || x > camera.right() + margin || y + kCell < camera.top() - margin ||
             y > camera.bottom() + margin);
}

/// A wall kind's images for direction `dir`, variant `variant` (a windowed piece's variant past
/// its images: the first, as a few maps name one); nullptr where it has none (nothing drawn).
const nox::things::WallVariant* wallImages(const nox::things::WallDef& kind, std::size_t dir, std::size_t variant)
{
    const auto& dirs = kind.directions;
    if (dir >= 11 && dir <= 12 && dir < dirs.size() && variant >= dirs[dir].size()) {
        variant = 0;
    }
    if (dir >= dirs.size() || variant >= dirs[dir].size()) {
        return nullptr;
    }
    return &dirs[dir][variant];
}

// The cache key for a tile and its edge stack: identical stacks share one composited texture.
std::string edgeKey(std::uint8_t floor, std::int16_t variant, const std::vector<nox::map::FloorEdge>& edges)
{
    std::string key = std::format("{}.{}", floor, variant);
    for (const auto& e : edges) {
        key += std::format("|{}.{}.{}.{}", e.floor, e.variant, e.edge, e.direction);
    }
    return key;
}

/// A full-grid tile position's key (tileIndex_, the pending tiles').
std::int32_t gridKey(int gx, int gy)
{
    return static_cast<std::int32_t>(((gy + 1) << 9) | (gx & 0x1FF));
}

/// A tile's full-grid key from its corner: an odd tile's lies at (46x + 12, 46y - 11), an even
/// one's at (46x - 11, 46y + 12).
std::int32_t gridKeyAt(float tx, float ty)
{
    const int x = static_cast<int>(std::lround(tx)), y = static_cast<int>(std::lround(ty));
    const bool odd = ((x % 46) + 46) % 46 == 12;
    const int px = odd ? (x - 12) / 46 : (x + 11) / 46, py = odd ? (y + 11) / 46 : (y - 12) / 46;
    return gridKey(odd ? 2 * px + 1 : 2 * px, odd ? 2 * py - 1 : 2 * py);
}

/// Where the full-grid tile (gx, gy)'s corner lies (the odd one of FloorMap pair ((gx - 1) / 2,
/// (gy + 1) / 2) for odd gx, else the even one of (gx / 2, gy / 2)).
SDL_FPoint tileCorner(int gx, int gy)
{
    const bool odd = (gx & 1) != 0;
    const float cx = static_cast<float>(odd ? (gx - 1) / 2 : gx / 2) * kCell;
    const float cy = static_cast<float>(odd ? (gy + 1) / 2 : gy / 2) * kCell;
    return odd ? SDL_FPoint{cx + kOddX, cy + kOddY} : SDL_FPoint{cx + kOddY, cy + kOddX};
}

/// An object's flags as the client has them once loaded: a new drawable starts enabled
/// (Cli_CreateDrawableAt sets 0x1000000) and only a header with details replaces that
/// (spriteLoadFromMap). Most maps' flames and torches have no details.
std::uint32_t objectFlags(const nox::map::ObjectHeader& header)
{
    return header.details ? header.details->flags : kObjectEnabled;
}

} // namespace

MapView::MapView(const nox::map::MapFile& map, const nox::things::ThingDb& things, ThingArt& art,
                 const nox::things::ModifierDb& modifiers, ObjectSet objectSet)
    : things_(things)
    , art_(&art)
    , modifiers_(&modifiers)
    , objectSet_(objectSet)
{
    static std::uint64_t made = 0;
    serial_ = ++made;
    const auto floor = nox::map::decodeSection<nox::map::FloorMap>(map);
    double sx = 0;
    double sy = 0;
    const auto add = [&](float x, float y, const nox::map::FloorTile& t) {
        Tile tile{x, y, t.image, t.variant, t.edges, {}};
        if (!tile.edges.empty()) {
            tile.key = edgeKey(tile.floor, tile.variant, tile.edges);
        }
        tiles_.push_back(std::move(tile));
    };
    // Each cell's odd and even tile at their corners; the camera starts at the mean of the cells' centres.
    // (The pairs as the game reads them: a custom map's pair stored twice shows its later tiles.)
    const auto pairs = floor.readPairs();
    for (const auto& pair : pairs) {
        const float cx = pair.x * kCell;
        const float cy = pair.y * kCell;
        if (pair.odd) {
            add(cx + kOddX, cy + kOddY, *pair.odd);
        }
        if (pair.even) {
            add(cx + kOddY, cy + kOddX, *pair.even);
        }
        sx += cx + kHalf;
        sy += cy + kHalf;
    }
    if (!pairs.empty()) {
        centre_ = {static_cast<float>(sx / pairs.size()), static_cast<float>(sy / pairs.size())};
    }

    // Walls, then which are windows, which can be broken, and which are secret (each with "starts open", flag 8).
    walls_ = nox::map::decodeSection<nox::map::WallMap>(map).walls;
    try {
        // WindowWalls (server_mapRWWindowWalls) flags walls as windows (0x40);
        // version 1 maps also reset their variant.
        const auto win = nox::map::decodeSection<nox::map::WindowWalls>(map);
        for (const auto& p : win.walls) {
            windows_.insert(wallKey(p.x, p.y));
        }
        if (win.version < 2) {
            for (auto& w : walls_) {
                if (windows_.contains(wallKey(w.x, w.y))) {
                    w.variant = 0;
                }
            }
        }
    } catch (const nox::FormatError&) {
        // No windows.
    }
    mapWalls_ = walls_;
    try {
        for (const auto& p : nox::map::decodeSection<nox::map::DestructableWalls>(map).walls) {
            breakable_.push_back(wallKey(p.x, p.y));
        }
    } catch (const nox::FormatError&) {
        // None.
    }
    try {
        for (const auto& s : nox::map::decodeSection<nox::map::SecretWalls>(map).walls) {
            secret_.emplace_back(wallKey(s.pos.x, s.pos.y), (s.flags & 8) != 0);
        }
    } catch (const nox::FormatError&) {
        // None.
    }
    // The walls as they stand at load: nothing broken yet, secret walls as the map has them.
    setWallState({}, {}, {}, {});
    // The ambient light: the map's AmbientData, then the polygons' own ambient and minimap group.
    {
        std::optional<nox::map::AmbientData> ambient;
        std::optional<nox::map::Polygons> polygons;
        try {
            ambient = nox::map::decodeSection<nox::map::AmbientData>(map);
        } catch (const nox::FormatError&) {
            // No AmbientData: the level switch's 25s stand (mapSwitchLevel, and a client's world reset).
        }
        try {
            polygons = nox::map::decodeSection<nox::map::Polygons>(map);
        } catch (const nox::FormatError&) {
            // No polygons: the map's ambient everywhere.
        }
        ambient_.load(ambient ? &*ambient : nullptr, polygons ? &*polygons : nullptr);
    }

    loadObjects(map);
    // The quest doors' lock images (drawScene), looked up once.
    constexpr std::array<std::string_view, 4> kLockNames = {"DoorLockSilverSW", "DoorLockSilverSE", "DoorLockGoldSW",
                                                            "DoorLockGoldSE"};
    for (std::size_t i = 0; i < kLockNames.size(); ++i) {
        const auto def = std::ranges::find(things_.images, kLockNames[i], &nox::things::ImageDef::name);
        lockImages_[i] = def != things_.images.end() && def->image ? &*def->image : nullptr;
    }
}

void MapView::setObjects(const nox::map::MapFile& map)
{
    objects_.clear();
    objectIndex_.clear();
    lights_.clear();
    sounds_.clear();
    doors_.clear();
    loadObjects(map);
    if (!pendingHidePlan_.empty()) {
        matchPendingObjects(); // (the indexes changed)
    }
}

void MapView::loadObjects(const nox::map::MapFile& map)
{
    const nox::things::ThingDb& things = things_;
    ThingArt& art = *art_;
    const nox::things::ModifierDb& modifiers = *modifiers_;
    const ObjectSet objectSet = objectSet_;
    // Top-level objects only: inventory records follow their owner and aren't drawn.
    const auto toc = nox::map::decodeSection<nox::map::ObjectTOC>(map);
    std::map<std::uint16_t, std::string_view> typeNames;
    for (const auto& e : toc.entries) {
        typeNames[e.id] = e.name;
    }
    const auto records = nox::map::decodeSection<nox::map::ObjectData>(map).records;
    std::vector<nox::map::ObjectXfer> xfers;
    xfers.reserve(records.size());
    for (const auto& r : records) {
        xfers.push_back(nox::map::ObjectXfer::parse(r.data));
    }
    // The static drawables (the ones the client loads itself) by extent, where a light can aim
    // (Cli_GetStaticDrawableByNetCode: class & (CLIENT_PERSIST | IMMOBILE)).
    std::map<std::uint32_t, std::array<float, 2>> statics;
    // Each top-level object: an ambient sound if its type has an AUDIOLOOP, a light if it is CLASS LIGHT,
    // then its picture if it is drawn at all.
    for (const auto& root : nox::map::buildObjectTree(xfers)) {
        const auto name = typeNames.find(records[root.record].tocId);
        if (name == typeNames.end()) {
            continue;
        }
        if (const auto* def = things.findThing(name->second)) {
            constexpr std::uint32_t kStatic = nox::things::ObjectClass::ClientPersist | nox::things::ObjectClass::Immobile;
            if (nox::things::thingClasses(*def) & kStatic) {
                const auto& h = xfers[root.record].header;
                statics.try_emplace(h.extent, std::array<float, 2>{h.x, h.y});
            }
            if (const auto loop = def->attribute("AUDIOLOOP")) {
                const auto& h = xfers[root.record].header;
                constexpr std::uint32_t kLoads = nox::things::ObjectClass::ClientPersist | nox::things::ObjectClass::Immobile;
                sounds_.push_back({h.extent, h.x, h.y, std::string(*loop), nox::audio::soundId(*loop),
                                   (nox::things::thingClasses(*def) & kLoads) != 0,
                                   (objectFlags(h) & kObjectEnabled) != 0});
            }
        }
        const TypeArt* a = art.get(name->second);
        if (!a) {
            continue;
        }
        const auto& header = xfers[root.record].header;
        const std::uint32_t flags = objectFlags(header);
        // The vanilla client loads these itself (client_mapSpecialRWObjectData); the server sends the rest.
        constexpr std::uint32_t kClientLoaded = nox::things::ObjectClass::ClientPersist | nox::things::ObjectClass::Immobile;
        const bool clientLoaded = (a->classes & kClientLoaded) != 0;

        if (a->classes & nox::things::ObjectClass::Light) {
            addMapLight(*a, xfers[root.record], records[root.record].data.size(), clientLoaded);
        }

        const bool plate = a->drawFn == DrawFn::PressurePlate;
        // The map editor's NPCs, maidens and items: drawn from the record as the game will draw them
        // (its colours, an NPC's worn things, an item's modifiers: player_art.hpp's recordLook).
        std::optional<RecordLook> look;
        if (objectSet == ObjectSet::Editor && (a->drawFn == DrawFn::NPC || a->drawFn == DrawFn::Maiden || a->itemColours != ItemColours::None)) {
            try {
                const auto& x = xfers[root.record];
                const auto data = nox::map::xfer::decode(a->xfer, x.xferVersion, x.extra, {a->classes, a->subclasses}, x.extraOffset());
                std::vector<nox::map::xfer::Data> itemData;
                itemData.reserve(root.inventory.size());
                std::vector<RecordItem> items;
                for (const auto& child : root.inventory) {
                    const auto childName = typeNames.find(records[child.record].tocId);
                    const TypeArt* ca = childName == typeNames.end() ? nullptr : art.get(childName->second);
                    if (!ca) {
                        continue;
                    }
                    const auto& cx = xfers[child.record];
                    try {
                        itemData.push_back(nox::map::xfer::decode(ca->xfer, cx.xferVersion, cx.extra, {ca->classes, ca->subclasses},
                                                                  cx.extraOffset()));
                    } catch (const std::exception&) {
                        itemData.emplace_back();
                    }
                    items.push_back({childName->second, (objectFlags(cx.header) & 0x100u) != 0, nullptr}); // (0x100: EQUIPPED)
                }
                for (std::size_t i = 0; i < items.size(); ++i) {
                    const auto& d = itemData[i];
                    namespace xf = nox::map::xfer;
                    if (const auto* w = std::get_if<xf::Weapon>(&d)) items[i].modifiers = &w->modifiers;
                    else if (const auto* ar = std::get_if<xf::Armor>(&d)) items[i].modifiers = &ar->modifiers;
                    else if (const auto* am = std::get_if<xf::Ammo>(&d)) items[i].modifiers = &am->modifiers;
                }
                look = recordLook(*a, data, items, things, modifiers);
            } catch (const std::exception&) {
                // (as it is)
            }
        }
        std::vector<LookLayer> npcLayers;
        if (look && look->kind == RecordLook::Kind::Npc) {
            npcLayers = lookLayers(*a, *look, std::nullopt, art, modifiers); // (facing south, as the map's creatures stand)
        }
        // Nothing to draw: no still picture (pressure plates and the editor's NPCs aside), or an
        // editor-only thing.
        if (objectSet == ObjectSet::None || (!a->still && !plate && npcLayers.empty()) || a->editorOnly) {
            continue;
        }
        if (objectSet == ObjectSet::ClientLoaded && !clientLoaded) {
            continue;
        }
        Object object{header.x, header.y, header.extent, a, std::nullopt};
        object.enabled = (flags & kObjectEnabled) != 0; // the server may turn it on later
        // Its state as the map has it (spriteLoadFromMap: the drawable's flags & 0xFFFFFFA1 |
        // the record's, all of it; those flags start as its type's, taken as 0): a saved game's
        // opened chest, a broken crate, as they were.
        object.xStatus = header.details ? header.details->statusFlags : 0u;
        if (plate) {
            // Its box and two outline colours; grey 90 and 10 before xfer 0x29.
            PlateOutline outline;
            try {
                const auto& x = xfers[root.record];
                const auto data = nox::map::xfer::decode(a->xfer, x.xferVersion, x.extra, {a->classes, a->subclasses},
                                                         x.extraOffset());
                if (const auto* t = std::get_if<nox::map::xfer::Trigger>(&data)) {
                    outline.width = static_cast<float>(t->sizeX);
                    outline.height = static_cast<float>(t->sizeY);
                    if (x.xferVersion > 0x28) {
                        outline.colors = {t->edgeColor, t->backColor};
                    }
                }
            } catch (const std::exception&) {
                continue;
            }
            object.plate = outline;
        }
        if (look) {
            for (const auto& l : npcLayers) {
                object.layers.emplace_back(l.image, l.colours);
            }
            if (look->kind != RecordLook::Kind::Npc && a->still) {
                if (const auto one = lookLayers(*a, *look, *a->still, art, modifiers); !one.empty()) {
                    object.colours = one.front().colours;
                }
            }
        } else if (a->itemColours != ItemColours::None) {
            // Weapon and Armor xfers name the item's four modifiers.
            std::optional<std::array<std::uint8_t, 4>> ids;
            try {
                const auto& x = xfers[root.record];
                const auto data = nox::map::xfer::decode(a->xfer, x.xferVersion, x.extra, {a->classes, a->subclasses},
                                                         x.extraOffset());
                const nox::map::xfer::Modifiers* names = nullptr;
                if (const auto* w = std::get_if<nox::map::xfer::Weapon>(&data)) {
                    names = &w->modifiers;
                } else if (const auto* ar = std::get_if<nox::map::xfer::Armor>(&data)) {
                    names = &ar->modifiers;
                }
                if (names) {
                    ids.emplace();
                    for (std::size_t i = 0; i < 4; ++i) {
                        const auto* m = (*names)[i].empty() ? nullptr : modifiers.findModifier((*names)[i]);
                        (*ids)[i] = m ? m->id : std::uint8_t{0xFF};  // 0xFF: no modifier in that slot
                    }
                }
            } catch (const std::exception&) {
                // Colours from the definition only.
            }
            object.colours = itemMaterials(*a, ids ? &*ids : nullptr, modifiers);
        }
        if (a->xfer == "DoorXfer") {
            // DoorDraw shows image[angle]; the map gives the angle it starts at.
            try {
                const auto& x = xfers[root.record];
                const auto data = nox::map::xfer::decode(a->xfer, x.xferVersion, x.extra, {a->classes, a->subclasses},
                                                         x.extraOffset());
                if (const auto* door = std::get_if<nox::map::xfer::Door>(&data)) {
                    object.doorAngle = static_cast<std::uint8_t>(door->currentDirection & 31);
                    // (its lock, and the locked mark set with one)
                    object.lock = static_cast<std::uint8_t>(door->lock);
                    object.lockMark = object.lock != 0;
                }
            } catch (const std::exception&) {
                object.doorAngle = 0;
            }
        }
        objectIndex_[object.extent] = objects_.size();
        objects_.push_back(std::move(object));
    }
    // A light that aims (anim flag 0x40) turns toward its target, if that is a static drawable.
    for (auto& l : lights_) {
        if (l.cone && (l.cone->flags & 0x40) != 0) {
            if (const auto it = statics.find(l.cone->target); it != statics.end()) {
                l.cone->targetAt = it->second;
            }
        }
    }
    listDoors();
    indexObjects();
}

void MapView::indexObjects()
{
    namespace oc = nox::things::ObjectClass;
    constexpr std::uint32_t kUseable = 0x80; // IMMOBILE's SUBCLASS USEABLE
    doorObjects_.clear();
    shadowObjects_.clear();
    useableObjects_.clear();
    for (std::size_t i = 0; i < objects_.size(); ++i) {
        const Object& o = objects_[i];
        if (o.doorAngle) {
            doorObjects_.push_back(i);
        }
        if (o.art->shadow) {
            shadowObjects_.push_back(i);
        }
        if ((o.art->classes & oc::Immobile) && (o.art->subclasses & kUseable)) {
            useableObjects_.push_back(i);
        }
    }
    lightByExtent_.clear();
    for (std::size_t i = 0; i < lights_.size(); ++i) {
        lightByExtent_.try_emplace(lights_[i].extent, i); // (the first with an extent)
    }
    soundsByExtent_.clear();
    for (std::size_t i = 0; i < sounds_.size(); ++i) {
        soundsByExtent_[sounds_[i].extent].push_back(i);
    }
}

// A light the map places (CLASS LIGHT objects); InvisibleLightXfer lights (ColorLight and friends) carry
// their own colour, intensity and animation keys in the record.
void MapView::addMapLight(const TypeArt& art, const nox::map::ObjectXfer& xfer, std::size_t /*recordSize*/,
                          bool clientLoaded)
{
    const auto& header = xfer.header;
    const std::uint32_t flags = objectFlags(header);
    LightSource light;
    light.x = header.x;
    light.y = header.y;
    light.intensity = art.lightIntensity;
    light.color = art.lightColor;
    light.flicker = art.flicker;
    light.cone = art.lightCone; // (its type's cone; an InvisibleLight's own block replaces it)
    light.seed = header.extent;
    MapLight entry{header.extent, {}, clientLoaded};
    // A light shines only while its drawable is enabled (cliLight16), as the map
    // has it until the server says otherwise.
    entry.enabled = (flags & kObjectEnabled) != 0;
    if (art.xfer == "InvisibleLightXfer") {
        // ColorLight and friends: the map stores the whole light block (XFerInvLight); the client
        // loads it as is (colorLightClientLoad).
        try {
            const auto data = nox::map::xfer::decode(art.xfer, xfer.xferVersion, xfer.extra,
                                                     {art.classes, art.subclasses}, xfer.extraOffset());
            if (const auto* inv = std::get_if<nox::map::xfer::InvisibleLight>(&data)) {
                light.intensity = inv->intensity;
                for (std::size_t i = 0; i < 3; ++i) {
                    light.color[i] = static_cast<std::uint8_t>(std::clamp(inv->color[i], 0, 255));
                }
                light.anti = inv->isAnti != 0;
                // The client counts the non-zero keys of each kind as it loads them (colorLightClientLoad)
                // and steps through that many from the first.
                std::size_t colors = 0;
                std::size_t intensities = 0;
                for (std::size_t i = 0; i < 16; ++i) {
                    const auto& c = inv->colorKeys[i];
                    colors += (c[0] | c[1] | c[2]) != 0 ? 1 : 0;
                    intensities += inv->intensityKeys[i] != 0 ? 1 : 0;
                }
                entry.colorKeys.assign(inv->colorKeys.begin(), inv->colorKeys.begin() + static_cast<std::ptrdiff_t>(colors));
                entry.intensityKeys.assign(inv->intensityKeys.begin(),
                                           inv->intensityKeys.begin() + static_cast<std::ptrdiff_t>(intensities));
                entry.colorStep = inv->colorStep;
                entry.intensityStep = inv->intensityStep;
                entry.animFlags = inv->animFlags;
                entry.cone = lightcone::coneAnim(*inv); // (its target is found once all objects are in)
            }
        } catch (const std::exception&) {
            return;
        }
    }
    // A negative intensity is an anti-light: it darkens.
    if (light.intensity < 0) {
        light.intensity = -light.intensity;
        light.anti = true;
    }
    // Only lights brighter than 1, or animated ones, are kept.
    if (light.intensity > 1 || entry.intensityKeys.size() > 1) {
        entry.light = light;
        lights_.push_back(std::move(entry));
    }
}

LightSource MapView::MapLight::at(std::uint32_t frame) const
{
    using lightcone::keySegment;
    LightSource out = light;
    if (cone) {
        out.cone = lightcone::coneAt(*cone, light.x, light.y, frame);
    }
    // Between two keys as the game works it (lightcone::keyWhole / keyFloat: the x87's chopped
    // steps): each colour component made whole, the intensity a float.
    if (const auto s = keySegment(colorKeys.size(), colorStep, (animFlags & 1) != 0, frame)) {
        for (std::size_t c = 0; c < 3; ++c) {
            const int v = lightcone::keyWhole(colorKeys[s->from][c], colorKeys[s->to][c], colorStep, s->frames);
            out.color[c] = static_cast<std::uint8_t>(std::clamp(v, 0, 255));
        }
    }
    if (const auto s = keySegment(intensityKeys.size(), intensityStep, (animFlags & 4) != 0, frame)) {
        out.intensity = lightcone::keyFloat(intensityKeys[s->from], intensityKeys[s->to], intensityStep, s->frames);
    }
    // What the server set overrides the animation.
    if (color) {
        out.color = *color;
    }
    if (intensity) {
        out.intensity = std::min(std::abs(*intensity), 63.0f); // Draw_LightSetIntensity's clamp
        out.anti = *intensity < 0;
    }
    return out;
}

void MapView::setStatics(const std::map<std::uint16_t, nox::client::StaticObjectState>& statics)
{
    for (const auto& [extent, s] : statics) {
        const auto it = objectIndex_.find(extent);
        Object* o = it == objectIndex_.end() ? nullptr : &objects_[it->second];
        if (o) {
            o->enabled = s.enabled.value_or(o->enabled);
            o->frame = s.frame ? s.frame : o->frame;
            o->lockMark = s.lockMark.value_or(o->lockMark);
            if (s.xStatus && *s.xStatus != o->xStatus) {
                o->xStatus = *s.xStatus;
                o->xStatusSince = time_;
            }
        }
        if (s.enabled) {
            if (const auto sounds = soundsByExtent_.find(extent); sounds != soundsByExtent_.end()) {
                for (const std::size_t i : sounds->second) {
                    sounds_[i].enabled = *s.enabled;
                }
            }
        }
        // (A pressed pressure plate gets a light too, but in its type's light colour, which is
        // black: the game sets no default. So only the map's lights change here: the first with
        // the extent.)
        if (const auto found = lightByExtent_.find(extent); found != lightByExtent_.end()) {
            MapLight* light = &lights_[found->second];
            light->enabled = s.enabled.value_or(light->enabled);
            light->intensity = s.lightIntensity ? s.lightIntensity : light->intensity;
            if (s.lightColor) {
                light->color = Rgb{(*s.lightColor)[0], (*s.lightColor)[1], (*s.lightColor)[2]};
            }
        }
    }
}

void MapView::drawPlate(SDL_Renderer* renderer, const Camera& camera, const Object& o) const
{
    const PlateOutline& p = *o.plate;
    const auto black = [](const Rgb& c) { return c[0] == 0 && c[1] == 0 && c[2] == 0; };
    if (black(p.colors[0]) && black(p.colors[1])) {
        return;
    }
    // The box's corners (0.35355339 = sqrt(2) / 4): top, left, right, bottom.
    constexpr float k = 0.35355339f;
    const float w = p.width * k;
    const float h = p.height * k;
    const SDL_FPoint top{h - w, -w - h};
    const SDL_FPoint left{-w - h, h - w};
    const SDL_FPoint right{h + w, w - h};
    const SDL_FPoint bottom{w - h, h + w};
    const std::array<std::pair<SDL_FPoint, SDL_FPoint>, 4> edges{
        std::pair{top, right}, std::pair{bottom, right}, std::pair{top, left}, std::pair{bottom, left}};
    const float thick = std::max(1.0f, camera.zoom); // one game pixel
    for (int pass = 0; pass < 2; ++pass) {
        const Rgb& c = p.colors[static_cast<std::size_t>(pass)];
        const SDL_FColor color{c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, 1.0f};
        for (const auto& [a, b] : edges) {
            // Screen space; the second pass a pixel to the right.
            const float ax = (o.x + a.x + static_cast<float>(pass) - camera.left()) * camera.zoom;
            const float ay = (o.y + a.y - camera.top()) * camera.zoom;
            const float bx = (o.x + b.x + static_cast<float>(pass) - camera.left()) * camera.zoom;
            const float by = (o.y + b.y - camera.top()) * camera.zoom;
            const float len = std::hypot(bx - ax, by - ay);
            if (len <= 0) {
                continue;
            }
            // Each edge a quad one game pixel thick, offset along the edge's normal.
            const float nx = -(by - ay) / len * thick / 2;
            const float ny = (bx - ax) / len * thick / 2;
            const SDL_Vertex v[4] = {{{ax + nx, ay + ny}, color, {}},
                                     {{bx + nx, by + ny}, color, {}},
                                     {{bx - nx, by - ny}, color, {}},
                                     {{ax - nx, ay - ny}, color, {}}};
            const int index[6] = {0, 1, 2, 0, 2, 3};
            SDL_RenderGeometry(renderer, nullptr, v, 4, index, 6);
        }
    }
}

void MapView::setDoorAngles(const std::map<std::uint16_t, std::uint8_t>& angles)
{
    bool turned = false;
    for (const std::size_t i : doorObjects_) {
        Object& o = objects_[i];
        // Static objects' net codes are their extent with bit 15 set.
        if (const auto it = angles.find(static_cast<std::uint16_t>((o.extent & 0x7FFF) | 0x8000)); it != angles.end()) {
            const auto angle = static_cast<std::uint8_t>(it->second & 31);
            turned = turned || *o.doorAngle != angle;
            o.doorAngle = angle;
        }
    }
    if (turned) {
        listDoors();
    }
}

void MapView::sightBlockers(std::vector<SightMap::Blocker>& out, const std::set<std::uint32_t>* liveCodes) const
{
    constexpr std::uint32_t kGate = 4; // the GATE subclass: bars you see through
    for (const std::size_t i : shadowObjects_) {
        const Object& o = objects_[i];
        if (liveCodes && liveCodes->contains(o.extent)) {
            continue;
        }
        const std::uint32_t key = SightMap::mapKey(i);
        if (o.art->classes & nox::things::ObjectClass::Door) {
            if (!(o.art->subclasses & kGate)) {
                const SDL_Point leaf = doorLeaf(o.doorAngle.value_or(0));
                out.push_back({o.x, o.y, o.x + static_cast<float>(leaf.x), o.y + static_cast<float>(leaf.y), 0, 0, 0, key});
            }
        } else if (o.art->extentBox) {
            out.push_back({o.x, o.y, o.x, o.y, 0, o.art->extentWidth, o.art->extentHeight, key});
        } else if (o.art->extentRadius > 0) {
            out.push_back({o.x, o.y, o.x, o.y, o.art->extentRadius, 0, 0, key});
        }
    }
}

bool MapView::objectSeen(std::size_t index, const SightMap* sight) const
{
    // (The map's objects stand on the floor: height 0.)
    if (!sight || index >= objects_.size()) {
        return true;
    }
    const Object& o = objects_[index];
    return sight->drawableSeen(*o.art, SightMap::mapKey(index), o.x, o.y, 0, o.doorAngle.value_or(0), o.drawn);
}

void MapView::setWallState(const std::set<std::uint16_t>& destroyed, const std::set<std::uint16_t>& opened,
                           const std::set<std::uint16_t>& closed,
                           const std::vector<std::pair<std::pair<int, int>, nox::map::Wall>>& magic)
{
    // The cells of broken walls and open secret walls; Wall-spell walls replace whatever the map has in their cell.
    std::set<std::uint16_t> gone;
    for (const auto id : destroyed) {
        if (id < breakable_.size()) {
            gone.insert(breakable_[id]);
        }
    }
    for (std::size_t id = 0; id < secret_.size(); ++id) {
        const auto key = static_cast<std::uint16_t>(id);
        // Open when the server says so, or when the map starts it open and the server hasn't closed it.
        const bool open = opened.contains(key) || (secret_[id].second && !closed.contains(key));
        if (open) {
            gone.insert(secret_[id].first);
        }
    }
    std::set<std::uint16_t> raised;
    for (const auto& [cell, wall] : magic) {
        raised.insert(wallKey(cell.first, cell.second));
    }
    walls_.clear();
    for (const auto& w : mapWalls_) {
        const auto key = wallKey(w.x, w.y);
        if (!gone.contains(key) && !raised.contains(key)) {
            walls_.push_back(w);
        }
    }
    for (const auto& [cell, wall] : magic) {
        auto w = wall;
        w.x = static_cast<std::uint8_t>(cell.first);
        w.y = static_cast<std::uint8_t>(cell.second);
        walls_.push_back(w);
    }
    wallByKey_.clear();
    for (std::size_t i = 0; i < walls_.size(); ++i) {
        wallByKey_[wallKey(walls_[i].x, walls_[i].y)] = i; // (a later wall in a cell over an earlier)
    }
}

void MapView::setTile(int gx, int gy, const nox::map::FloorTile* tile)
{
    const auto keyOf = [](int x, int y) { return gridKey(x, y); };
    const auto keyOfTile = [](const Tile& t) { return gridKeyAt(t.x, t.y); };
    if (tileIndex_.empty() && !tiles_.empty()) {
        for (std::size_t i = 0; i < tiles_.size(); ++i) {
            tileIndex_[keyOfTile(tiles_[i])] = i;
        }
    }
    const std::int32_t key = keyOf(gx, gy);
    const auto it = tileIndex_.find(key);
    if (!tile) {
        if (it == tileIndex_.end()) {
            return;
        }
        // Swap-remove, the last tile taking the place.
        const std::size_t at = it->second;
        tileIndex_.erase(it);
        if (at + 1 != tiles_.size()) {
            tiles_[at] = std::move(tiles_.back());
            tileIndex_[keyOfTile(tiles_[at])] = at;
        }
        tiles_.pop_back();
        return;
    }
    const SDL_FPoint corner = tileCorner(gx, gy);
    Tile t{corner.x, corner.y, tile->image, tile->variant, tile->edges, {}};
    if (!t.edges.empty()) {
        t.key = edgeKey(t.floor, t.variant, t.edges);
    }
    if (it != tileIndex_.end()) {
        tiles_[it->second] = std::move(t);
    } else {
        tileIndex_[key] = tiles_.size();
        tiles_.push_back(std::move(t));
    }
}

void MapView::setWalls(const std::vector<nox::map::Wall>& walls, const nox::map::WindowWalls& windows,
                       const nox::map::DestructableWalls& breakable, const nox::map::SecretWalls& secret)
{
    mapWalls_ = walls;
    windows_.clear();
    for (const auto& p : windows.walls) {
        windows_.insert(wallKey(p.x, p.y));
    }
    breakable_.clear();
    for (const auto& p : breakable.walls) {
        breakable_.push_back(wallKey(p.x, p.y));
    }
    secret_.clear();
    for (const auto& s : secret.walls) {
        secret_.emplace_back(wallKey(s.pos.x, s.pos.y), (s.flags & 8) != 0);
    }
    setWallState({}, {}, {}, {});
}

void MapView::setPending(const nox::mapedit::PendingPlan& plan)
{
    pendingHideWalls_.clear();
    pendingWallLook_.clear();
    pendingWalls_.clear();
    pendingHideTiles_.clear();
    pendingTiles_.clear();
    pendingObjects_.clear();
    pendingHidePlan_.clear();
    pendingHideObject_.clear();
    pendingActive_ = !plan.empty();
    if (!pendingActive_) {
        return;
    }
    for (const auto& c : plan.hideWalls) {
        pendingHideWalls_.insert(wallKey(c.x, c.y));
    }
    for (const auto& w : plan.walls) {
        const auto look = static_cast<std::uint8_t>(w.look);
        if (w.inBase) {
            pendingWallLook_[wallKey(w.wall.x, w.wall.y)] = look; // (ours, drawn as the sight has it, in the look)
        } else {
            pendingWalls_.push_back({w.wall, w.window, look});
        }
    }
    for (const auto& t : plan.hideTiles) {
        pendingHideTiles_.insert(gridKey(t.gx, t.gy));
    }
    for (const auto& t : plan.tiles) {
        const SDL_FPoint corner = tileCorner(t.at.gx, t.at.gy);
        Tile tile{corner.x, corner.y, t.tile.image, t.tile.variant, t.tile.edges, {}};
        tile.look = static_cast<std::uint8_t>(t.look);
        if (!tile.edges.empty() || tile.look != 0) {
            // Composited (greyed) under its own key: "~p" pending, "~g" a ghost.
            tile.key = tile.edges.empty() ? std::format("{}.{}", tile.floor, tile.variant) : edgeKey(tile.floor, tile.variant, tile.edges);
            if (tile.look != 0) {
                tile.key += tile.look == 1 ? "~p" : "~g";
            }
        }
        pendingTiles_.push_back(std::move(tile));
    }
    for (const auto& o : plan.objects) {
        const TypeArt* a = art_->get(o.type);
        if (!a || !a->still || a->editorOnly || a->drawFn == DrawFn::PressurePlate) {
            continue;
        }
        Object object{o.x, o.y, 0, a, std::nullopt};
        object.look = static_cast<std::uint8_t>(o.look);
        pendingObjects_.push_back(std::move(object));
    }
    pendingHidePlan_ = plan.hideObjects;
    matchPendingObjects();
}

void MapView::matchPendingObjects()
{
    pendingHideObject_.clear();
    if (pendingHidePlan_.empty()) {
        return;
    }
    pendingHideObject_.assign(objects_.size(), 0);
    for (const auto& h : pendingHidePlan_) {
        const TypeArt* a = art_->get(h.type);
        for (std::size_t i = 0; i < objects_.size(); ++i) {
            const Object& o = objects_[i];
            if (!pendingHideObject_[i] && o.art == a && std::abs(o.x - h.x) < 0.5f && std::abs(o.y - h.y) < 0.5f) {
                pendingHideObject_[i] = 1; // (one of ours per one the plan names)
                break;
            }
        }
    }
}

void MapView::setAmbient(const nox::map::AmbientData* ambient, const nox::map::Polygons& polygons)
{
    ambient_.load(ambient, &polygons); // (worked out again on the next frame, as after a map load)
}

Rgb MapView::ambientAt(float x, float y) const
{
    return ambient_.at(x, y);
}

std::optional<std::uint8_t> MapView::minimapGroupAt(float x, float y) const
{
    return ambient_.groupAt(x, y);
}

std::uint8_t MapView::minimapGroup(float x, float y) const
{
    // (The frame's test, as the light's ambient takes it: at the same centre a second call this
    // frame tests nothing again. Made here too, the game making it every frame, lit or not.)
    ambient_.at(x, y);
    return ambient_.group();
}

bool MapView::duplicates(std::uint32_t netCode, float x, float y) const
{
    const auto it = objectIndex_.find(netCode);
    if (it == objectIndex_.end()) {
        return false;
    }
    const Object& o = objects_[it->second];
    return std::abs(o.x - x) <= 2 && std::abs(o.y - y) <= 2;
}

void MapView::listDoors()
{
    doors_.clear();
    for (const auto& o : objects_) {
        if (o.doorAngle && (o.art->classes & nox::things::ObjectClass::Door)) {
            doors_.push_back({o.x, o.y, *o.doorAngle});
        }
    }
}

void MapView::drawSprite(SDL_Renderer* renderer, SpriteCache& sprites, const Camera& camera, const Sprite& s,
                         float x, float y, Rgb light, float clipLeft, float clipRight, std::uint8_t alpha,
                         bool bloom, float clipBottom, std::uint8_t look, std::int32_t image, float drawScale, float pivotX,
                         float pivotY) const
{
    SDL_FRect dst = camera.toScreen(x + s.offsetX, y + s.offsetY, s.width, s.height);
    if (drawScale != 1.0f) {
        // (A mode's draw scale: about its drawable's pivot, or without one the image's middle.)
        drawscale::Point pivot{dst.x + dst.w / 2, dst.y + dst.h / 2};
        if (pivotX == pivotX && pivotY == pivotY) {
            const SDL_FRect p = camera.toScreen(pivotX, pivotY, 0, 0);
            pivot = {p.x, p.y};
        }
        const drawscale::Rect r = drawscale::scaledAbout({dst.x, dst.y, dst.w, dst.h}, drawScale, pivot);
        dst = {r.x, r.y, r.w, r.h};
    }
    // A session's pending look: its alpha, and the pending shader, or the CPU's greyed copy (the
    // same logical size and offsets) when there's none for this sprite.
    const Sprite* drawn = &s;
    bool shaded = false;
    if (look != 0) {
        alpha = pending::alphaFor(static_cast<pending::Look>(look), alpha);
        shaded = sprites.pendingInShader(s);
        if (!shaded && image >= 0) {
            if (const Sprite* copy = sprites.pendingCopy(image, look)) {
                drawn = copy;
            }
        }
    }
    // (In its own pixels: a shader-tinted sprite's image is the left half of its place.)
    SDL_FRect src{0, 0, drawn->imageWidth(), drawn->rect.h};
    // Horizontal clip in world x (walls drawn only in their visible columns).
    const float left = (clipLeft - camera.left()) * camera.zoom;
    const float right = (clipRight - camera.left()) * camera.zoom;
    if (left > dst.x || right < dst.x + dst.w) {
        const float x0 = std::max(dst.x, left);
        const float x1 = std::min(dst.x + dst.w, right);
        if (x1 <= x0) {
            return;
        }
        const float scale = src.w / dst.w;
        src.x += (x0 - dst.x) * scale;
        src.w = (x1 - x0) * scale;
        dst.x = x0;
        dst.w = x1 - x0;
    }
    // Its bottom cut at world y clipBottom (below the floor: sunk into it).
    if (const float bottom = (clipBottom - camera.top()) * camera.zoom; bottom < dst.y + dst.h) {
        if (bottom <= dst.y) {
            return;
        }
        src.h *= (bottom - dst.y) / dst.h;
        dst.h = bottom - dst.y;
    }
    drawn->scale = drawn->hd ? SDL_SCALEMODE_LINEAR : camera.artScaleMode();
    SDL_SetTextureColorMod(drawn->texture, light[0], light[1], light[2]);
    SDL_SetTextureAlphaMod(drawn->texture, alpha);
    (void)renderer;
    if (shaded) {
        sprites.renderPending(*drawn, &src, dst, look);
    } else {
        sprites.render(*drawn, &src, dst);
    }
    SDL_SetTextureColorMod(drawn->texture, 255, 255, 255);
    SDL_SetTextureAlphaMod(drawn->texture, 255);
    if (look != 0) {
        return; // (a pending edit neither blooms nor hides a light's bloom: it's see-through)
    }
    if (collectBloom_) {
        if (bloom && !s.tint) {
            bloom_.push_back({&s, src, dst, light, alpha, true});
        } else if (!bloom_.empty()) {
            bloom_.push_back({&s, src, dst, light, alpha, false}); // may hide a light drawn before it
        }
    }
}

namespace {

/// A wall's base line per direction, world offsets in the cell; directions 7-10 have a
/// second half at entry dir + 4.
constexpr std::array<std::array<float, 4>, 15> kWallBase = {{
    {0, 23, 23, 0}, {0, 0, 23, 23}, {0, 11, 23, 11}, {0, 23, 23, 0}, {0, 0, 23, 23},
    {0, 23, 23, 0}, {0, 0, 23, 23}, {0, 0, 10, 10}, {13, 10, 23, 0}, {0, 23, 10, 13},
    {0, 0, 10, 10}, {13, 10, 23, 0}, {13, 13, 23, 23}, {13, 13, 23, 23}, {0, 23, 10, 13},
}};

} // namespace

bool MapView::frontWall(const nox::map::Wall& w, float eyeX, float eyeY) const
{
    // drawBlack (byte 4's bit 2). A wall that doesn't block sight (WALL flag 1
    // off): its row below the eye. One that does: an edge the sight reaches whose seen span, swept
    // clockwise from the eye, runs right to left (case 0: the sight test's hit at its end angle left of
    // its start angle's), i.e. a face turned to the eye from below; a wall below to the east or
    // west running away from us stays solid.
    if (w.material < things_.walls.size() && !(things_.walls[w.material].flags & 1)) {
        return static_cast<float>(w.y) * 23.0f > eyeY;
    }
    const std::size_t dir = w.direction & 0x7F;
    if (dir >= kWallBase.size()) {
        return false;
    }
    const float ox = w.x * 23.0f;
    const float oy = w.y * 23.0f;
    const auto faces = [&](const std::array<float, 4>& b) {
        const float ax = ox + b[0] - eyeX;
        const float ay = oy + b[1] - eyeY;
        const float bx = ox + b[2] - eyeX;
        const float by = oy + b[3] - eyeY;
        const float cross = ax * by - ay * bx; // > 0: (a, b) in clockwise order seen from the eye
        return cross * (b[2] - b[0]) < 0;
    };
    return faces(kWallBase[dir]) || (dir >= 7 && dir <= 10 && faces(kWallBase[dir + 4]));
}

void MapView::drawScene(SDL_Renderer* renderer, const Camera& camera, SpriteCache& sprites,
                        std::span<const SceneSprite> extra, const std::set<std::uint32_t>* liveCodes,
                        const LightMap* light, const SightMap* sight) const
{
    const auto lightAt = [&](float x, float y) { return light ? light->sample(x, y) : Rgb{255, 255, 255}; };
    bloom_.clear();
    drawnMarkers_.clear();
    constexpr float kMargin = 200; // sprites reach well above and around their position
    const auto visible = [&](float x, float y) {
        return x + kMargin >= camera.left() && x - kMargin <= camera.right() && y + kMargin >= camera.top() &&
               y - kMargin <= camera.bottom();
    };

    // The scene's order: the solid walls (WALL flag 4 off) aren't sorted with the
    // rest. Those that aren't front walls are drawn first, under everything; the front ones last,
    // over everything (translucent). The see-through walls (fences, cages) are sorted with the
    // objects, back to front, a straight one by which side of it we stand on.
    using Item = SceneItem;
    auto& items = sceneItems_;
    auto& backWalls = backWalls_;
    auto& frontWalls = frontWalls_;
    items.clear();
    backWalls.clear();
    frontWalls.clear();
    const auto seeThroughKey = [&](const nox::map::Wall& w) {
        // A straight diagonal sorts at its cell's top or bottom by our side of it.
        const int dir = w.direction & 0x7F;
        int step = 0;
        int x0 = 0;
        if (dir == 0 || dir == 3 || dir == 11) {
            step = -23;
            x0 = w.x * 23 + 22;
        } else if (dir == 1 || dir == 4 || dir == 12) {
            step = 23;
            x0 = w.x * 23;
        } else {
            return static_cast<float>(w.y * 23 + 11);
        }
        // Which side of the diagonal the camera is on (the sign of a cross product).
        int side = (static_cast<int>(camera.y) - w.y * 23) * step + (static_cast<int>(camera.x) - x0) * -23;
        if (step < 0) {
            side = -side;
        }
        return static_cast<float>(side < 0 ? w.y * 23 + 22 : w.y * 23);
    };
    // A front wall: with line of sight, as the sight found it (drawBlack); without, by the rule
    // for every wall in view.
    const auto isFront = [&](const nox::map::Wall& w) {
        if (!translucentFrontWalls_) {
            return false; // (the map editor's opaque walls: none drawn as a front wall)
        }
        if (sight) {
            const auto* s = sight->wallRegion().wall(w.x, w.y);
            return s && s->front;
        }
        return frontWall(w, camera.x, camera.y);
    };
    // Walls in view: see-through ones sorted with the objects; solid ones split into front and back.
    for (const auto& w : walls_) {
        if (!visible(w.x * kWallCell, w.y * kWallCell)) {
            continue;
        }
        if (!pendingHideWalls_.empty() && pendingHideWalls_.contains(wallKey(w.x, w.y))) {
            continue; // (a session's pending edit has the cell)
        }
        if (w.material < things_.walls.size() && (things_.walls[w.material].flags & 4)) {
            items.push_back({seeThroughKey(w), &w, nullptr, nullptr});
        } else if (isFront(w)) {
            frontWalls.push_back(&w);
        } else {
            backWalls.push_back(&w);
        }
    }
    // A session's pending walls: see-through decorations sorted with the objects, drawn where the
    // floor under them is in sight (fog covers them as it does the floor); they never take part in
    // the sight, nor are they front walls.
    for (const auto& p : pendingWalls_) {
        const float x0 = p.wall.x * kWallCell;
        const float y0 = p.wall.y * kWallCell;
        if (!visible(x0, y0)) {
            continue;
        }
        // (Its cell's middle or a corner in sight: a real wall in the cell may hide the middle.)
        if (sight && !sight->visible(x0 + 11, y0 + 11) && !sight->visible(x0 + 2, y0 + 2) && !sight->visible(x0 + 21, y0 + 2) &&
            !sight->visible(x0 + 2, y0 + 21) && !sight->visible(x0 + 21, y0 + 21)) {
            continue;
        }
        pendingSeen_.insert(wallKey(p.wall.x, p.wall.y)); // (for the minimap's pending walls)
        items.push_back({seeThroughKey(p.wall), nullptr, nullptr, nullptr, -1, &p});
    }
    // A drawable's sight test (the drawable sight test, SightMap::drawableSeen); anything else
    // drawn with the scene (effects, icons) at its point.
    const auto extraSeen = [&](const SceneSprite& e) {
        if (!sight) {
            return true;
        }
        if (e.sightArt) {
            return sight->drawableSeen(*e.sightArt, e.sightKey, e.lightX, e.lightY, e.sightZ, e.sightDoorAngle,
                                       e.drawnImage ? *e.drawnImage : nox::client::DrawnImage{});
        }
        return sight->visible(e.lightX, e.lightY);
    };
    // The generic drawable draw notes the size of each image it draws on the drawable (its sight
    // test's box from then on): the image's height less what sinks below the floor, nothing
    // noted when all of it does.
    const auto noteImage = [](nox::client::DrawnImage& d, const Sprite& s, float z) {
        int h = static_cast<int>(s.height);
        if (z < 0) {
            h += static_cast<int>(z);
            if (h < 1) {
                return;
            }
        }
        d = {true, static_cast<std::uint16_t>(s.width), static_cast<std::uint16_t>(h)};
    };
    // The game frame Fade Objects' sort and fade go by (client/fade.hpp).
    const auto frame = static_cast<std::uint32_t>(std::max(time_, 0.0));
    for (std::size_t index = 0; index < objects_.size(); ++index) {
        const Object& o = objects_[index];
        if ((o.art->classes & nox::things::ObjectClass::Fire) && !o.enabled) {
            // A disabled drawable is hidden only when its class has FIRE (an
            // unlit candle, a flame not yet started); others (VISIBLE_ENABLE gears and pulleys)
            // stand still, drawn.
            continue;
        }
        if (!visible(o.x, o.y) || (liveCodes && liveCodes->contains(o.extent))) {
            continue;
        }
        if (index < pendingHideObject_.size() && pendingHideObject_[index]) {
            continue; // (a session's pending edit moved or removed it)
        }
        // The map's own objects are drawables too: those of the general list (not BELOW, not a
        // wall hanging) go by the scene's sort and linger out of sight with Fade Objects; the
        // rest are drawn only in sight. Lingering, the generic draw fades them.
        bool drawn = objectSeen(index, sight);
        std::int16_t alpha = -1;
        if (!o.art->below && !o.art->hanging) {
            drawn = nox::client::sortStep(o.fade, {.fadeObjects = fadeObjects_, .inSight = drawn, .frame = frame});
        }
        if (!drawn) {
            continue;
        }
        nox::client::markDrawn(o.fade, frame);
        // An obelisk drawn goes on the minimap (the scene draw, after its DRAW; SUBCLASS
        // VISIBLE_OBELISK). (One past 0x7FFF has no net code.)
        if ((o.art->classes & nox::things::ObjectClass::Immobile) && (o.art->subclasses & 0x8u) && o.extent <= 0x7FFFu) {
            drawnMarkers_.push_back(o.extent);
        }
        if (o.fade.lingering && o.art->fades && !o.plate) {
            alpha = nox::client::lingerAlpha(time_ - static_cast<double>(o.fade.inSightFrame));
        }
        // BELOW things (a plate, a fire pit, scorch marks) lie on the floor: before everything
        // standing on them, in their own order; the floor's decals (the scene's second list,
        // drawn with the floor: a cave lift's stone ring) before the other BELOW things (its
        // pit, drawn over it). Wall hangings (AIRBORNE + NO_COLLIDE: shields, banners) aren't
        // sorted at all: the exe draws them after the back walls, before everything else (the
        // hangings' own list), in the order they come.
        items.push_back({sortKey(*o.art, o.y, o.plate.has_value()), nullptr, &o, nullptr, alpha});
    }
    // A session's pending client-only things, where their point is in sight.
    for (const Object& o : pendingObjects_) {
        if (!visible(o.x, o.y) || (sight && !sight->visible(o.x, o.y))) {
            continue;
        }
        items.push_back({sortKey(*o.art, o.y, false), nullptr, &o, nullptr});
    }
    // A drawable's body goes by the scene's sort (client/fade.hpp), worked out once for its sprites
    // (they come together): the general list's lingers out of sight with Fade Objects; the others
    // are drawn only in sight. Drawn while neither lingering nor told into shadows, the frame is
    // its last in sight; drawn lingering, the fade is its alpha (the generic draw's), but over
    // the invisible's look, for the floor's decals (drawn apart, never faded) and for what's drawn
    // by its own DRAW rather than the generic one; the Ghost takes the smaller of its own and the fade.
    const nox::client::FadeState* sorted = nullptr;
    bool sortedDrawn = false;
    for (const auto& e : extra) {
        if (!(e.sprite || e.draw)) {
            continue;
        }
        std::int16_t alpha = -1;
        if (e.fade) {
            if (e.fade != sorted) {
                sorted = e.fade;
                const bool seen = extraSeen(e);
                sortedDrawn = e.fadeGeneral ? nox::client::sortStep(*e.fade, {.fadeObjects = fadeObjects_,
                                                                              .self = e.fadeSelf,
                                                                              .inSight = seen,
                                                                              .creatureOrPlayer = e.fadeCreature,
                                                                              .frame = frame})
                                            : seen || e.fadeSelf;
                if (sortedDrawn) {
                    nox::client::markDrawn(*e.fade, frame);
                }
            }
            if (!sortedDrawn) {
                continue;
            }
            if (e.fade->lingering && !e.fadeKeepAlpha && !e.fadeNever) {
                alpha = nox::client::lingerAlpha(time_ - static_cast<double>(e.fade->inSightFrame));
                if (e.fadeMin) {
                    alpha = std::min<std::int16_t>(alpha, e.alpha);
                }
            }
        } else if (!extraSeen(e)) {
            continue;
        }
        items.push_back({e.sortY, nullptr, nullptr, &e, alpha});
    }
    std::ranges::stable_sort(items, {}, &Item::sortY);
    // The solid walls in the map's order, row by row (the scene draw's grid walk).
    const auto byRow = [](const nox::map::Wall* a, const nox::map::Wall* b) { return a->y != b->y ? a->y < b->y : a->x < b->x; };
    std::ranges::sort(backWalls, byRow);
    std::ranges::sort(frontWalls, byRow);

    // `prepare`: its image only made, nothing drawn (batchImages_). `pending`: a session's pending
    // wall (a decoration: no sight, never a front wall, not remembered as seen).
    const auto drawWall = [&](const nox::map::Wall& w, bool prepare = false, const PendingWall* pending = nullptr) {
        if (w.material >= things_.walls.size()) {
            return;
        }
        const bool window = pending ? pending->window : windows_.contains(wallKey(w.x, w.y));
        std::uint8_t look = pending ? pending->look : std::uint8_t{0};
        if (!pending && !pendingWallLook_.empty()) {
            if (const auto it = pendingWallLook_.find(wallKey(w.x, w.y)); it != pendingWallLook_.end()) {
                look = it->second; // (ours, in a pending look: a ghost of what the working copy removed)
            }
        }
        std::size_t dir = w.direction & 0x7F;
        std::pair<float, float> columns{-1e9f, 1e9f};
        bool front = false;
        if (sight && !pending) {
            // drawWalls: only a wall the sight reached, drawn as the direction its seen arms
            // make (a crossing seen from one side as a T or a straight wall; a window's
            // windowed piece, a half wall's half), only in the columns of that direction's base
            // line the sight reaches.
            const auto d = sight->wallRegion().wallDraw(w.x, w.y, w.direction, window);
            if (!d) {
                return;
            }
            dir = static_cast<std::size_t>(d->image);
            if (d->clipLeft) {
                columns.first = static_cast<float>(*d->clipLeft);
            }
            if (d->clipRight) {
                columns.second = static_cast<float>(*d->clipRight);
            }
            front = d->front;
        } else {
            // Windows in the straight diagonals draw the windowed pieces, images 11 and 12.
            if (window && dir <= 1) {
                dir += 11;
            }
            front = !pending && frontWall(w, camera.x, camera.y);
        }
        front = front && translucentFrontWalls_;
        // (A variant the direction drawn doesn't have: its image is empty, nothing drawn.)
        const auto* images = wallImages(things_.walls[w.material], dir, w.variant);
        if (!images) {
            return;
        }
        // A front wall (facing the eye from below) is drawn with its front image (slot 1,
        // drawWalls) at half alpha, so what's behind it shows (TranslucentFrontWalls, always on
        // here), unless its kind is see-through already (WALL flag 4: fences, cages).
        const bool translucent = front && !(things_.walls[w.material].flags & 4);
        const auto& slots = *images;
        const auto& image = front && slots[1].image.index >= 0 ? slots[1] : slots[0];
        if (prepare) {
            sprites.get(image.image);
            return;
        }
        if (!pending) {
            seenWalls_.set(wallKey(w.x, w.y)); // for the minimap (drawWalls sets wall byte 0xC)
        }
        if (const Sprite* s = sprites.get(image.image)) {
            // Lit from the cell's centre (drawWalls: 23x + 11, 23y + 11).
            drawSprite(renderer, sprites, camera, *s, w.x * kWallCell - 50 + static_cast<float>(image.x),
                       w.y * kWallCell - 72 - static_cast<float>(image.y),
                       lightAt(w.x * kWallCell + 11, w.y * kWallCell + 11), columns.first, columns.second,
                       translucent ? 0x80 : 255, false, 1e9f, look, look ? sprites.indexOf(image.image) : -1);
        }
    };

    // (Not again while the view stands still and nothing new was made since: they're all made.)
    const std::array<float, 4> view{camera.left(), camera.top(), camera.right(), camera.bottom()};
    if (batchImages_ && (sprites.made() != preparedMade_ || view != preparedView_)) {
        // Every image the draw below takes made first, so the new ones go up to the GPU together
        // rather than an upload between draws each (the first look over a big map).
        const auto prepareObject = [&](const Object& o) {
            if (o.plate) {
                return;
            }
            if (!o.layers.empty()) {
                for (const auto& [image, colours] : o.layers) {
                    colours ? sprites.tinted(image, *colours) : sprites.get(image);
                }
                return;
            }
            if (const auto image = mapObjectImage(*o.art, o.extent, o.doorAngle, o.frame, o.enabled, o.xStatus, o.xStatusSince, time_)) {
                o.colours ? sprites.tinted(*image, *o.colours) : sprites.get(*image);
            }
        };
        for (const nox::map::Wall* w : backWalls) {
            drawWall(*w, true);
        }
        for (const auto& item : items) {
            if (item.wall) {
                drawWall(*item.wall, true);
            } else if (item.pendingWall) {
                drawWall(item.pendingWall->wall, true, item.pendingWall);
            } else if (item.object) {
                prepareObject(*item.object);
            }
        }
        for (const nox::map::Wall* w : frontWalls) {
            drawWall(*w, true);
        }
    }
    // Back walls first, then everything sorted back to front, then the front walls over it all.
    for (const nox::map::Wall* w : backWalls) {
        drawWall(*w);
    }
    for (const auto& item : items) {
        if (item.wall) {
            drawWall(*item.wall);
        } else if (item.pendingWall) {
            drawWall(item.pendingWall->wall, false, item.pendingWall);
        } else if (item.extra) {
            const auto& e = *item.extra;
            if (e.draw) {
                e.draw(renderer, camera);
            } else {
                drawSprite(renderer, sprites, camera, *e.sprite, e.x, e.y,
                           e.light ? *e.light : lightAt(e.lightX, e.lightY), -1e9f, 1e9f,
                           item.alpha >= 0 ? static_cast<std::uint8_t>(item.alpha) : e.alpha, e.bloom, e.clipBottom,
                           e.pendingLook, e.pendingImage, e.scale, e.pivotX, e.pivotY);
                if (e.notesImage && e.drawnImage) {
                    noteImage(*e.drawnImage, *e.sprite, e.sightZ);
                }
            }
        } else {
            const auto& o = *item.object;
            if (o.plate) {
                drawPlate(renderer, camera, o);
                continue;
            }
            if (!o.layers.empty()) {
                // The editor's NPC: its layers back to front, each from the object's origin (NPCDraw).
                const std::uint8_t alpha = item.alpha >= 0 ? static_cast<std::uint8_t>(item.alpha) : std::uint8_t{255};
                for (const auto& [image, colours] : o.layers) {
                    if (const Sprite* s = colours ? sprites.tinted(image, *colours) : sprites.get(image)) {
                        drawSprite(renderer, sprites, camera, *s, o.x - o.art->halfWidth, o.y - o.art->halfHeight - o.art->z,
                                   lightAt(o.x, o.y), -1e9f, 1e9f, alpha);
                    }
                }
                continue;
            }
            const auto image = mapObjectImage(*o.art, o.extent, o.doorAngle, o.frame, o.enabled, o.xStatus, o.xStatusSince, time_);
            if (!image) {
                continue;
            }
            const Sprite* s = o.colours ? sprites.tinted(*image, *o.colours) : sprites.get(*image);
            if (s) {
                // A light source that's on is drawn full bright, not lit (a white light).
                const bool selfLit = (o.art->classes & nox::things::ObjectClass::Light) && o.enabled;
                drawSprite(renderer, sprites, camera, *s, o.x - o.art->halfWidth, o.y - o.art->halfHeight - o.art->z,
                           selfLit ? Rgb{255, 255, 255} : lightAt(o.x, o.y), -1e9f, 1e9f,
                           item.alpha >= 0 ? static_cast<std::uint8_t>(item.alpha) : std::uint8_t{255},
                           (o.art->classes & nox::things::ObjectClass::Light) != 0, // light sources bloom
                           1e9f, o.look, o.look ? sprites.indexOf(*image) : -1);
                if (o.art->fades) {
                    noteImage(o.drawn, *s, 0);
                }
            }
            if (questLocks_ && o.doorAngle && o.lockMark) {
                // DoorDraw in a quest, its mark still on (MSG_GAUNTLET 0x0F clears it):
                // the lock over it, lit as the door, at its place less (0x40, 0x4F) and by its
                // angle: 0 (-0xF, -0x14) a SW image, 8 (+0xF, -0x14) SE, 0x10 (+8, +2) SW, any
                // other (-8, +2) SE; silver for lock 1, else gold.
                const std::uint8_t angle = *o.doorAngle;
                const bool sw = angle == 0 || angle == 0x10;
                const auto dx = static_cast<float>(angle == 0 ? -0xF : angle == 8 ? 0xF : angle == 0x10 ? 8 : -8);
                const auto dy = static_cast<float>(angle == 0 || angle == 8 ? -0x14 : 2);
                if (const nox::things::ImageRef* lockImage = lockImages_[(o.lock == 1 ? 0 : 2) + (sw ? 0 : 1)]) {
                    if (const Sprite* lock = sprites.get(*lockImage)) {
                        drawSprite(renderer, sprites, camera, *lock, o.x - 0x40 + dx, o.y - 0x4F + dy, lightAt(o.x, o.y));
                    }
                }
            }
        }
    }
    for (const nox::map::Wall* w : frontWalls) {
        drawWall(*w);
    }
    if (batchImages_) {
        preparedMade_ = sprites.made();
        preparedView_ = view;
    }
}

#ifndef NOX_NO_TEST_COMMANDS
void MapView::traceFade() const
{
    for (const Object& o : objects_) {
        if (o.fade.lingering) {
            SDL_Log("trace-fade %.2f map %s #%u at %.0f,%.0f in-sight %u alpha %u%s", time_, o.art->name.c_str(), o.extent,
                    o.x, o.y, o.fade.inSightFrame, nox::client::lingerAlpha(time_ - static_cast<double>(o.fade.inSightFrame)),
                    o.art->fades ? "" : " (not faded: its own DRAW)");
        }
    }
}
#endif

const nox::things::ImageRef* MapView::floorRef(std::uint8_t floor, std::int16_t variant) const
{
    if (floor >= things_.floors.size() || variant < 0 ||
        static_cast<std::size_t>(variant) >= things_.floors[floor].images.size()) {
        return nullptr;
    }
    return &things_.floors[floor].images[static_cast<std::size_t>(variant)];
}

const nox::things::ImageRef* MapView::edgeRef(const nox::map::FloorEdge& e) const
{
    if (e.edge >= things_.edges.size() || e.direction >= things_.edges[e.edge].images.size()) {
        return nullptr;
    }
    return &things_.edges[e.edge].images[e.direction];
}

const nox::video::DecodedImage* MapView::floorImage(std::uint8_t floor, std::int16_t variant,
                                                    SpriteCache& sprites) const
{
    const auto* ref = floorRef(floor, variant);
    if (!ref) {
        return nullptr;
    }
    const auto* img = sprites.decoded(sprites.indexOf(*ref));
    return img && img->width == kTilePixels && img->height == kTilePixels ? img : nullptr;
}

void MapView::resolveTile(const Tile& tile, SpriteCache& sprites) const
{
    if (tile.baseIndex != Tile::kUnresolved) {
        return;
    }
    const auto* base = floorRef(tile.floor, tile.variant);
    tile.baseIndex = base ? sprites.indexOf(*base) : -1;
    tile.edgeIndexes.clear();
    for (const auto& e : tile.edges) {
        const auto* floor = floorRef(e.floor, e.variant);
        const auto* edge = edgeRef(e);
        tile.edgeIndexes.push_back({floor ? sprites.indexOf(*floor) : -1, edge ? sprites.indexOf(*edge) : -1});
    }
}

const Sprite* MapView::hdTile(const Tile& tile, SpriteCache& sprites, bool* pending) const
{
    // With the image pack's upscaled floors (its Floors/, 4x): the tile composited at their
    // resolution as edgedTile does at the original's, each image from the pack where it has it
    // and the original's pixels (each repeated) where it doesn't, the edges' masks (46 x 46)
    // scaled up. Null when none of its images is upscaled (drawn as before).
    resolveTile(tile, sprites);
    // The largest upscale among the tile's and its edges' floor images; 1 means none is upscaled.
    // Any of its images (edges' masks too) still being decoded: none meanwhile, so no composite is
    // made (and kept) half SD (drawTiles waits for them, or with placeholders draws the SD tile).
    int scale = 1;
    bool waiting = false;
    const auto consider = [&](std::int32_t index) {
        const SpriteCache::TilePixels* t = index >= 0 ? sprites.hdTilePixels(index) : nullptr;
        if (const nox::video::PackImage* p = t ? t->image.get() : nullptr; p && p->width == kTilePixels && p->height == kTilePixels) {
            scale = std::max(scale, static_cast<int>(p->pixelWidth) / p->width);
        }
        waiting = waiting || (index >= 0 && sprites.hdTilePending(index));
    };
    const std::int32_t base = tile.baseIndex;
    consider(base);
    for (const auto& [floor, mask] : tile.edgeIndexes) {
        consider(floor);
        if (mask >= 0) {
            sprites.hdTilePixels(mask);
            waiting = waiting || sprites.hdTilePending(mask);
        }
    }
    if (scale <= 1 || waiting) {
        if (pending && waiting) {
            *pending = true;
        }
        return nullptr;
    }
    const int n = kTilePixels * scale;
    // Cached per tile (or edge stack) and scale, built on the sprite cache's workers (the SD tile
    // till then) and kept till the next map (one of the map's own). A session's pending tile is
    // composited greyed, under its own key (its look's suffix), and never kept. (The key made
    // again only when the scale changes.)
    if (tile.hdKeyScale != scale) {
        tile.hdKey = (tile.key.empty() ? "tile:" + std::to_string(base) : tile.key) + "@x" + std::to_string(scale);
        tile.hdKeyScale = scale;
        tile.hdHint = {};
    }
    const std::string& key = tile.hdKey;
    const auto floorIndex = [&](std::size_t edge) { return tile.edgeIndexes[edge][0]; };
    const auto edgeIndex = [&](std::size_t edge) { return tile.edgeIndexes[edge][1]; };
    const auto look = static_cast<pending::Look>(tile.look);
    const Sprite* s = sprites.compositeLater(key, n, n, [&]() -> SpriteCache::Builder {
        // What the build reads, its own: the HD pixels shared (nothing changes them), the
        // originals' (46 x 46) copied.
        struct Source {
            std::shared_ptr<const nox::video::PackImage> hd; ///< upscaled to this tile's size
            std::shared_ptr<const nox::video::DecodedImage> sd;
        };
        const auto source = [&](std::int32_t index) {
            Source src;
            if (index < 0) {
                return src;
            }
            if (auto p = sprites.hdTile(index); p && static_cast<int>(p->pixelWidth) == n && static_cast<int>(p->pixelHeight) == n) {
                src.hd = std::move(p);
            }
            if (const auto* d = sprites.decoded(index); d && d->width == kTilePixels && d->height == kTilePixels) {
                src.sd = std::make_shared<const nox::video::DecodedImage>(*d);
            }
            return src;
        };
        struct Edge {
            Source mask; ///< the edge image: its pixels, and its mask (the original's edgeMask)
            Source blend;
        };
        Source baseSource = source(base);
        std::vector<Edge> edges;
        for (std::size_t e = 0; e < tile.edgeIndexes.size(); ++e) {
            Edge edge{source(edgeIndex(e)), source(floorIndex(e))};
            if (edge.mask.sd && edge.mask.sd->edgeMask.size() == static_cast<std::size_t>(kTilePixels) * kTilePixels) {
                edges.push_back(std::move(edge));
            }
        }
        return [n, scale, look, baseSource = std::move(baseSource), edges = std::move(edges)] {
            std::vector<std::uint8_t> out(static_cast<std::size_t>(n) * n * 4, 0);
            // An image's pixel at the HD (x, y): the pack's if it's upscaled to this size, else the
            // original's pixel it covers.
            const auto pixelOf = [&](const Source& src, int x, int y) -> const std::uint8_t* {
                if (src.hd) {
                    return &src.hd->rgba[(static_cast<std::size_t>(y) * n + static_cast<std::size_t>(x)) * 4];
                }
                if (!src.sd) {
                    return nullptr;
                }
                return &src.sd->rgba[(static_cast<std::size_t>(y / scale) * kTilePixels + static_cast<std::size_t>(x / scale)) * 4];
            };
            // The base floor first.
            for (int y = 0; y < n; ++y) {
                for (int x = 0; x < n; ++x) {
                    if (const std::uint8_t* src = pixelOf(baseSource, x, y)) {
                        std::copy(src, src + 4, out.begin() + (static_cast<std::ptrdiff_t>(y) * n + x) * 4);
                    }
                }
            }
            // The base tile's diamond (its alpha), which keeps the edges' blur inside it.
            std::vector<std::uint8_t> inside;
            if (baseSource.sd) {
                inside.resize(static_cast<std::size_t>(kTilePixels) * kTilePixels);
                for (std::size_t i = 0; i < inside.size(); ++i) {
                    inside[i] = baseSource.sd->rgba[i * 4 + 3] != 0 ? 1 : 0;
                }
            }
            for (const Edge& e : edges) {
                const auto& mask = *e.mask.sd;
                // The mask scaled up as smooth blend weights (the original's blocks and dithers made
                // borders and gradients: edgeWeights), the colours mixed by them; the alpha the nearest
                // mask pixel's, so the tile's diamond stays hard.
                const auto maskAt = [&](int mx, int my) {
                    mx = std::clamp(mx, 0, kTilePixels - 1);
                    my = std::clamp(my, 0, kTilePixels - 1);
                    return mask.edgeMask[static_cast<std::size_t>(my) * kTilePixels + static_cast<std::size_t>(mx)];
                };
                const nox::video::EdgeWeights weights = nox::video::edgeWeights(mask.edgeMask, kTilePixels, scale, inside);
                // An edge not upscaled: its pixels spread past its own (mask 1) ones, so a soft border
                // mixes in its colour (the pack's upscaled edges come spread already).
                std::vector<std::uint8_t> spread;
                if (!e.mask.hd) {
                    spread = mask.rgba;
                    std::vector<std::uint8_t> own(mask.edgeMask.size());
                    for (std::size_t i = 0; i < own.size(); ++i) {
                        own[i] = mask.edgeMask[i] == 1 ? 1 : 0;
                    }
                    nox::video::spreadColour(spread, own, kTilePixels, kTilePixels);
                }
                for (int y = 0; y < n; ++y) {
                    for (int x = 0; x < n; ++x) {
                        const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(n) + static_cast<std::size_t>(x);
                        const float w1 = weights.own[i];
                        const float w2 = weights.floor[i];
                        if (w1 <= 0 && w2 <= 0) {
                            continue;
                        }
                        std::uint8_t* dst = &out[i * 4];
                        const std::uint8_t* edgePx = w1 <= 0 ? nullptr
                                                     : e.mask.hd ? pixelOf(e.mask, x, y)
                                                                 : &spread[(static_cast<std::size_t>(y / scale) * kTilePixels + static_cast<std::size_t>(x / scale)) * 4];
                        const std::uint8_t* blendPx = w2 > 0 ? pixelOf(e.blend, x, y) : nullptr;
                        // The colour: the tile's own less the weights taken, plus the edge's and the floor's shares.
                        for (int c = 0; c < 3; ++c) {
                            float value = static_cast<float>(dst[c]) * std::max(0.0f, 1 - (edgePx ? w1 : 0) - (blendPx ? w2 : 0));
                            value += edgePx ? static_cast<float>(edgePx[c]) * w1 : 0;
                            value += blendPx ? static_cast<float>(blendPx[c]) * w2 : 0;
                            dst[c] = static_cast<std::uint8_t>(std::clamp(value + 0.5f, 0.0f, 255.0f));
                        }
                        const std::uint8_t m = maskAt(x / scale, y / scale);
                        const std::uint8_t* hard = m == 1 ? pixelOf(e.mask, x, y) : m == 2 ? pixelOf(e.blend, x, y) : nullptr;
                        if (hard) {
                            dst[3] = hard[3];
                        }
                    }
                }
            }
            pending::applyFloor(out, n, look, scale);
            return out;
        };
    }, tile.look == 0, pending, &tile.hdHint);
    if (s) {
        s->scale = SDL_SCALEMODE_LINEAR;
    }
    return s;
}

// The tile and its edges composited at the original 46 x 46: mask 1 takes the edge image's pixel, 2 the
// edge's floor texture's, anything else keeps what is there.
const Sprite* MapView::edgedTile(const Tile& tile, SpriteCache& sprites) const
{
    return sprites.composite(tile.key, kTilePixels, kTilePixels, [&] {
        std::vector<std::uint8_t> out(static_cast<std::size_t>(kTilePixels) * kTilePixels * 4, 0);
        if (const auto* base = floorImage(tile.floor, tile.variant, sprites)) {
            out = base->rgba;
        }
        for (const auto& e : tile.edges) {
            const auto* edge = edgeRef(e);
            if (!edge) {
                continue;
            }
            const auto* mask = sprites.decoded(sprites.indexOf(*edge));
            if (!mask || mask->edgeMask.size() != static_cast<std::size_t>(kTilePixels) * kTilePixels) {
                continue;
            }
            const auto* blend = floorImage(e.floor, e.variant, sprites);
            for (std::size_t i = 0; i < mask->edgeMask.size(); ++i) {
                const std::uint8_t* src = nullptr;
                if (mask->edgeMask[i] == 1) {
                    src = &mask->rgba[i * 4];
                } else if (mask->edgeMask[i] == 2 && blend) {
                    src = &blend->rgba[i * 4];
                }
                if (src) {
                    std::copy(src, src + 4, out.begin() + static_cast<std::ptrdiff_t>(i * 4));
                }
            }
        }
        pending::applyFloor(out, kTilePixels, static_cast<pending::Look>(tile.look)); // (a session's pending tile: greyed)
        return out;
    }, false, &tile.sdHint);
}

MapView::~MapView()
{
    if (floorTarget_) {
        SDL_DestroyTexture(floorTarget_);
    }
}

void MapView::drawFloor(SDL_Renderer* renderer, const Camera& screen, SpriteCache& sprites, LightMap* light,
                        SightMap* sight)
{
    // The world rectangle in view, on whole units, and the target at 1 texel per unit (the
    // resolution of the original tiles); with the image pack in use, as many as the screen
    // shows a unit with (up to its floors' 4), for the upscaled floors.
    const float kTexelsPerUnit = sprites.usingPack() ? std::clamp(std::ceil(screen.zoom), 1.0f, 4.0f) : 1.0f;
    const float left = std::floor(screen.left());
    const float top = std::floor(screen.top());
    const int width = static_cast<int>(std::ceil(screen.right() - left)) + 1;
    const int height = static_cast<int>(std::ceil(screen.bottom() - top)) + 1;
    const int tw = static_cast<int>(width * kTexelsPerUnit);
    const int th = static_cast<int>(height * kTexelsPerUnit);
    // Below the map editor's sdFloorBelow_ zoom, the original tiles even with the pack (setSdFloorBelow).
    const bool hd = screen.zoom >= sdFloorBelow_;
    const bool perPixel = light && light->perPixel();
    // The world rectangle as pieces of at most kMaxFloorTarget texels a side, each drawn into the
    // target and scaled to the window in turn: one piece unless the view is that big (the map
    // editor zoomed far out), whose one target would be hundreds of megabytes, or past the GPU's limit.
    constexpr int kMaxFloorTarget = 4096;
    const bool pieces = tw > kMaxFloorTarget || th > kMaxFloorTarget;
    const int pieceWidth = pieces ? static_cast<int>(kMaxFloorTarget / kTexelsPerUnit) : width;
    const int pieceHeight = pieces ? static_cast<int>(kMaxFloorTarget / kTexelsPerUnit) : height;
    for (int py = 0; py < height; py += pieceHeight) {
        for (int px = 0; px < width; px += pieceWidth) {
            const int pw = pieces ? std::min(pieceWidth, width - px) : width;
            const int ph = pieces ? std::min(pieceHeight, height - py) : height;
            const int ptw = pieces ? static_cast<int>(pw * kTexelsPerUnit) : tw;
            const int pth = pieces ? static_cast<int>(ph * kTexelsPerUnit) : th;
            const float pleft = left + static_cast<float>(px);
            const float ptop = top + static_cast<float>(py);
            if (!floorTarget_ || floorTargetWidth_ < ptw || floorTargetHeight_ < pth) {
                if (floorTarget_) {
                    SDL_DestroyTexture(floorTarget_);
                }
                // Rounded up so small zoom steps and resizes don't reallocate every frame.
                floorTargetWidth_ = (ptw + 255) / 256 * 256;
                floorTargetHeight_ = (pth + 255) / 256 * 256;
                floorTarget_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
                                                 floorTargetWidth_, floorTargetHeight_);
                if (!floorTarget_) {
                    SDL_Log("floor target %dx%d: %s", floorTargetWidth_, floorTargetHeight_, SDL_GetError());
                    return;
                }
            }

            // A camera over the target: the same world rectangle at kTexelsPerUnit texels a unit.
            Camera local;
            local.zoom = kTexelsPerUnit;
            local.viewWidth = ptw;
            local.viewHeight = pth;
            local.x = pleft + ptw / (2 * kTexelsPerUnit);
            local.y = ptop + pth / (2 * kTexelsPerUnit);

            SDL_Texture* const previousTarget = SDL_GetRenderTarget(renderer); // (the frame's, with gamma)
            SDL_SetRenderTarget(renderer, floorTarget_);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
            SDL_RenderClear(renderer);
            drawTiles(renderer, local, sprites, hd);
            // A per-pixel light map goes on at the screen's resolution below; otherwise its overlay goes onto the floor target.
            if (light && !perPixel) {
                light->drawOverlay(renderer, local);
            }
            SDL_SetRenderTarget(renderer, previousTarget);

            SDL_SetTextureScaleMode(floorTarget_, screen.artScaleMode());
            const SDL_FRect src{0, 0, static_cast<float>(ptw), static_cast<float>(pth)};
            SDL_FRect dst{(left - screen.left()) * screen.zoom, (top - screen.top()) * screen.zoom,
                          width * screen.zoom, height * screen.zoom};
            if (pieces) {
                // (Each edge where its neighbour's is, so no seam opens between them.)
                const float x0 = (pleft - screen.left()) * screen.zoom;
                const float y0 = (ptop - screen.top()) * screen.zoom;
                dst = {x0, y0, (pleft + static_cast<float>(pw) - screen.left()) * screen.zoom - x0,
                       (ptop + static_cast<float>(ph) - screen.top()) * screen.zoom - y0};
            }
            SDL_RenderTexture(renderer, floorTarget_, &src, &dst);
        }
    }
    // Both at the screen's resolution over the floor just drawn (they multiply, so in either
    // order): the sight mask's edge stays sharp at any zoom instead of the floor's texels scaled up.
    if (sight) {
        sight->drawMask(renderer, screen);
    }
    if (perPixel) {
        light->drawPerPixel(renderer, screen);
    }
}

bool MapView::prefetchFloor(const Camera& camera, SpriteCache& sprites) const
{
    if (!sprites.usingPack() || !sprites.waitsForHd()) {
        return false;
    }
    constexpr float kAhead = 2 * kCell; // (as drawTiles asks ahead)
    bool pending = false;
    for (const auto& t : tiles_) {
        if (!cellInView(t.x, t.y, camera, kAhead)) {
            continue;
        }
        bool waiting = false;
        hdTile(t, sprites, &waiting);
        pending = pending || (waiting && cellInView(t.x, t.y, camera));
    }
    sprites.clearAsked(); // (not waited for here)
    return pending;
}

bool MapView::prefetchOriginals(const Camera& camera, SpriteCache& sprites, std::uint64_t untilNs) const
{
    // (What drawTiles and drawScene would make for this view first; whatever's left out here is
    // made as it's drawn, as before. Past the deadline: the rest on the next call.)
    const auto late = [&] { return SDL_GetTicksNS() > untilNs; };
    const auto inView = [&](float x, float y, float margin) {
        return x >= camera.left() - margin && x <= camera.right() + margin && y >= camera.top() - margin &&
               y <= camera.bottom() + margin;
    };
    for (const auto& t : tiles_) {
        if (!cellInView(t.x, t.y, camera)) {
            continue;
        }
        if (t.edges.empty() && t.look == 0) {
            const auto* ref = floorRef(t.floor, t.variant);
            if (!ref || sprites.cached(sprites.indexOf(*ref))) {
                continue;
            }
            sprites.get(*ref);
        } else {
            if (sprites.cachedComposite(t.key)) {
                continue;
            }
            edgedTile(t, sprites);
        }
        if (late()) {
            return true;
        }
    }
    // The walls (both of a piece's slots: which is drawn depends on the eye) and the things, with
    // a margin for how far their pictures reach from where they stand.
    constexpr float kReach = 160;
    for (const auto& w : walls_) {
        if (w.material >= things_.walls.size() || !inView(w.x * kWallCell, w.y * kWallCell, kReach)) {
            continue;
        }
        std::size_t dir = w.direction & 0x7F;
        if (windows_.contains(wallKey(w.x, w.y)) && dir <= 1) {
            dir += 11;
        }
        const auto* images = wallImages(things_.walls[w.material], dir, w.variant);
        if (!images) {
            continue;
        }
        for (const auto& slot : *images) {
            const std::int32_t index = sprites.indexOf(slot.image);
            if (index < 0 || sprites.cached(index)) {
                continue;
            }
            sprites.get(index);
            if (late()) {
                return true;
            }
        }
    }
    for (const Object& o : objects_) {
        if (o.plate || !o.art || !inView(o.x, o.y, kReach)) {
            continue;
        }
        if (!o.layers.empty()) {
            for (const auto& [image, colours] : o.layers) {
                colours ? sprites.tinted(image, *colours) : sprites.get(image);
            }
        } else if (const auto image = mapObjectImage(*o.art, o.extent, o.doorAngle, o.frame, o.enabled, o.xStatus, o.xStatusSince, time_)) {
            if (!o.colours && sprites.cached(sprites.indexOf(*image))) {
                continue;
            }
            o.colours ? sprites.tinted(*image, *o.colours) : sprites.get(*image);
        }
        if (late()) {
            return true;
        }
    }
    return false;
}

// Each tile in view: the HD composite when the pack has one, else the plain floor image or its edge
// composite. With the pack's floors waited for (SpriteCache::waitsForHd), a tile whose HD pixels or
// composite are on their way is drawn after the rest, once the workers have them (awaitAsked:
// the SD tile is drawn only if they don't come in time); the tiles just past the view are asked
// for after that, ahead of being seen, so walking rarely waits.
void MapView::drawTiles(SDL_Renderer* renderer, const Camera& camera, SpriteCache& sprites, bool hd) const
{
    constexpr float kAhead = 2 * kCell;       // asked for this far past the view
    constexpr std::uint64_t kWaitMs = 500;    // the frame's wait at most, all rounds (tiles' pixels, then composites)
    const bool pack = sprites.usingPack() && hd;
    const bool wait = pack && sprites.waitsForHd();
    const float ahead = wait ? kAhead : 0;
    // batchImages_: the tiles drawn once their images are all made (drawDeferred), so the new ones
    // go up together instead of an upload between draws each.
    auto& deferred = deferredTiles_;
    deferred.clear();
    const auto drawDeferred = [&] {
        for (const auto& [t, s] : deferred) {
            noxsdl::drawSprite(renderer, *s, nullptr, camera.toScreen(t->x, t->y, kTilePixels, kTilePixels));
        }
        deferred.clear();
    };
    // The map editor's make budget (setFloorMakeBudget): past it, a tile whose image isn't made yet
    // waits for a later frame.
    const std::uint64_t makeUntil = floorMakeBudgetMs_ > 0 ? SDL_GetTicksNS() + static_cast<std::uint64_t>(floorMakeBudgetMs_ * 1e6) : 0;
    const auto overBudget = [&] { return makeUntil != 0 && SDL_GetTicksNS() > makeUntil; };
    const auto draw = [&](const Tile& t, const Sprite* s) {
        if (s) {
            // (an upscaled tile, its texture larger than the 46 units it covers)
        } else if (t.edges.empty() && t.look == 0) {
            if (const auto* ref = floorRef(t.floor, t.variant)) {
                if (overBudget() && !sprites.cached(sprites.indexOf(*ref))) {
                    return;
                }
                s = sprites.get(*ref);
            }
        } else {
            if (overBudget() && !sprites.cachedComposite(t.key)) {
                return;
            }
            s = edgedTile(t, sprites);
        }
        if (!s) {
            return;
        }
        if (batchImages_) {
            deferred.emplace_back(&t, s);
            return;
        }
        const SDL_FRect dst = camera.toScreen(t.x, t.y, kTilePixels, kTilePixels);
        noxsdl::drawSprite(renderer, *s, nullptr, dst);
    };
    sprites.clearAsked(); // (only what this frame asks for is waited for)
    auto& missed = missedTiles_;
    auto& soon = soonTiles_;
    missed.clear();
    soon.clear();
    for (const auto& t : tiles_) {
        if (!cellInView(t.x, t.y, camera, ahead)) {
            continue;
        }
        if (!pendingHideTiles_.empty() && pendingHideTiles_.contains(gridKeyAt(t.x, t.y))) {
            continue; // (a session's pending edit has the place)
        }
        if (!cellInView(t.x, t.y, camera)) {
            soon.push_back(&t);
            continue;
        }
        bool waiting = false;
        const Sprite* s = pack ? hdTile(t, sprites, &waiting) : nullptr;
        if (!s && waiting && wait) {
            missed.push_back(&t);
            continue;
        }
        draw(t, s);
    }
    // A session's pending tiles over the places left out (not waited for: the SD composite till
    // the HD one is in).
    for (const Tile& t : pendingTiles_) {
        if (!cellInView(t.x, t.y, camera)) {
            continue;
        }
        draw(t, pack ? hdTile(t, sprites) : nullptr);
    }
    // Up to 3 rounds (a tile's pixels in, its composite is asked for: another round), sharing one
    // deadline: a frame never stops longer than kWaitMs for its floor.
    drawDeferred(); // (before any wait for the workers)
    const std::uint64_t deadline = SDL_GetTicks() + kWaitMs;
    const auto left = [&] {
        const std::uint64_t now = SDL_GetTicks();
        return now < deadline ? deadline - now : 0;
    };
    for (int round = 0; round < 3 && !missed.empty() && left() > 0 && sprites.awaitAsked(left()); ++round) {
        std::erase_if(missed, [&](const Tile* t) {
            bool waiting = false;
            const Sprite* s = hdTile(*t, sprites, &waiting);
            if (!s && waiting) {
                return false;
            }
            draw(*t, s);
            return true;
        });
    }
    for (const Tile* t : missed) {
        draw(*t, nullptr); // (not in time: as they are)
    }
    drawDeferred();
    for (const Tile* t : soon) {
        hdTile(*t, sprites); // (asked for, not waited for)
    }
}

std::optional<MapView::Useable> MapView::useableAt(float cx, float cy, const SightMap* sight) const
{
    // (The USEABLE ones in the map's order: useableObjects_.)
    std::optional<Useable> best;
    for (const std::size_t index : useableObjects_) {
        const Object& o = objects_[index];
        if (!o.enabled) {
            continue;
        }
        if (!underCursor(*o.art, o.x, o.y, 0, cx, cy) || !objectSeen(index, sight)) {
            continue;
        }
        const float depth = hoverDepth(*o.art, o.y, 0);
        if (!best || depth > best->depth) {
            best = Useable{o.extent, o.x, o.y, depth, o.art};
        }
    }
    return best;
}

} // namespace noxsdl
