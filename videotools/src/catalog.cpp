#include "catalog.hpp"

#include "noxcore/things/draw.hpp"
#include "noxcore/things/object_class.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <map>
#include <optional>
#include <unordered_map>

namespace videotools {

using nox::things::ImageRef;
namespace ObjectClass = nox::things::ObjectClass;

namespace {

constexpr std::array<std::string_view, 8> kDirectionNames = {"NW", "N", "NE", "W", "E", "SW", "S", "SE"};

// Unreferenced images grouped under one name are split into entries of at most this many.
constexpr std::size_t kMaxGroupImages = 1000;

/// thing.bin strings sometimes keep their NUL terminator (a sound of "\0" means none).
std::string text(std::string_view s)
{
    return std::string(s.substr(0, s.find('\0')));
}

std::string stripExtension(std::string_view name)
{
    std::string s(name);
    if (const auto dot = s.rfind('.'); dot != std::string::npos) {
        s.resize(dot);
    }
    return s;
}

/// Resolves thing.bin image references to bag indices and records which images are used.
class Resolver {
public:
    explicit Resolver(const nox::video::VideoBag& bag) : used_(bag.images().size(), false)
    {
        // As VideoBag::findImage: case-insensitive, extension ignored, first match wins.
        byName_.reserve(bag.images().size());
        for (std::size_t i = 0; i < bag.images().size(); ++i) {
            byName_.try_emplace(lowered(stripExtension(bag.images()[i].name)), static_cast<std::int32_t>(i));
        }
    }

    /// nullopt for an empty reference (index -1 without a name: no image at all).
    std::optional<std::int32_t> resolve(const ImageRef& ref)
    {
        std::int32_t index = -1;
        if (ref.index >= 0) {
            index = static_cast<std::size_t>(ref.index) < used_.size() ? ref.index : -1;
        } else if (ref.name.empty()) {
            return std::nullopt;
        } else if (const auto it = byName_.find(lowered(stripExtension(ref.name))); it != byName_.end()) {
            index = it->second;
        }
        if (index < 0) {
            ++unresolved_;
        } else {
            used_[static_cast<std::size_t>(index)] = true;
        }
        return index;
    }

    std::int32_t resolveOr(const ImageRef& ref) { return resolve(ref).value_or(-1); }

    std::vector<std::int32_t> resolveAll(const std::vector<ImageRef>& refs)
    {
        std::vector<std::int32_t> out;
        out.reserve(refs.size());
        for (const auto& r : refs) {
            out.push_back(resolveOr(r));
        }
        return out;
    }

    const std::vector<bool>& used() const { return used_; }
    std::size_t unresolved() const { return unresolved_; }

private:
    std::unordered_map<std::string, std::int32_t> byName_;
    std::vector<bool> used_;
    std::size_t unresolved_ = 0;
};

Sequence stills(std::string name, std::vector<std::int32_t> frames)
{
    Sequence s;
    s.name = std::move(name);
    s.tracks.push_back({{}, std::move(frames), {}});
    return s;
}

Sequence animation(std::string name, const nox::things::Animation& a, Resolver& res)
{
    Sequence s = stills(std::move(name), res.resolveAll(a.frames));
    s.kind = text(a.kind);
    s.frameDelay = a.frameDelay;
    return s;
}

/// Frames per direction, in GridDirection order.
void addDirections(Sequence& s, const std::array<std::vector<ImageRef>, 8>& dirs, Resolver& res)
{
    s.trackKind = TrackKind::Direction;
    for (std::size_t d = 0; d < dirs.size(); ++d) {
        s.tracks.push_back({std::string(kDirectionNames[d]), res.resolveAll(dirs[d]), {}});
    }
}

Sequence directional(std::string name, const nox::things::DirectionalAnimation& a, Resolver& res)
{
    Sequence s;
    s.name = std::move(name);
    s.kind = text(a.kind);
    s.frameDelay = a.frameDelay;
    addDirections(s, a.directions, res);
    return s;
}

std::vector<Sequence> thingSequences(const nox::things::DrawSpec& spec, Resolver& res)
{
    using nox::things::DrawKind;
    std::vector<Sequence> out;
    switch (spec.kind) {
    case DrawKind::None:
        break;
    case DrawKind::Static:
        out.push_back(stills("image", res.resolveAll(spec.images)));
        break;
    case DrawKind::Random:
        out.push_back(stills("images", res.resolveAll(spec.images)));
        break;
    case DrawKind::Animate:
        out.push_back(animation("animation", spec.animations.front(), res));
        break;
    case DrawKind::Conditional:
        for (std::size_t i = 0; i < spec.animations.size(); ++i) {
            out.push_back(animation(std::format("condition_{}", i), spec.animations[i], res));
            out.back().state = static_cast<int>(i);
        }
        break;
    case DrawKind::Vector:
        out.push_back(directional("animation", *spec.directional, res));
        break;
    case DrawKind::Monster:
        for (std::size_t i = 0; i < spec.monsterStates.size(); ++i) {
            if (const auto& st = spec.monsterStates[i]) {
                Sequence s = directional(std::string(nox::things::kMonsterStates[i]), st->animation, res);
                s.state = static_cast<int>(i);
                s.sound = text(st->sound);
                if (s.sound == "NULL") {
                    s.sound.clear();
                }
                if (const auto stored = text(st->name); !stored.empty() && stored != "NULL") {
                    s.info.emplace_back("storedName", stored);
                }
                out.push_back(std::move(s));
            }
        }
        break;
    case DrawKind::AnimateState:
        for (std::size_t i = 0; i < spec.stateAnimations.size(); ++i) {
            if (const auto& a = spec.stateAnimations[i]) {
                const auto stored = text(spec.stateNames[i]);
                out.push_back(animation(stored.empty() || stored == "NULL" ? std::format("slot_{}", i) : stored, *a,
                                        res));
                out.back().state = static_cast<int>(i);
            }
        }
        break;
    case DrawKind::Player:
        for (std::size_t si = 0; si < spec.playerStates.size(); ++si) {
            const auto& st = spec.playerStates[si];
            if (!st) {
                continue;
            }
            const auto part = [&](std::string_view name, const std::optional<nox::things::PlayerPart>& frames) {
                if (!frames) {
                    return;
                }
                Sequence s;
                s.group = std::string(nox::things::kPlayerStates[si]);
                s.name = std::string(name);
                s.kind = text(st->kind);
                s.frameDelay = st->frameDelay;
                s.state = static_cast<int>(si);
                addDirections(s, *frames, res);
                out.push_back(std::move(s));
            };
            part("NAKED", st->naked);
            for (std::size_t a = 0; a < st->armor.size(); ++a) {
                part(nox::things::kArmorParts[a], st->armor[a]);
            }
            for (std::size_t w = 0; w < st->weapons.size(); ++w) {
                part(nox::things::kWeaponParts[w], st->weapons[w]);
            }
        }
        break;
    }
    return out;
}

enum Cat {
    kMonsters,
    kPlayers,
    kWeapons,
    kArmor,
    kItems,
    kMissiles,
    kDoors,
    kGenerators,
    kScenery,
    kEffects,
    kFloors,
    kEdges,
    kWalls,
    kSpells,
    kAbilities,
    kImag,
    kOther,
    kCatCount
};

constexpr std::array<std::string_view, kCatCount> kCategoryNames = {
    "Monsters",      "Players",      "Weapons",       "Armor",  "Items",     "Missiles",
    "Doors",         "Generators",   "Scenery",       "Effects & misc",      "Floors",
    "Edges",         "Walls",        "Spell icons",   "Ability icons",       "GUI images (IMAG)",
    "Other bag images"};

Cat thingCategory(const nox::things::ThingDef& t, nox::things::DrawKind kind)
{
    const std::uint32_t c = nox::things::thingClasses(t);
    if (kind == nox::things::DrawKind::Player || (c & ObjectClass::Player)) {
        return kPlayers;
    }
    if (c & ObjectClass::Monster) {
        return kMonsters;
    }
    if (c & ObjectClass::MonsterGenerator) {
        return kGenerators;
    }
    if (c & ObjectClass::Weapon) {
        return kWeapons;
    }
    if (c & ObjectClass::Armor) {
        return kArmor;
    }
    if (c & ObjectClass::Missile) {
        return kMissiles;
    }
    if (c & ObjectClass::Door) {
        return kDoors;
    }
    if ((c & (ObjectClass::Food | ObjectClass::Key | ObjectClass::InfoBook | ObjectClass::Treasure |
              ObjectClass::Wand | ObjectClass::Readable | ObjectClass::Flag | ObjectClass::Pickup)) ||
        t.attribute("WEIGHT")) {
        return kItems;
    }
    if (c & (ObjectClass::Immobile | ObjectClass::Obstacle | ObjectClass::Light | ObjectClass::Hole |
             ObjectClass::Elevator | ObjectClass::ElevatorShaft | ObjectClass::Transporter | ObjectClass::Exit |
             ObjectClass::Trigger | ObjectClass::Fire)) {
        return kScenery;
    }
    return kEffects;
}

std::int32_t firstFrame(const std::vector<Sequence>& seqs)
{
    // Prefer a south-facing frame, as the game's still images do.
    for (const auto& s : seqs) {
        for (const auto& t : s.tracks) {
            if ((s.trackKind != TrackKind::Direction || t.name == "S") && !t.frames.empty() && t.frames.front() >= 0) {
                return t.frames.front();
            }
        }
    }
    for (const auto& s : seqs) {
        for (const auto& t : s.tracks) {
            for (auto f : t.frames) {
                if (f >= 0) {
                    return f;
                }
            }
        }
    }
    return -1;
}

/// Unreferenced images are grouped by name without extension and trailing digits.
std::string groupName(std::string_view imageName)
{
    std::string s = stripExtension(imageName);
    while (!s.empty() && (std::isdigit(static_cast<unsigned char>(s.back())) || s.back() == '_' || s.back() == '-' ||
                          s.back() == ' ')) {
        s.pop_back();
    }
    return s.empty() ? "(numbered)" : s;
}

void addInfo(Sequence& s, std::string key, long long value) { s.info.emplace_back(std::move(key), std::to_string(value)); }

} // namespace

std::string_view trackKindName(TrackKind kind)
{
    switch (kind) {
    case TrackKind::None:
        return "none";
    case TrackKind::Direction:
        return "direction";
    case TrackKind::Variant:
        return "variant";
    case TrackKind::Slot:
        return "slot";
    }
    return "none";
}

std::string Sequence::path() const { return group.empty() ? name : group + "/" + name; }

std::size_t Sequence::imageCount() const
{
    std::size_t n = 0;
    for (const auto& t : tracks) {
        n += t.frames.size();
    }
    return n;
}

std::size_t Entry::imageCount() const
{
    std::size_t n = 0;
    for (const auto& s : sequences) {
        n += s.imageCount();
    }
    return n;
}

std::size_t Category::imageCount() const
{
    std::size_t n = 0;
    for (const auto& e : entries) {
        n += e.imageCount();
    }
    return n;
}

const Category* Catalog::findCategory(std::string_view name) const
{
    const std::string wanted = lowered(name);
    for (const auto& c : categories) {
        if (lowered(c.name) == wanted) {
            return &c;
        }
    }
    return nullptr;
}

const Entry* Catalog::findEntry(std::string_view name, const Category** category, bool* ambiguous) const
{
    std::string_view catName;
    if (const auto slash = name.find('/'); slash != std::string_view::npos) {
        catName = name.substr(0, slash);
        name = name.substr(slash + 1);
    }
    const std::string wanted = lowered(name);
    const Entry* found = nullptr;
    const Category* foundCat = nullptr;
    bool twice = false;
    for (const auto& c : categories) {
        if (!catName.empty() && lowered(c.name) != lowered(catName)) {
            continue;
        }
        for (const auto& e : c.entries) {
            if (lowered(e.name) == wanted) {
                twice = twice || found;
                found = &e;
                foundCat = &c;
            }
        }
    }
    if (ambiguous) {
        *ambiguous = twice;
    }
    if (twice) {
        return nullptr;
    }
    if (category) {
        *category = foundCat;
    }
    return found;
}

Catalog buildCatalog(const nox::things::ThingDb& things, const nox::video::VideoBag& bag)
{
    Resolver res(bag);
    std::array<std::vector<Entry>, kCatCount> cats;

    for (std::size_t i = 0; i < things.things.size(); ++i) {
        const auto& t = things.things[i];
        if (!t.draw) {
            continue;
        }
        nox::things::DrawSpec spec;
        try {
            spec = nox::things::decodeDraw(*t.draw);
        } catch (const std::exception&) {
            continue; // a payload noxcore can't decode draws nothing we could show
        }
        Entry e;
        e.name = t.name;
        e.source = "THNG";
        e.drawType = t.draw->type;
        e.thingId = static_cast<int>(i) + 1;
        e.attributes = t.attributes;
        e.sequences = thingSequences(spec, res);
        if (e.imageCount() == 0) {
            continue;
        }
        e.icon = firstFrame(e.sequences);
        cats[thingCategory(t, spec.kind)].push_back(std::move(e));
    }

    for (const auto& f : things.floors) {
        Entry e;
        e.name = f.name;
        e.source = "FLOR";
        Sequence s = stills("tiles", res.resolveAll(f.images));
        addInfo(s, "cols", f.cols);
        addInfo(s, "rows", f.rows);
        addInfo(s, "frames", f.frames);
        e.sequences.push_back(std::move(s));
        e.icon = firstFrame(e.sequences);
        cats[kFloors].push_back(std::move(e));
    }

    for (const auto& ed : things.edges) {
        Entry e;
        e.name = ed.name;
        e.source = "EDGE";
        Sequence s = stills("tiles", res.resolveAll(ed.images));
        addInfo(s, "cols", ed.cols);
        addInfo(s, "rows", ed.rows);
        addInfo(s, "frames", ed.frames);
        e.sequences.push_back(std::move(s));
        e.icon = firstFrame(e.sequences);
        cats[kEdges].push_back(std::move(e));
    }

    for (const auto& w : things.walls) {
        Entry e;
        e.name = w.name;
        e.source = "WALL";
        for (std::size_t d = 0; d < w.directions.size(); ++d) {
            if (w.directions[d].empty()) {
                continue;
            }
            Sequence s;
            s.name = std::format("dir_{:02}", d);
            s.state = static_cast<int>(d);
            s.trackKind = TrackKind::Variant;
            for (std::size_t v = 0; v < w.directions[d].size(); ++v) {
                Track tr;
                tr.name = std::format("v{}", v);
                for (const auto& img : w.directions[d][v]) {
                    tr.frames.push_back(res.resolveOr(img.image));
                    tr.drawOffsets.push_back({img.x, img.y});
                }
                s.tracks.push_back(std::move(tr));
            }
            e.sequences.push_back(std::move(s));
        }
        if (e.imageCount() == 0) {
            continue;
        }
        e.icon = firstFrame(e.sequences);
        cats[kWalls].push_back(std::move(e));
    }

    for (const auto& sp : things.spells) {
        Entry e;
        e.name = sp.id;
        e.source = "SPELL";
        Sequence s;
        s.name = "icons";
        s.trackKind = TrackKind::Slot;
        if (const auto i = res.resolve(sp.icon)) {
            s.tracks.push_back({"icon", {*i}, {}});
        }
        if (const auto i = res.resolve(sp.iconEnabled)) {
            s.tracks.push_back({"enabled", {*i}, {}});
        }
        if (s.tracks.empty()) {
            continue;
        }
        e.sequences.push_back(std::move(s));
        e.icon = firstFrame(e.sequences);
        cats[kSpells].push_back(std::move(e));
    }

    for (const auto& ab : things.abilities) {
        Entry e;
        e.name = ab.name;
        e.source = "ABILITY";
        Sequence s;
        s.name = "icons";
        s.trackKind = TrackKind::Slot;
        for (std::size_t k = 0; k < ab.icons.size(); ++k) {
            if (const auto i = res.resolve(ab.icons[k])) {
                s.tracks.push_back({std::format("icon_{}", k), {*i}, {}});
            }
        }
        if (s.tracks.empty()) {
            continue;
        }
        e.sequences.push_back(std::move(s));
        e.icon = firstFrame(e.sequences);
        cats[kAbilities].push_back(std::move(e));
    }

    for (const auto& im : things.images) {
        Entry e;
        e.name = im.name;
        e.source = "IMAG";
        if (im.image) {
            if (const auto i = res.resolve(*im.image)) {
                e.sequences.push_back(stills("image", {*i}));
            }
        }
        if (im.animation) {
            e.sequences.push_back(animation("animation", *im.animation, res));
        }
        if (e.imageCount() == 0) {
            continue;
        }
        e.icon = firstFrame(e.sequences);
        cats[kImag].push_back(std::move(e));
    }

    // Everything thing.bin didn't reach.
    std::size_t referenced = 0;
    std::map<std::string, std::pair<std::string, std::vector<std::int32_t>>> groups; // by lower-case name
    for (std::size_t i = 0; i < res.used().size(); ++i) {
        if (res.used()[i]) {
            ++referenced;
            continue;
        }
        const std::string name = groupName(bag.images()[i].name);
        auto& g = groups[lowered(name)];
        if (g.first.empty()) {
            g.first = name;
        }
        g.second.push_back(static_cast<std::int32_t>(i));
    }
    for (auto& [key, g] : groups) {
        const auto& [name, frames] = g;
        const std::size_t parts = (frames.size() + kMaxGroupImages - 1) / kMaxGroupImages;
        for (std::size_t p = 0; p < parts; ++p) {
            Entry e;
            e.name = parts == 1 ? name : std::format("{} (part {})", name, p + 1);
            e.source = "BAG";
            const auto begin = frames.begin() + static_cast<std::ptrdiff_t>(p * kMaxGroupImages);
            const auto end = frames.begin() + static_cast<std::ptrdiff_t>(std::min(frames.size(), (p + 1) * kMaxGroupImages));
            e.sequences.push_back(stills("images", {begin, end}));
            e.icon = e.sequences.front().tracks.front().frames.front();
            cats[kOther].push_back(std::move(e));
        }
    }

    Catalog cat;
    cat.bagImages = bag.images().size();
    cat.referencedImages = referenced;
    cat.unresolvedRefs = res.unresolved();
    for (std::size_t c = 0; c < kCatCount; ++c) {
        auto& entries = cats[c];
        std::stable_sort(entries.begin(), entries.end(),
                         [](const Entry& a, const Entry& b) { return lowered(a.name) < lowered(b.name); });
        cat.categories.push_back({std::string(kCategoryNames[c]), std::move(entries)});
    }
    return cat;
}

} // namespace videotools
