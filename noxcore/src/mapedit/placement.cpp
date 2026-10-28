// The map editor's object placement (placement.hpp): new objects as the game's save writes a
// fresh thing, facing, snapping, the placement op and the scatter brush.
#include "noxcore/mapedit/placement.hpp"

#include "doc_types.hpp"

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/map/objects.hpp"
#include "noxcore/map/xfer.hpp"
#include "noxcore/server/object_save.hpp"
#include "noxcore/server/world.hpp"
#include "noxcore/things/thing_db.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numbers>

namespace nox::mapedit {

namespace {

namespace xf = map::xfer;

constexpr float kPi = std::numbers::pi_v<float>;

/// The creatures' 8 grid steps in compass order from east (0 degrees) turning clockwise on screen
/// (the world's y grows downwards): the loader's direction table (xferDirectionToAngle) read in
/// order of its angles 0, 0x20 ... 0xE0.
constexpr std::array<std::array<std::int32_t, 2>, 8> kSteps = {{{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}}};

int stepIndex(const std::array<std::int32_t, 2>& d)
{
    for (std::size_t i = 0; i < kSteps.size(); ++i) {
        if (kSteps[i] == d) return static_cast<int>(i);
    }
    return 0; // ((0, 0) faces east, as the table's middle entry gives angle 0)
}

float wrapDegrees(float d)
{
    d = std::fmod(d, 360.0f);
    return d < 0 ? d + 360.0f : d;
}

} // namespace

// --- The factory -----------------------------------------------------------------------------------

struct ObjectFactory::Impl {
    server::World world;
    const things::ModifierDb* modifiers = nullptr;
    std::map<std::string, DocObject, std::less<>> made; ///< each type's record, made once
    std::map<std::string, std::uint32_t, std::less<>> flags; ///< each type's fresh flags (masked as saved)
};

ObjectFactory::ObjectFactory(const things::ThingDb& things, const things::ModifierDb* modifiers, const things::MonsterDb* monsters,
                             const things::GameData* gameData)
    : things_(things)
    , impl_(std::make_unique<Impl>())
{
    auto& w = impl_->world;
    // The editor build's game flag alone (0x200000, solo numbers: gamedata's SOLO lines): neither
    // a network game (createAt would add NO_COLLIDE to items) nor a quest (durabilities and
    // charges multiplied). The creatures' monster.bin numbers are the caller's: every stock map's
    // creatures carry the solo table's (the editor passes the solo monster.bin).
    w.gameFlags = server::GameFlag::SoloOther;
    w.gameData = gameData;
    w.modifierDb = modifiers;
    w.monsterDb = monsters;
    w.thingDb = &things;
    impl_->modifiers = modifiers;
}

ObjectFactory::~ObjectFactory() = default;

DocObject ObjectFactory::make(std::string_view type, float x, float y) const
{
    auto it = impl_->made.find(type);
    if (it == impl_->made.end()) {
        const detail::TypeXfer tx = detail::typeXfer(things_, type); // (throws for an unknown type)
        auto& w = impl_->world;
        server::ServerObject* o = w.createAt(things_, type, 0, 0, w.frameNow);
        if (!o) {
            throw FormatError("map editor: can't make a " + std::string(type));
        }
        const std::uint32_t extent = o->extent;
        impl_->flags[std::string(type)] = o->flags & 0x11408162u; // (the save's mask)
        std::vector<std::uint8_t> bytes;
        try {
            bytes = server::objectRecord(w, things_, impl_->modifiers, w.frameNow, *o);
        } catch (const std::exception& ex) {
            w.objects().erase(extent);
            w.takeNew();
            throw FormatError("map editor: a new " + std::string(type) + " can't be written: " + ex.what());
        }
        w.objects().erase(extent);
        w.takeNew();
        const map::ObjectXfer rec = map::ObjectXfer::parse(bytes);
        DocObject d;
        d.type = std::string(type);
        d.xferVersion = rec.xferVersion;
        d.header = rec.header;
        d.header.extent = 0;
        d.header.scriptId = 0; // (assigned when the map loads)
        d.header.x = 0;
        d.header.y = 0;
        d.data = xf::decode(tx.xfer, rec.xferVersion, rec.extra, tx.info, rec.extraOffset());
        // The AI state as a non-host writes it (XFer_ActionData: "present" is set only on the
        // host), as every stock map's monsters have it.
        const auto plain = [](xf::ActionData& a) {
            a.present = 0;
            a.body.reset();
        };
        if (auto* m = std::get_if<xf::Monster>(&d.data)) plain(m->actionData);
        if (auto* n = std::get_if<xf::Npc>(&d.data)) plain(n->actionData);
        if (auto* g = std::get_if<xf::MonsterGenerator>(&d.data)) {
            (void)g; // (a fresh generator has no templates)
        }
        it = impl_->made.emplace(std::string(type), std::move(d)).first;
    }
    DocObject out = it->second;
    out.header.x = x;
    out.header.y = y;
    return out;
}

std::uint32_t ObjectFactory::freshFlags(std::string_view type) const
{
    if (!impl_->flags.contains(type)) {
        (void)make(type, 0, 0);
    }
    const auto it = impl_->flags.find(type);
    return it != impl_->flags.end() ? it->second : 0x01000000u;
}

// --- Facing --------------------------------------------------------------------------------------

FacingKind facingKind(const DocObject& o)
{
    if (std::holds_alternative<xf::Door>(o.data)) return FacingKind::Door;
    if (std::holds_alternative<xf::Monster>(o.data) || std::holds_alternative<xf::Npc>(o.data)) return FacingKind::Creature;
    if (std::holds_alternative<xf::Sentry>(o.data)) return FacingKind::Sentry;
    return FacingKind::None;
}

std::optional<float> facingDegrees(const DocObject& o)
{
    if (const auto* d = std::get_if<xf::Door>(&o.data)) return static_cast<float>(d->direction & 31) * (360.0f / 32);
    if (const auto* m = std::get_if<xf::Monster>(&o.data)) return static_cast<float>(stepIndex(m->direction)) * 45.0f;
    if (const auto* n = std::get_if<xf::Npc>(&o.data)) return static_cast<float>(stepIndex(n->direction)) * 45.0f;
    if (const auto* s = std::get_if<xf::Sentry>(&o.data)) return wrapDegrees(s->basePosition * 180.0f / kPi);
    return std::nullopt;
}

bool setFacingDegrees(DocObject& o, float degrees)
{
    degrees = wrapDegrees(degrees);
    if (auto* d = std::get_if<xf::Door>(&o.data)) {
        // The four ways a door hangs (every stock door: 0, 8, 16, 24, both copies alike).
        const auto q = static_cast<std::uint32_t>(std::lround(degrees / 90.0f)) % 4;
        d->direction = d->currentDirection = q * 8;
        return true;
    }
    const auto step = [&] { return kSteps[static_cast<std::size_t>(std::lround(degrees / 45.0f)) % 8]; };
    if (auto* m = std::get_if<xf::Monster>(&o.data)) {
        m->direction = step();
        return true;
    }
    if (auto* n = std::get_if<xf::Npc>(&o.data)) {
        n->direction = step();
        return true;
    }
    if (auto* s = std::get_if<xf::Sentry>(&o.data)) {
        const float snapped = std::round(degrees / 5.0f) * 5.0f;
        s->basePosition = wrapDegrees(snapped) * kPi / 180.0f;
        s->currentPosition = s->basePosition;
        return true;
    }
    return false;
}

bool rotateFacing(DocObject& o, int steps)
{
    const auto now = facingDegrees(o);
    if (!now) return false;
    const float step = facingKind(o) == FacingKind::Door ? 90.0f : facingKind(o) == FacingKind::Creature ? 45.0f : 5.0f;
    return setFacingDegrees(o, *now + static_cast<float>(steps) * step);
}

// --- Snapping --------------------------------------------------------------------------------------

WorldPoint snapPoint(WorldPoint p, Snap snap, float custom, bool door)
{
    const float step = door || snap == Snap::Grid ? kWallCellSize
                       : snap == Snap::HalfGrid   ? kWallCellSize / 2
                       : snap == Snap::Custom     ? std::max(custom, 1.0f)
                                                  : 0.0f;
    if (step <= 0) return p;
    return {std::round(p.x / step) * step, std::round(p.y / step) * step};
}

bool snapsAsDoor(const DocObject& o)
{
    return std::holds_alternative<xf::Door>(o.data);
}

// --- Placement -------------------------------------------------------------------------------------

Op placeObjectsOp(const MapDocument& doc, std::vector<DocObject> objects, Selection* placed, std::string label)
{
    Op op;
    AddObjects add;
    Selection sel;
    ObjId nextId = doc.nextObjId;
    std::uint32_t nextExtent = doc.nextFreeExtent();
    std::uint64_t order = 0;
    bool anyRoot = false;
    for (const auto& [id, o] : doc.objects) {
        if (o.parent == 0) {
            order = std::max(order, o.order);
            anyRoot = true;
        }
    }
    std::vector<ObjId> idOf;
    std::size_t roots = 0;
    for (auto& o : objects) {
        const ObjId id = nextId++;
        idOf.push_back(id);
        if (o.parent != 0) {
            if (o.parent > idOf.size() - 1) {
                throw FormatError("map editor: an item placed before its owner");
            }
            o.parent = idOf[o.parent - 1];
        } else {
            o.order = anyRoot ? ++order : order++;
            anyRoot = true;
            sel.objects.insert(id);
            ++roots;
        }
        o.header.extent = nextExtent++;
        o.header.scriptId = 0;
        o.tocId = 0;
        o.padding.clear();
        o.sizeHi = 0;
        add.objects.push_back({id, std::move(o)});
    }
    if (!add.objects.empty()) op.parts.push_back(std::move(add));
    if (label.empty()) {
        label = roots == 1 && !op.parts.empty() ? "Place " + std::get<AddObjects>(op.parts.front()).objects.front().object.type
                                                : "Place " + std::to_string(roots) + " objects";
    }
    op.label = std::move(label);
    if (placed) *placed = std::move(sel);
    return op;
}

Op setObjectsOp(const MapDocument& doc, const std::vector<std::pair<ObjId, DocObject>>& changed, std::string label)
{
    SetObjects set;
    for (const auto& [id, o] : changed) {
        if (doc.objects.contains(id)) set.objects.push_back({id, o});
    }
    Op op;
    op.label = std::move(label);
    if (!set.objects.empty()) op.parts.push_back(std::move(set));
    return op;
}

// --- Scatter -------------------------------------------------------------------------------------

float scatterRandom(std::uint64_t seed, std::uint64_t n)
{
    // splitmix64 of (seed, n): the same numbers on every compiler and platform.
    std::uint64_t z = seed * 0x9E3779B97F4A7C15ull + n * 0xBF58476D1CE4E5B9ull + 0x94D049BB133111EBull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<float>(z >> 40) / static_cast<float>(1ull << 24);
}

ScatterStroke::ScatterStroke(const MapDocument& doc, const ObjectFactory& factory, ScatterBrush brush)
    : doc_(doc)
    , factory_(factory)
    , brush_(std::move(brush))
{
    for (const auto& [id, o] : doc.objects) {
        if (o.parent == 0) taken_.push_back({o.header.x, o.header.y});
    }
}

std::vector<DocObject> ScatterStroke::dab(WorldPoint at)
{
    std::vector<DocObject> added;
    float total = 0;
    for (const auto& e : brush_.types) total += std::max(e.weight, 0.0f);
    if (brush_.types.empty() || total <= 0 || brush_.radius <= 0) return added;
    const float area = kPi * brush_.radius * brush_.radius;
    const int wanted = std::max(1, static_cast<int>(std::lround(brush_.density * area / 1058.0f)));
    const float spacing2 = brush_.spacing * brush_.spacing;
    // Darts: up to 8 tries per object wanted, each a point in the disc.
    const std::uint64_t seed = brush_.seed;
    for (int tries = 0, placed = 0; tries < wanted * 8 && placed < wanted; ++tries) {
        const float a = scatterRandom(seed, counter_++) * 2 * kPi;
        const float r = std::sqrt(scatterRandom(seed, counter_++)) * brush_.radius;
        const WorldPoint p{at.x + std::cos(a) * r, at.y + std::sin(a) * r};
        const float pickAt = scatterRandom(seed, counter_++) * total;
        const float facing = scatterRandom(seed, counter_++) * 360.0f;
        if (p.x < 1 || p.y < 1 || p.x >= kWorldSize - 1 || p.y >= kWorldSize - 1) continue;
        if (std::ranges::any_of(taken_, [&](const WorldPoint& q) { return (q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y) < spacing2; })) continue;
        if (brush_.avoidWalls) {
            if (const auto c = wallCellAt(p.x, p.y); c && doc_.walls.contains(*c)) continue;
        }
        if (brush_.avoidHoles) {
            const auto t = tileAt(p.x, p.y);
            if (!t || !doc_.floor.tile(t->gx, t->gy)) continue;
        }
        float acc = 0;
        const ScatterBrush::Entry* pick = &brush_.types.back();
        for (const auto& e : brush_.types) {
            acc += std::max(e.weight, 0.0f);
            if (pickAt < acc) {
                pick = &e;
                break;
            }
        }
        DocObject o = factory_.make(pick->type, p.x, p.y);
        if (brush_.randomFacing) setFacingDegrees(o, facing);
        taken_.push_back(p);
        objects_.push_back(o);
        added.push_back(std::move(o));
        ++placed;
    }
    return added;
}

Op ScatterStroke::finish(Selection* placed) const
{
    if (objects_.empty()) return {};
    return placeObjectsOp(doc_, objects_, placed, "Scatter " + std::to_string(objects_.size()) + " objects");
}

} // namespace nox::mapedit
