// MapView::preloadList (map_view.hpp): every image a map uses itself, for the sprite cache to
// decode while it loads and keep till the next map (SpriteCache::beginMap); and sessionImages,
// those any map can need, kept for the session (SpriteCache::beginSession).
#include "map_view.hpp"

#include "noxcore/map/objects.hpp"
#include "noxcore/map/xfer.hpp"
#include "noxcore/things/object_class.hpp"

#include <algorithm>
#include <exception>
#include <format>
#include <map>
#include <set>
#include <string>

namespace noxsdl {

namespace {

constexpr int kTilePixels = 46;

/// Every image a thing type's draw can show: its still, its images, every frame of its
/// animations (each direction, each monster or object state). Players' layered art isn't here.
void drawImages(const TypeArt& art, const std::function<void(const nox::things::ImageRef&)>& add)
{
    const auto& d = art.draw;
    if (art.still) {
        add(*art.still);
    }
    for (const auto& i : d.images) {
        add(i);
    }
    for (const auto& a : d.animations) {
        for (const auto& f : a.frames) {
            add(f);
        }
    }
    if (d.directional) {
        for (const auto& dir : d.directional->directions) {
            for (const auto& f : dir) {
                add(f);
            }
        }
    }
    for (const auto& s : d.monsterStates) {
        if (s) {
            for (const auto& dir : s->animation.directions) {
                for (const auto& f : dir) {
                    add(f);
                }
            }
        }
    }
    for (const auto& a : d.stateAnimations) {
        if (a) {
            for (const auto& f : a->frames) {
                add(f);
            }
        }
    }
}

} // namespace

SpriteCache::MapImages MapView::preloadList(const nox::map::MapFile& map, ThingArt& art, SpriteCache& sprites,
                                            std::string name) const
{
    namespace oc = nox::things::ObjectClass;
    SpriteCache::MapImages out;
    out.name = std::move(name);
    const auto add = [&](std::vector<SpriteCache::MapImages::Image>& to, const nox::things::ImageRef& ref, bool tile,
                         bool tinted) {
        if (const std::int32_t index = sprites.indexOf(ref); index >= 0) {
            to.push_back({index, tile, tinted});
        }
    };

    // The floor: each tile's image and its edges' images and floors, as hdTile composites them,
    // and the composites they make (one per distinct tile and edge stack with an HD floor).
    std::vector<SpriteCache::MapImages::Image> floors;
    std::set<std::string> composites;
    for (const auto& t : tiles_) {
        int scale = 1;
        std::int32_t base = -1;
        if (const auto* ref = floorRef(t.floor, t.variant)) {
            add(floors, *ref, true, false);
            base = sprites.indexOf(*ref);
            scale = std::max(scale, sprites.hdTileScale(base));
        }
        for (const auto& e : t.edges) {
            if (const auto* ref = floorRef(e.floor, e.variant)) {
                add(floors, *ref, true, false);
                scale = std::max(scale, sprites.hdTileScale(sprites.indexOf(*ref)));
            }
            if (const auto* ref = edgeRef(e)) {
                add(floors, *ref, true, false);
            }
        }
        if (scale > 1) {
            const std::string key = (t.key.empty() ? "tile:" + std::to_string(base) : t.key) + "@x" + std::to_string(scale);
            if (composites.insert(key).second) {
                const auto side = static_cast<std::size_t>(kTilePixels * scale + 4); // (with its gutter on the page)
                out.compositeBytes += side * side * 4;
            }
        }
    }

    // Walls as the map places them: every slot of their piece (the cut-away front one too), and
    // a window's pieces.
    std::vector<SpriteCache::MapImages::Image> walls;
    for (const auto& w : mapWalls_) {
        if (w.material >= things_.walls.size()) {
            continue;
        }
        const auto& dirs = things_.walls[w.material].directions;
        const std::size_t dir = w.direction & 0x7F;
        const auto addPiece = [&](std::size_t d, std::size_t variant) {
            if (d < dirs.size() && variant < dirs[d].size()) {
                for (const auto& slot : dirs[d][variant]) {
                    if (slot.image.index >= 0 || !slot.image.name.empty()) {
                        add(walls, slot.image, false, false);
                    }
                }
            }
        };
        addPiece(dir, w.variant);
        if (windows_.contains(wallKey(w.x, w.y)) && dir <= 1) {
            addPiece(dir + 11, w.variant);
            addPiece(dir + 11, 0);
        }
    }

    // Every object the map places (the server's too), by kind: doors, the rest of the scenery and
    // objects, then monsters and NPCs, with the monsters its generators make.
    std::vector<SpriteCache::MapImages::Image> doors;
    std::vector<SpriteCache::MapImages::Image> objects;
    std::vector<SpriteCache::MapImages::Image> monsters;
    std::set<std::string> types; // (each type once)
    const auto addType = [&](std::string_view typeName) {
        if (!types.insert(std::string(typeName)).second) {
            return;
        }
        const TypeArt* a = art.get(typeName);
        if (!a || a->editorOnly || a->draw.kind == nox::things::DrawKind::Player) {
            return;
        }
        const bool tinted = a->itemColours != ItemColours::None || a->drawFn == DrawFn::Maiden;
        auto& to = (a->classes & oc::Door) ? doors : (a->classes & oc::Monster) ? monsters : objects;
        drawImages(*a, [&](const nox::things::ImageRef& ref) { add(to, ref, false, tinted); });
    };
    try {
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
        for (const auto& root : nox::map::buildObjectTree(xfers)) {
            const auto typeName = typeNames.find(records[root.record].tocId);
            if (typeName == typeNames.end()) {
                continue;
            }
            addType(typeName->second);
            // A monster generator: the monsters of its groups (its templates' types).
            const TypeArt* a = art.get(typeName->second);
            if (!a || !(a->classes & oc::MonsterGenerator)) {
                continue;
            }
            try {
                const auto& x = xfers[root.record];
                const auto data = nox::map::xfer::decode(a->xfer, x.xferVersion, x.extra, {a->classes, a->subclasses},
                                                         x.extraOffset());
                if (const auto* gen = std::get_if<nox::map::xfer::MonsterGenerator>(&data)) {
                    for (const auto& group : gen->groups) {
                        for (const auto& t : group) {
                            addType(t.type);
                        }
                    }
                }
            } catch (const std::exception&) {
                // (an older record: its monsters come on demand)
            }
        }
    } catch (const std::exception&) {
        // No objects to read: the floor and walls only.
    }

    for (auto* list : {&floors, &walls, &doors, &objects, &monsters}) {
        out.images.insert(out.images.end(), list->begin(), list->end());
    }
    return out;
}

SpriteCache::MapImages sessionImages(const nox::things::ThingDb& things, ThingArt& art, SpriteCache& sprites,
                                     std::string* counts)
{
    namespace oc = nox::things::ObjectClass;
    // What players carry, wear, throw and cast, and what they pick up.
    constexpr std::uint32_t kCarried = oc::Missile | oc::Food | oc::Key | oc::Wand | oc::Fire | oc::Weapon | oc::Armor |
                                       oc::Treasure | oc::Flag | oc::Pickup | oc::ClientPersist | oc::ClientPredict;
    SpriteCache::MapImages out;
    out.name = "session";
    std::set<std::int32_t> named; // every image thing.bin names (the interface's art is the rest)
    std::set<std::int32_t> taken;
    // By kind, in the order they're wanted: the players', the carried things', the icons, then
    // thing.bin's named images (smallest first: the big ones are the menus' screens).
    std::vector<SpriteCache::MapImages::Image> playerList;
    std::vector<SpriteCache::MapImages::Image> carriedList;
    std::vector<SpriteCache::MapImages::Image> iconList;
    std::vector<SpriteCache::MapImages::Image> namedList;
    // (The loading screens, LS_*, are never drawn by noxsdl.)
    const auto wanted = [&](std::int32_t index) {
        return index >= 0 && sprites.hdPackEntry(index) && !sprites.packName(index).starts_with("LS_");
    };
    const auto add = [&](std::vector<SpriteCache::MapImages::Image>& to, const nox::things::ImageRef& ref, bool tinted,
                         std::size_t& count) {
        const std::int32_t index = sprites.indexOf(ref);
        if (index >= 0 && wanted(index) && taken.insert(index).second) {
            to.push_back({index, false, tinted});
            ++count;
        }
    };
    const auto note = [&](const nox::things::ImageRef& ref) {
        if (const std::int32_t index = sprites.indexOf(ref); index >= 0) {
            named.insert(index);
        }
    };
    std::size_t players = 0;
    std::size_t carried = 0;
    std::size_t imag = 0;
    std::size_t icons = 0;
    std::size_t ui = 0;
    std::vector<const TypeArt*> carriedTypes;
    for (const auto& def : things.things) {
        const TypeArt* a = art.get(def.name);
        if (def.menuIcon) {
            note(*def.menuIcon);
        }
        if (def.prettyImage) {
            note(*def.prettyImage);
        }
        if (!a) {
            continue;
        }
        drawImages(*a, note);
        if (a->draw.kind == nox::things::DrawKind::Player) {
            // The players' layered art: the body, armour and weapons in every state (material-shaded).
            for (const auto& state : a->draw.playerStates) {
                if (!state) {
                    continue;
                }
                const auto part = [&](const std::optional<nox::things::PlayerPart>& p) {
                    if (!p) {
                        return;
                    }
                    for (const auto& dir : *p) {
                        for (const auto& f : dir) {
                            note(f);
                            add(playerList, f, true, players);
                        }
                    }
                };
                part(state->naked);
                for (const auto& p : state->armor) {
                    part(p);
                }
                for (const auto& p : state->weapons) {
                    part(p);
                }
            }
        } else if ((a->classes & kCarried) && !a->editorOnly) {
            carriedTypes.push_back(a);
            const bool tinted = a->itemColours != ItemColours::None;
            drawImages(*a, [&](const nox::things::ImageRef& ref) { add(carriedList, ref, tinted, carried); });
            if (def.menuIcon) {
                add(carriedList, *def.menuIcon, false, carried);
            }
            if (def.prettyImage) {
                add(carriedList, *def.prettyImage, false, carried);
            }
        }
    }
    for (const auto& def : things.images) {
        if (def.image) {
            note(*def.image);
            add(namedList, *def.image, false, imag);
        }
        if (def.animation) {
            for (const auto& f : def.animation->frames) {
                note(f);
                add(namedList, f, false, imag);
            }
        }
    }
    for (const auto& s : things.spells) {
        for (const auto* ref : {&s.icon, &s.iconEnabled}) {
            note(*ref);
            add(iconList, *ref, false, icons);
        }
    }
    for (const auto& ab : things.abilities) {
        for (const auto& ref : ab.icons) {
            note(ref);
            add(iconList, ref, false, icons);
        }
    }
    // The map's own art isn't the interface's.
    for (const auto& f : things.floors) {
        for (const auto& ref : f.images) {
            note(ref);
        }
    }
    for (const auto& e : things.edges) {
        for (const auto& ref : e.images) {
            note(ref);
        }
    }
    for (const auto& w : things.walls) {
        for (const auto& dir : w.directions) {
            for (const auto& variant : dir) {
                for (const auto& slot : variant) {
                    note(slot.image);
                }
            }
        }
    }
    // The interface: the pack's HD images nothing above names.
    std::vector<SpriteCache::MapImages::Image> uiList;
    for (const std::int32_t index : sprites.hdImages()) {
        if (!named.contains(index) && !taken.contains(index) && wanted(index)) {
            uiList.push_back({index, false, false});
            ++ui;
        }
    }
    const auto bySize = [&](std::vector<SpriteCache::MapImages::Image>& list) {
        std::ranges::stable_sort(list, {}, [&](const SpriteCache::MapImages::Image& i) {
            const auto* e = sprites.hdPackEntry(i.index);
            return static_cast<std::size_t>(e->pixelWidth) * e->pixelHeight;
        });
    };
    bySize(namedList);
    bySize(uiList);
    for (auto* list : {&playerList, &carriedList, &iconList, &namedList, &uiList}) {
        out.images.insert(out.images.end(), list->begin(), list->end());
    }
    if (counts) {
        *counts = std::format("players {}, carried things {} ({} types), named images {}, icons {}, interface {}", players,
                              carried, carriedTypes.size(), imag, icons, ui);
    }
    return out;
}

} // namespace noxsdl
