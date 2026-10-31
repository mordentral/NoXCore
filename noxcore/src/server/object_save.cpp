// The solo save's objects (object_save.hpp): the live world written back as a map's ObjectTOC and
// ObjectData sections (Saver), one thing's record for a .plr's inventory, and a loaded map's
// monsters' enchantments and poison read back after the load.
#include "noxcore/server/object_save.hpp"

#include "noxcore/audio/sound_names.hpp"
#include "noxcore/client/spell_bars.hpp"
#include "noxcore/map/objects.hpp"
#include "noxcore/map/xfer.hpp"
#include "noxcore/server/combat.hpp"
#include "noxcore/server/enchant.hpp"
#include "noxcore/server/host_client.hpp"
#include "noxcore/server/monsters.hpp"
#include "noxcore/server/npc.hpp"
#include "noxcore/server/script_host.hpp"
#include "noxcore/server/spells.hpp"
#include "noxcore/things/field_guide.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/things/spell_ids.hpp"

#include "noxcore/util/bytes.hpp"

#include "../map/ai_actions.hpp"

#include <algorithm>
#include <bit>
#include <tuple>
#include <unordered_map>

namespace nox::server {

namespace {

namespace oc = things::ObjectClass;
namespace xf = map::xfer;

constexpr std::uint32_t kFlagMask = 0x11408162u;    // (the header's flags)
constexpr std::uint32_t kXStatusMask = 0x5Eu;
constexpr std::uint32_t kTransient = 0x400000u;     // (TRANSIENT: decaying, unitSetDecayTime)

// A float's bits; two floats as one 64-bit value (the first low); the same as 8 little-endian bytes.
std::uint32_t bitsOf(float f) { return std::bit_cast<std::uint32_t>(f); }

std::uint64_t pairBits(float a, float b) { return bitsOf(a) | static_cast<std::uint64_t>(bitsOf(b)) << 32; }

std::array<std::uint8_t, 8> pairBytes(float a, float b)
{
    std::array<std::uint8_t, 8> out{};
    const std::uint64_t v = pairBits(a, b);
    for (std::size_t i = 0; i < 8; ++i) {
        out[i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
    return out;
}

/// A string kept with its end (a sign's text, an exit's map) up to its end.
std::string upToNul(const std::string& s) { return s.substr(0, s.find('\0')); }

/// The version each xfer writes (its current one: the readers' highest, map/xfer.cpp's table).
std::uint16_t saveVersion(std::string_view xfer)
{
    static constexpr std::pair<std::string_view, std::uint16_t> kVersions[] = {
        {"TriggerXfer", 0x3D},      {"ElevatorXfer", 0x3D}, {"SentryXfer", 0x3D}, {"AbilityRewardXfer", 0x3D},
        {"WeaponXfer", 0x40},       {"ArmorXfer", 0x3E},    {"ObeliskXfer", 0x3D}, {"ToxicCloudXfer", 0x3D},
        {"RewardMarkerXfer", 0x3F}, {"MonsterXfer", 0x40},  {"NPCXfer", 0x3E},    {"MonsterGeneratorXfer", 0x3F}};
    for (const auto& [name, v] : kVersions) {
        if (xfer == name) {
            return v;
        }
    }
    return 0x3C;
}

// What the save needs of a thing type, worked out once per type (Saver::facts).
struct TypeFacts {
    const things::ThingDef* def = nullptr;
    std::string xfer;
    xf::TypeInfo info;
    std::uint32_t flags = 0; ///< the type's flags as the type reader leaves them
    std::uint32_t clientFlags = 0; ///< a drawable's of the type: FLAGS and ENABLED (no NO_COLLIDE added)
    bool icon = false;       ///< a MENUICON (its index not -1)
    bool clientOnly = false; ///< a client-only type: the client loads it, the server frees it
};

// Writes the world's objects as the exe's map save does; made for one save and dropped.
class Saver {
public:
    Saver(World& world, const things::ThingDb& things, const things::ModifierDb* modifiers, std::uint32_t frame)
        : world_(world)
        , things_(things)
        , modifiers_(modifiers)
        , frame_(frame)
        , types_(things.things.size() + 1)
        , ids_(things.things.size() + 1, 0)
    {
        for (std::size_t i = 0; i < world.loadOrder.size(); ++i) {
            loadIndex_.emplace(world.loadOrder[i], i);
        }
        // Each one's owned list (firstOwned / nextOwned: newest first, unitSetOwner).
        for (const auto& [e, o] : world.objects()) {
            if (o.owner != 0 && o.owner != e) {
                owned_[o.owner].push_back(&o);
            }
        }
        for (auto& [e, list] : owned_) {
            std::ranges::stable_sort(list, [](const ServerObject* a, const ServerObject* b) { return a->ownedOrder > b->ownedOrder; });
        }
    }

    SavedObjects run();
    /// One thing's record alone (its xfer in write mode: the header with how many it holds, then
    /// its own fields), as a .plr's Inventory writes each (plr_rwInventory).
    std::vector<std::uint8_t> record(const ServerObject& o)
    {
        std::size_t held = 0;
        for (const std::uint32_t e : o.inventory) {
            held += find(e) ? 1 : 0;
        }
        return recordData(o, held);
    }

private:
    const TypeFacts* facts(std::uint16_t type);
    const TypeFacts* facts(const ServerObject& o) { return facts(o.type); }
    const ServerObject* find(std::uint32_t extent) const
    {
        const auto it = world_.objects().find(extent);
        return it != world_.objects().end() ? &it->second : nullptr;
    }
    std::uint32_t scriptIdOf(std::uint32_t extent) const
    {
        const ServerObject* o = extent ? find(extent) : nullptr;
        return o ? static_cast<std::uint32_t>(o->scriptId) : 0u;
    }
    std::uint32_t waypointIdOf(std::uint32_t indexPlusOne) const
    {
        const auto& wps = world_.waypoints();
        return indexPlusOne != 0 && indexPlusOne <= wps.size() ? static_cast<std::uint32_t>(wps[indexPlusOne - 1].id) : 0u;
    }

    // Gives a type its TOC id (from 1, in the order met) the first time it's met.
    void assign(std::uint16_t type)
    {
        if (type != 0 && type < ids_.size() && ids_[type] == 0) {
            ids_[type] = ++count_;
        }
    }

    std::vector<ServerObject*> list(bool missiles);
    map::ScriptHandler handler(const ServerObject::ScriptBlock& b) const;
    /// A map record's handlers as the exe writes them back (a function the script lacks: none).
    map::ScriptHandler resolve(const map::ScriptHandler& h) const { return handler({h.unknown, h.function}); }
    void resolve(xf::MonsterScripts& s) const
    {
        for (map::ScriptHandler* h : {&s.enemySighted, &s.death, &s.lookingForEnemy, &s.changeFocus, &s.isHit, &s.retreat, &s.collision,
                                      &s.hearEnemy, &s.endOfWaypoint, &s.lostEnemy}) {
            *h = resolve(*h);
        }
    }
    xf::Modifiers modifierNames(const ServerObject& o) const;
    /// xfer_saveObj: its record, then what it holds (nested), into `out`.
    bool saveObj(const ServerObject& o, std::vector<map::ObjectRecord>& out, const std::vector<const ServerObject*>* items = nullptr);
    std::vector<std::uint8_t> recordData(const ServerObject& o, std::size_t held);
    map::ObjectHeader header(const ServerObject& o, std::size_t held);
    xf::Data data(const ServerObject& o, const TypeFacts& f, const map::ObjectXfer* rec, std::uint16_t version);
    xf::Monster monster(const ServerObject& o, const TypeFacts& f, const xf::Monster* rec);
    xf::Npc npc(const ServerObject& o, const xf::Npc* rec);
    xf::ActionData actionData(const ServerObject& o, const xf::ActionData* rec);
    xf::Buffs buffs(const ServerObject& o);
    xf::MonsterGenerator generator(const ServerObject& o, const xf::MonsterGenerator* rec);
    map::ObjectRecord clientRecord(const World::ClientOnly& o, const TypeFacts& f);

    World& world_;
    const things::ThingDb& things_;
    const things::ModifierDb* modifiers_;
    std::uint32_t frame_;
    std::vector<std::optional<TypeFacts>> types_;
    std::vector<std::uint16_t> ids_; ///< the TOC's, by thing index
    std::uint16_t count_ = 0;        ///< (the TOC's count)
    std::unordered_map<std::uint32_t, std::size_t> loadIndex_;
    std::unordered_map<std::uint32_t, std::vector<const ServerObject*>> owned_;
};

const TypeFacts* Saver::facts(std::uint16_t type)
{
    if (type == 0 || type > things_.things.size()) {
        return nullptr;
    }
    auto& slot = types_[type];
    if (!slot) {
        TypeFacts f;
        const things::ThingDef& t = things_.things[type - 1u];
        f.def = &t;
        const auto x = t.lastAttribute("XFER");
        f.xfer = x ? std::string(*x) : std::string("DefaultXfer");
        f.info = {things::thingClasses(t), things::thingSubclasses(t)};
        // The type reader (read_things_alternative): FLAGS, every type ENABLED, one
        // without a COLLIDE function NO_COLLIDE.
        f.flags |= things::thingFlags(t);
        f.flags |= 0x1000000u;
        f.clientFlags = f.flags;
        const auto collide = t.attribute("COLLIDE");
        if (!collide || collide->starts_with("NoCollide")) {
            f.flags |= 0x40u;
        }
        f.icon = t.menuIcon && t.menuIcon->index != -1;
        // Client-only: not a missile, IMMOBILE, not VISIBLE_ENABLE, NO_COLLIDE, no SHADOW.
        const std::uint32_t c = f.info.classes;
        f.clientOnly = !(c & oc::Missile) && (c & oc::Immobile) && !(c & oc::VisibleEnable) && (f.flags & 0x40u) && !(f.flags & 0x10000u);
        slot = std::move(f);
    }
    return &*slot;
}

std::vector<ServerObject*> Saver::list(bool missiles)
{
    // The exe's object list (or, missiles, its updatable list): what's in the world, new things
    // at the head (unitsNewAddToList moves a frame's batch there, oldest first).
    std::vector<ServerObject*> out;
    for (auto& [extent, o] : world_.objects()) {
        if (o.holder == 0 && ((o.classes & oc::Missile) != 0) == missiles) {
            out.push_back(&o);
        }
    }
    // The sort key: the SaveGameLocation first; then by the frame made, newest first; within a
    // frame the things made in play (by extent) before the map's (in its file's order).
    const auto rank = [&](const ServerObject* o) {
        const auto it = loadIndex_.find(o->extent);
        const bool fromMap = o->extent < kDynamicExtents && it != loadIndex_.end();
        return std::tuple(o->typeName == "SaveGameLocation" ? 0 : 1, -static_cast<std::int64_t>(o->born), fromMap ? 1 : 0,
                          fromMap ? static_cast<std::uint64_t>(it->second) : static_cast<std::uint64_t>(o->extent));
    };
    std::ranges::stable_sort(out, [&](const ServerObject* a, const ServerObject* b) { return rank(a) < rank(b); });
    return out;
}

map::ScriptHandler Saver::handler(const ServerObject::ScriptBlock& b) const
{
    // xferReadScriptHandler (write): version 1, its function's name when the
    // script has it (the block's index isn't -1), the block's flags.
    map::ScriptHandler h;
    h.version = 1;
    if (world_.script && world_.script->resolves(b)) {
        h.function = b.function;
    }
    h.unknown = b.flags;
    return h;
}

xf::Modifiers Saver::modifierNames(const ServerObject& o) const
{
    xf::Modifiers names;
    for (std::size_t i = 0; i < 4; ++i) {
        if (const things::ModifierDef* m = modifiers_ ? modifiers_->modifier(o.modifiers[i]) : nullptr) {
            names[i] = m->name;
        }
    }
    return names;
}

map::ObjectHeader Saver::header(const ServerObject& o, std::size_t held)
{
    const TypeFacts* f = facts(o);
    map::ObjectHeader h;
    h.version = 0x40;
    h.extent = o.fileExtent.number(); // (the exe's Extent: the map's number, 0 for a thing made in play)
    h.scriptId = static_cast<std::uint32_t>(o.scriptId);
    h.x = o.x;
    h.y = o.y;
    const std::uint32_t flags = o.flags | (o.decayAt != 0 ? kTransient : 0u);
    static const std::vector<const ServerObject*> kNone;
    const auto ownedIt = owned_.find(o.extent);
    const std::vector<const ServerObject*>& owned = ownedIt != owned_.end() ? ownedIt->second : kNone;
    // Its pickup handler as it stands (ScriptPickup: the record's, none once it's been picked
    // up, Obj_TryPickup).
    const ServerObject::ScriptBlock& pickup = o.onPickup;
    // Its details when anything differs from its type's.
    const bool pickupScript = world_.script && world_.script->resolves(pickup);
    const bool details = !o.scriptName.empty() || held != 0 || !owned.empty() || o.team != 0 ||
                         (f && ((f->flags ^ flags) & kFlagMask) != 0) || (o.xStatus & kXStatusMask) != 0 || pickupScript;
    h.detailsFlag = details ? 0xFF : 0;
    if (details) {
        map::ObjectHeader::Details d;
        d.flags = flags & kFlagMask;
        d.scriptName = o.scriptName.substr(0, 0xFF);
        d.team = o.team;
        d.inventoryCount = static_cast<std::uint8_t>(held);
        for (const ServerObject* w : owned) {
            const TypeFacts* wf = facts(*w);
            if (!w->gone() && !w->pendingDelete && wf && wf->icon) {
                d.ownedScriptIds.push_back(static_cast<std::uint32_t>(w->scriptId));
            }
        }
        d.statusFlags = o.xStatus & kXStatusMask;
        d.onPickup = handler(pickup);
        // Its timer less the frame: a decaying thing's end, else the frame it was put in the world.
        const std::uint32_t field34 = o.decayAt != 0 ? o.decayAt : o.timer;
        d.decayFrames = static_cast<std::int32_t>(field34 - frame_);
        h.details = std::move(d);
    }
    return h;
}

template <class T>
const T* decoded(const std::optional<xf::Data>& d)
{
    return d ? std::get_if<T>(&*d) : nullptr;
}

// Its type's xfer fields from the live object; what ours doesn't keep comes from the map record
// it was read from (`rec`), else the type's fresh values.
xf::Data Saver::data(const ServerObject& o, const TypeFacts& f, const map::ObjectXfer* rec, std::uint16_t version)
{
    // The record's own fields, for what ours doesn't keep.
    std::optional<xf::Data> old;
    if (rec && rec->xferVersion >= 0x3C) {
        try {
            old = xf::decode(f.xfer, rec->xferVersion, rec->extra, f.info, rec->extraOffset());
        } catch (const std::exception&) {
        }
    }
    const std::string& x = f.xfer;
    if (x == "SpellPagePedestalXfer") { // (its spell)
        const auto* r = decoded<xf::SpellPagePedestal>(old);
        return xf::SpellPagePedestal{r ? r->spell : 0u};
    }
    if (x == "ReadableXfer") { // (its text and its end)
        return xf::Readable{upToNul(o.text) + '\0'};
    }
    if (x == "ExitXfer") { // (the map's name and its end, then the point on it)
        return xf::Exit{upToNul(o.exitMap) + '\0', o.exitX, o.exitY};
    }
    if (x == "DoorXfer") { // (its direction, lock and whether closed)
        return xf::Door{o.doorDirection, o.lock, o.doorClosed};
    }
    if (x == "TriggerXfer") {
        xf::Trigger t;
        const auto* r = decoded<xf::Trigger>(old);
        t.sizeX = static_cast<std::int32_t>(o.boxWidth); // (__ftol of the box)
        t.sizeY = static_cast<std::int32_t>(o.boxHeight);
        if (r) { // (the colours: not kept)
            t.edgeColor = r->edgeColor;
            t.backColor = r->backColor;
            t.unknown2 = r->unknown2;
        } else { // (one made in play: TriggerCreate's colours)
            t.edgeColor = {90, 90, 90};
            t.backColor = {10, 10, 10};
        }
        const ServerObject::Trigger tr = o.trigger.value_or(ServerObject::Trigger{});
        t.unknown1 = tr.flags;
        t.onPressed = handler(tr.pressed);
        t.onReleased = handler(tr.released);
        t.onCollided = handler(tr.collided);
        t.allowedClasses = tr.allowedClasses;
        t.ignoredClasses = tr.ignoredClasses;
        t.allowedTeam = tr.allowedTeam;
        t.ignoredTeam = tr.ignoredTeam;
        t.state = tr.state;
        t.animFrame = o.animFrame;
        return t;
    }
    if (x == "HoleXfer") {
        if (!o.pit) {
            const auto* r = decoded<xf::Hole>(old);
            xf::Hole h = r ? *r : xf::Hole{};
            h.script = resolve(h.script);
            return h;
        }
        const auto& p = *o.pit;
        return xf::Hole{p.sprung ? 1u : 0u, handler(p.stepped), static_cast<std::int32_t>(p.fallX), static_cast<std::int32_t>(p.fallY), p.opensAt,
                        p.delay};
    }
    if (x == "TransporterXfer") { // (its partner's number, 0 unlinked)
        // The exe's: 0 when the load linked it to nothing, else the number its record named (the
        // u32 the record starts with, any version); one made in play, its partner's extent.
        const auto* r = decoded<xf::Transporter>(old);
        if (!o.transport) {
            return xf::Transporter{r ? r->targetExtent : 0u};
        }
        if (o.transport->link == 0) {
            return xf::Transporter{0u};
        }
        return xf::Transporter{rec && rec->extra.size() >= 4 ? util::u32At(rec->extra, 0) : o.transport->link};
    }
    // An elevator's or shaft's partner as its record named it (the u32 each starts with, any
    // version: the exe writes back the number it read, its partner being a pointer the load
    // linked); one made in play, its partner's extent.
    const auto named = [&] { return rec && rec->extra.size() >= 4 ? util::u32At(rec->extra, 0) : o.elevatorLink; };
    if (x == "ElevatorXfer") { // (its link, level and state)
        return xf::Elevator{named(), static_cast<std::uint32_t>(o.elevatorLevel), o.elevatorState};
    }
    if (x == "ElevatorShaftXfer") {
        return xf::ElevatorShaft{named()};
    }
    if (x == "MoverXfer") {
        if (!o.mover) {
            const auto* r = decoded<xf::Mover>(old);
            return r ? *r : xf::Mover{};
        }
        const auto& m = *o.mover;
        return xf::Mover{static_cast<std::uint32_t>(m.speed), static_cast<std::uint32_t>(m.start), m.carried.number(), m.state,
                         static_cast<std::uint32_t>(m.target), static_cast<std::uint32_t>(m.previous), o.speedBase, o.speedCur};
    }
    if (x == "GlyphXfer") { // (Direction1's low byte, its target point and the spells)
        xf::Glyph g;
        g.direction = static_cast<std::uint8_t>(o.direction);
        if (o.trap) {
            g.targetX = o.trap->x;
            g.targetY = o.trap->y;
            for (int i = 0; i < o.trap->count && i < 5; ++i) {
                g.spells.emplace_back(things::spellName(o.trap->spells[static_cast<std::size_t>(i)]));
            }
        }
        return g;
    }
    if (x == "InvisibleLightXfer") { // (the client's drawable's light: the map's)
        const auto* r = decoded<xf::InvisibleLight>(old);
        return r ? *r : xf::InvisibleLight{};
    }
    if (x == "SentryXfer") {
        if (!o.sentry) {
            const auto* r = decoded<xf::Sentry>(old);
            return r ? *r : xf::Sentry{};
        }
        return xf::Sentry{o.sentry->start, o.sentry->speed, o.sentry->angle};
    }
    // A book's entry: the reward generator's, else (World::load doesn't read a
    // map book's yet) the map's.
    if (x == "SpellRewardXfer") {
        const auto* r = decoded<xf::SpellReward>(old);
        return xf::SpellReward{o.bookEntry == 0 && r ? r->spell : std::string(things::spellName(o.bookEntry))};
    }
    if (x == "AbilityRewardXfer") {
        const auto* r = decoded<xf::AbilityReward>(old);
        if (o.bookEntry == 0 && r) {
            return *r;
        }
        return xf::AbilityReward{o.bookEntry < client::kAbilityNames.size() ? std::string(client::kAbilityNames[o.bookEntry]) : std::string()};
    }
    if (x == "FieldGuideXfer") {
        const auto* r = decoded<xf::FieldGuide>(old);
        return xf::FieldGuide{o.guideCreature.empty() && r ? r->creature : upToNul(o.guideCreature)};
    }
    if (x == "WeaponXfer") {
        xf::Weapon w;
        w.modifiers = modifierNames(o);
        if ((f.info.classes & oc::Wand) && (f.info.subclasses & things::ObjectSubclass::StaffWands)) {
            w.charges = xf::Weapon::Charges{o.charges, o.maxCharges, static_cast<std::uint32_t>(o.chargePercent)};
        }
        w.health = o.health; // (unitGetHP)
        w.unknown2 = o.itemFlags;
        return w;
    }
    if (x == "ArmorXfer") {
        xf::Armor a;
        a.modifiers = modifierNames(o);
        a.health = o.health;
        a.unknown2 = o.itemFlags;
        return a;
    }
    if (x == "AmmoXfer") {
        return xf::Ammo{modifierNames(o), o.charges, o.maxCharges};
    }
    if (x == "TeamXfer") {
        return xf::Team{modifierNames(o)};
    }
    if (x == "GoldXfer") {
        return xf::Gold{o.goldAmount};
    }
    if (x == "ObeliskXfer") {
        // The byte: 0, or in a solo game 1 when its client's static drawable of the Extent
        // (Cli_GetStaticDrawableByNetCode: NetCode32 is a net code less its 0x8000) is on the
        // client's minimap list. With no host client here, the map's byte is kept.
        const auto* r = decoded<xf::Obelisk>(old);
        std::uint8_t known = r ? r->minimapKnown : std::uint8_t{0};
        if (world_.hostClient) {
            known = (world_.gameFlags & GameFlag::Solo) && o.fileExtent.number() <= 0x7FFFu &&
                            world_.hostClient->minimapMarked(static_cast<std::uint16_t>(o.fileExtent.number() | 0x8000u))
                        ? std::uint8_t{1}
                        : std::uint8_t{0};
        }
        return xf::Obelisk{static_cast<std::uint32_t>(o.value), known};
    }
    if (x == "ToxicCloudXfer") {
        return xf::ToxicCloud{static_cast<std::uint32_t>(o.value)};
    }
    if (x == "RewardMarkerXfer") {
        const auto* r = decoded<xf::RewardMarker>(old);
        xf::RewardMarker m;
        if (r) {
            m.unknown = r->unknown; // (not kept)
        }
        if (o.rewardMarker) {
            const auto& rm = *o.rewardMarker;
            m.rewardType = rm.kinds;
            m.unknown1 = rm.listed;
            for (std::uint32_t i = 0; i < rm.spells.size(); ++i) {
                if (rm.spells[i]) {
                    m.spells.emplace_back(things::spellName(i));
                }
            }
            for (std::size_t i = 0; i < rm.abilities.size(); ++i) {
                if (rm.abilities[i]) {
                    m.abilities.emplace_back(client::kAbilityNames[i]);
                }
            }
            for (std::size_t i = 1; i < rm.guides.size(); ++i) { // (guideNameByN)
                if (rm.guides[i] && i - 1 < things::kGuideNames.size()) {
                    m.creatures.emplace_back(things::kGuideNames[i - 1]);
                }
            }
            m.activateChance = rm.chance;
            m.rareOrSpecial = rm.flags;
        } else if (r) {
            m = *r;
        }
        return m;
    }
    if (x == "MonsterXfer") {
        return monster(o, f, decoded<xf::Monster>(old));
    }
    if (x == "NPCXfer") {
        return npc(o, decoded<xf::Npc>(old));
    }
    if (x == "MonsterGeneratorXfer") {
        return generator(o, decoded<xf::MonsterGenerator>(old));
    }
    (void)version;
    return xf::Default{};
}

xf::Buffs Saver::buffs(const ServerObject& o)
{
    // XFer_ReadMonsterBuffs (write): version 2, its enchantments in the write
    // order (kEnchantOrder): name, power, frames left, the Shield's strength (its spell's value; 100 without).
    xf::Buffs b;
    b.version = 2;
    for (const int e : kEnchantOrder) {
        if (!(o.buffs & (1u << e))) {
            continue;
        }
        xf::Buffs::Buff buff;
        buff.enchantment = std::string(kEnchantNames[e]);
        buff.power = o.buffPower[static_cast<std::size_t>(e)];
        buff.timer = o.buffTime[static_cast<std::size_t>(e)];
        if (e == Enchant::Shield) {
            buff.shieldHealth = 100;
            for (const DurationSpell& s : world_.durationSpells()) {
                if (!s.cancelled && s.spell == 0x33 && s.target != 0 && s.target == o.extent) {
                    buff.shieldHealth = static_cast<std::uint32_t>(s.value);
                    break;
                }
            }
        }
        b.buffs.push_back(std::move(buff));
    }
    return b;
}

xf::ActionData Saver::actionData(const ServerObject& o, const xf::ActionData* rec)
{
    // XFer_ActionData (write): version 4, present (a host's), its AI's state.
    xf::ActionData a;
    a.version = 4;
    a.present = 1;
    const auto& m = *o.monster;
    const xf::ActionData::Body* r = rec && rec->body ? &*rec->body : nullptr;
    xf::ActionData::Body b;
    b.saveFrame = frame_;
    b.unknown1 = 0;
    for (int i = 0; i < m.pathCount && i < 32; ++i) {
        const auto& [px, py] = m.path[static_cast<std::size_t>(i)];
        b.unknownList1.push_back(pairBytes(px, py));
    }
    b.unknown2 = static_cast<std::uint32_t>(m.pathIndex);
    b.unknown3 = pairBytes(m.pathToX, m.pathToY);
    b.frame1 = m.pathAt;
    b.unknown4 = m.pathStatus;
    for (int i = 0; i < m.waypointCount && i < 16; ++i) {          // (each one's id)
        b.waypoints.push_back(waypointIdOf(m.waypoints[static_cast<std::size_t>(i)]));
    }
    b.unknown5 = static_cast<std::uint32_t>(m.waypointIndex);
    b.unknown6 = pairBytes(m.waypointsToX, m.waypointsToY);
    b.unknown7 = static_cast<std::uint32_t>(m.homeDirection);
    b.unknown8 = pairBytes(m.homeX, m.homeY);
    b.sound = std::string(audio::soundName(m.heard));
    b.unknown9 = pairBytes(m.heardX, m.heardY);
    b.frame2 = m.heardAt;
    b.unknown10 = {m.animFrame, m.animDelay, m.animDone};
    b.frame3 = m.field1f0;
    b.unknown12 = pairBytes(m.stuckX, m.stuckY);
    // (The u32 after them is the waypoints' loop count: their count, 0 for none.)
    b.unknown13 = static_cast<std::uint32_t>(std::max(0, std::min(m.waypointCount, 16)));
    b.frame4 = m.sightedSoundAt;
    b.frame5 = m.blockedAt;
    for (int i = 0; i <= m.top && i < 24; ++i) {
        const auto& it = m.stack[static_cast<std::size_t>(i)];
        xf::AiActionEntry e;
        const bool known = 0 <= it.action && static_cast<std::size_t>(it.action) < map::detail::kAiActionCount;
        const map::detail::AiAction def = known ? map::detail::kAiActions[it.action] : map::detail::AiAction{};
        e.action = std::string(def.name);
        for (std::uint8_t k = 0; k < def.argCount; ++k) {
            xf::AiActionEntry::Arg arg;
            arg.type = def.argTypes[k];
            const std::uint32_t lo = it.args[2u * k];
            const std::uint32_t hi = it.args[2u * k + 1u];
            switch (arg.type) {
            case 0: arg.value = lo | static_cast<std::uint64_t>(hi) << 32; break; // (a place)
            case 1: arg.value = scriptIdOf(lo); break;                              // (an object: its script id)
            case 2: arg.value = waypointIdOf(lo); break;                            // (a waypoint: its id)
            case 7: arg.value = lo & 0xFFu; break;
            default: arg.value = lo; break;
            }
            e.args.push_back(arg);
        }
        e.unknown = it.started ? 1u : 0u;
        b.stack.push_back(std::move(e));
    }
    for (std::size_t i = 0; i < m.seenCount && i < m.seen.size(); ++i) {
        b.unknownList2.push_back(scriptIdOf(m.seen[i]));
    }
    b.unknown14 = scriptIdOf(m.enemy);
    b.frame6 = m.lookedAt;
    // The dialog (dialog.hpp): its start and end functions (-1 none), type,
    // answer and portrait (a u8 of its length, then that many letters).
    b.unknown15 = static_cast<std::uint32_t>(m.dialogStart);
    b.unknown16 = static_cast<std::uint32_t>(m.dialogEnd);
    b.unknown17 = m.dialogType;
    b.unknown18 = m.dialogAnswer;
    b.unknownName = m.portrait.substr(0, m.portrait.size() & 0xFFu);
    xf::ActionData::Body::V4 v;
    if (r && r->v4) {
        v.unknown1 = r->v4->unknown1; // (not kept)
    }
    v.scriptId1 = scriptIdOf(m.heardFrom);
    v.unknown2 = m.deadFrames;
    v.frames1 = {m.movedAwayAt, m.nextMelee, m.hurtHandledAt, m.hurtSoundAt};
    v.frames2 = {m.idleSoundAt, m.tauntSoundAt};
    v.unknown3 = bitsOf(m.nearestPlayer);
    v.frame3 = m.actionFrame;
    v.unknown4 = m.stamina;
    v.scriptId2 = scriptIdOf(m.lastEnemy);
    v.frames4 = {m.nextLook, m.reachCheckedAt};
    v.scriptId3 = scriptIdOf(m.focus);
    for (std::size_t i = 0; i < m.transits.size() && i < 8; ++i) {
        v.scriptIds.push_back(scriptIdOf(m.transits[i]));
    }
    b.v4 = std::move(v);
    a.body = std::move(b);
    // The write mode runs AssignIfGreater with the time since the save (0) on
    // these too: once saved, each is at least 1 (the live monster's, after its fields went out).
    auto& live = const_cast<ServerObject::Monster&>(m);
    for (std::uint32_t* f : {&live.sightedSoundAt, &live.blockedAt, &live.lookedAt, &live.movedAwayAt, &live.nextMelee, &live.hurtHandledAt,
                             &live.hurtSoundAt, &live.idleSoundAt, &live.tauntSoundAt, &live.actionFrame, &live.nextLook, &live.reachCheckedAt}) {
        if (static_cast<std::int32_t>(*f) < 1) {
            *f = 1;
        }
    }
    return a;
}

// MonsterXfer's write from the live monster: its scripts, AI numbers, spells, start action and
// saved AI state, a shopkeeper's shop, colours and voice where the subclass has them. With no AI
// state on the object, the map's own record goes back as it was.
xf::Monster Saver::monster(const ServerObject& o, const TypeFacts& f, const xf::Monster* rec)
{
    if (!o.monster) {
        if (rec) {
            xf::Monster x = *rec;
            resolve(x.scripts);
            return x;
        }
        throw std::runtime_error("object save: a MonsterXfer thing without its AI state");
    }
    const auto& m = *o.monster;
    xf::Monster x;
    // Facing as the -1..1 grid step (dx, dy) the format stores.
    const auto [dx, dy] = gridDirection(static_cast<std::int16_t>(o.direction)); // (xferIndexedDirection)
    x.direction = {dx, dy};
    // Its script handlers as map records (handler()).
    x.scripts.enemySighted = handler(m.onEnemySighted);
    x.scripts.death = handler(m.onDeath);
    x.scripts.unknown305 = static_cast<std::uint16_t>(m.lookDelay);
    x.scripts.lookingForEnemy = handler(m.onLookingForEnemy);
    x.scripts.changeFocus = handler(m.onChangeFocus);
    x.scripts.isHit = handler(m.onIsHit);
    x.scripts.retreat = handler(m.onRetreat);
    x.scripts.collision = handler(m.onCollision);
    x.scripts.hearEnemy = handler(m.onHearEnemy);
    x.scripts.endOfWaypoint = handler(m.onEndOfWaypoint);
    x.scripts.lostEnemy = handler(m.onLostEnemy);
    x.unknown1 = 0;
    x.unknown333 = m.field333;
    x.statusFlags = m.status;
    x.unknown338 = bitsOf(m.field338);
    x.retreatLevel = bitsOf(m.retreatLevel);
    x.resumeLevel = bitsOf(m.resumeLevel);
    x.sightRange = bitsOf(m.sightRange);
    x.aggression = bitsOf(m.aggression);
    x.unknown341 = m.escortName;
    // Spells by name, only those it has.
    for (std::uint32_t i = 0; i < m.spells.size(); ++i) {
        if (m.spells[i] != 0) {
            x.spells.push_back({std::string(things::spellName(i)), m.spells[i]});
        }
    }
    x.spellDelays = m.spellDelays;
    x.unknown329 = bitsOf(m.field329);
    x.unknown510 = static_cast<std::uint32_t>(m.spellPower);
    x.unknown330 = bitsOf(m.field330);
    for (std::size_t i = 0; i < 3; ++i) {
        x.defaultSpells[i] = std::string(things::spellName(m.bombSpells[i]));
    }
    // AIAction340's name when it's IDLE, GUARD, ROAM or ESCORT, else ACTION_INVALID.
    const int start = m.startAction;
    x.action = start == MonsterAction::Idle || start == MonsterAction::Guard || start == MonsterAction::Roam || start == MonsterAction::Escort
                   ? std::string(MonsterAction::name(start))
                   : std::string("ACTION_INVALID");
    x.actionData = actionData(o, rec ? &rec->actionData : nullptr);
    x.unknown361 = m.healthFromMap;
    if (f.info.subclasses & things::ObjectSubclass::Shopkeeper) {
        // Its shop, its InitData as it stands (trade.hpp): what it buys at and sells for
        // its greeting, its records (XFer_WriteShopItem: the count,
        // the type's name, a field guide's creature by its type (none: nothing), an ability book's
        // ability by name, anything else's spell by name (SPELL_INVALID for 0), the modifiers by
        // name).
        const ServerObject::Shopkeeper k = o.shopkeeper ? *o.shopkeeper : ServerObject::Shopkeeper{};
        xf::Shop shop;
        shop.unknown1 = bitsOf(k.buyMultiplier);
        shop.unknown2 = bitsOf(k.sellMultiplier);
        shop.name = k.greeting;
        for (const auto& r : k.stock) {
            xf::Shop::Item item;
            item.count = r.count;
            item.type = r.type;
            const things::ThingDef* t = things_.findThing(r.type);
            const auto codec = t ? t->lastAttribute("XFER") : std::nullopt;
            if (codec && *codec == "FieldGuideXfer") {
                item.parameter = things_.findThing(r.guide) ? r.guide : std::string();
            } else if (codec && *codec == "AbilityRewardXfer") {
                const auto a = static_cast<std::size_t>(r.parameter);
                item.parameter = a < client::kAbilityNames.size() ? std::string(client::kAbilityNames[a]) : std::string();
            } else {
                item.parameter = std::string(things::spellName(static_cast<std::uint32_t>(r.parameter)));
            }
            for (std::size_t i = 0; i < 4; ++i) {
                if (const things::ModifierDef* mod = modifiers_ ? modifiers_->modifier(r.modifiers[i]) : nullptr) {
                    item.modifiers[i] = mod->name;
                }
            }
            shop.items.push_back(std::move(item));
        }
        x.shop = std::move(shop);
    }
    x.unknown0 = m.polygon;
    x.subclassBits = o.subclasses & 0x180u;  // subclass 0x80 (MONITOR) and 0x100 (MIGRATE)
    x.health = o.health;
    x.unknown337 = {m.resumeFromDef, m.retreatFromDef, m.statusFromDef, m.typeSpells};
    if (f.info.subclasses & things::ObjectSubclass::FemaleNpc) { // (its colours; its voice, readNPCVoiceSet)
        x.colors.emplace();
        for (std::size_t i = 0; i < 6; ++i) {
            (*x.colors)[i] = m.colors[i];
        }
        x.voiceSet = npcVoiceName(world_, o);
    }
    x.buffs = buffs(o);
    if (f.info.subclasses & things::ObjectSubclass::WoundedNpc) {
        x.voiceSet2 = npcVoiceName(world_, o);
    }
    x.poison = o.poison;
    return x;
}

// NPCXfer's record, the same as monster() above but in NPCXfer's field order, with its colours,
// experience and voice always written.
xf::Npc Saver::npc(const ServerObject& o, const xf::Npc* rec)
{
    if (!o.monster) {
        if (rec) {
            return *rec;
        }
        throw std::runtime_error("object save: an NPCXfer thing without its AI state");
    }
    // XFerNPC's write: the live NPC.
    const auto& m = *o.monster;
    xf::Npc n;
    const auto [dx, dy] = gridDirection(static_cast<std::int16_t>(o.direction));
    n.direction = {dx, dy};
    n.scripts.enemySighted = handler(m.onEnemySighted);
    n.scripts.death = handler(m.onDeath);
    n.scripts.unknown305 = static_cast<std::uint16_t>(m.lookDelay);
    n.scripts.lookingForEnemy = handler(m.onLookingForEnemy);
    n.scripts.changeFocus = handler(m.onChangeFocus);
    n.scripts.isHit = handler(m.onIsHit);
    n.scripts.retreat = handler(m.onRetreat);
    n.scripts.collision = handler(m.onCollision);
    n.scripts.hearEnemy = handler(m.onHearEnemy);
    n.scripts.endOfWaypoint = handler(m.onEndOfWaypoint);
    n.scripts.lostEnemy = handler(m.onLostEnemy);
    // (The exe writes a stack value here unset: whatever its stack held, an original bug. Ours 0.)
    n.unknown1 = 0;
    for (std::size_t i = 0; i < 6; ++i) {
        n.colors[i] = m.colors[i];
    }
    n.unknown333 = m.field333;
    n.statusFlags = m.status;
    n.unknown338 = bitsOf(m.field338);
    n.retreatLevel = bitsOf(m.retreatLevel);
    n.resumeLevel = bitsOf(m.resumeLevel);
    n.sightRange = bitsOf(m.sightRange);
    n.health = o.health;
    n.aggression = bitsOf(m.aggression);
    n.unknown341 = m.escortName;
    for (std::uint32_t i = 0; i < m.spells.size(); ++i) {
        if (m.spells[i] != 0) {
            n.spells.push_back({std::string(things::spellName(i)), m.spells[i]});
        }
    }
    n.spellDelays = m.spellDelays;
    n.unknown329 = bitsOf(m.field329);
    n.unknown510 = static_cast<std::uint32_t>(m.spellPower);
    n.unknown331 = m.field331;
    n.unknown332 = bitsOf(m.field332);
    n.unknown330 = bitsOf(m.field330);
    for (std::size_t i = 0; i < 3; ++i) {
        n.defaultSpells[i] = std::string(things::spellName(m.bombSpells[i]));
    }
    const int start = m.startAction;
    n.action = start == MonsterAction::Idle || start == MonsterAction::Guard || start == MonsterAction::Roam || start == MonsterAction::Escort
                   ? std::string(MonsterAction::name(start))
                   : std::string("ACTION_INVALID");
    n.actionData = actionData(o, rec ? &rec->actionData : nullptr);
    n.unknown361 = m.healthFromMap;
    n.unknown0 = m.polygon;
    n.healthMax = o.maxHealth;
    n.subclassBits = o.subclasses & 0x180u;  // subclass 0x80 (MONITOR) and 0x100 (MIGRATE)
    n.healthCurrent = o.health;
    n.experience = bitsOf(o.experience);
    n.voiceSet = npcVoiceName(world_, o); // (readNPCVoiceSet's write)
    n.buffs = buffs(o);
    n.poison = o.poison;
    return n;
}

xf::MonsterGenerator Saver::generator(const ServerObject& o, const xf::MonsterGenerator* rec)
{
    if (!o.generator) {
        if (rec) {
            xf::MonsterGenerator x = *rec;
            for (auto& s : x.scripts) {
                s = resolve(s);
            }
            return x;
        }
        // (One made in play: MonsterGeneratorCreate's state, spawn mode 2, near players.)
        return xf::MonsterGenerator{{0, 0, 0}, 0, 0, 0, {}, {{}, {}, {}}, {0, 0, 0}, 2};
    }
    const auto& g = *o.generator;
    xf::MonsterGenerator x;
    x.unknown1.assign(g.rate.begin(), g.rate.end()); // (three)
    x.unknown2 = g.active;
    x.unknown3 = g.most;
    x.unknown4 = g.lastSpawn;
    x.scripts = {handler(g.onHurt), handler(g.onDie), handler(g.onCollide), handler(g.onSpawn)};
    // Three groups, each its templates (its non-empty slots: the type's name, then the template
    // through xfer_saveObj with what it holds).
    x.groups.resize(3);
    for (std::size_t k = 0; k < 3; ++k) {
        for (const auto& tp : g.groups[k]) {
            if (!tp.object) {
                continue;
            }
            std::vector<const ServerObject*> items;
            for (const auto& i : tp.items) {
                if (i) {
                    items.push_back(i.get());
                }
            }
            std::vector<map::ObjectRecord> records;
            if (!saveObj(*tp.object, records, &items) || records.empty()) {
                continue; // (nothing written for it but its name: never so for a template)
            }
            xf::MonsterGenerator::Template t;
            t.type = tp.object->typeName;
            t.record = std::move(records.front());
            t.inventory.assign(std::make_move_iterator(records.begin() + 1), std::make_move_iterator(records.end()));
            x.groups[k].push_back(std::move(t));
        }
    }
    x.unknown5.assign(g.mostBy.begin(), g.mostBy.end()); // (three)
    x.unknown6 = g.flags;
    return x;
}

// One record: the header, then its type's xfer fields at that xfer's current version.
std::vector<std::uint8_t> Saver::recordData(const ServerObject& o, std::size_t held)
{
    const TypeFacts* f = facts(o);
    if (!f) {
        throw std::runtime_error("object save: a thing of no known type");
    }
    map::ObjectXfer x;
    x.xferVersion = saveVersion(f->xfer);

    x.header = header(o, held);
    const xf::Data d = data(o, *f, o.record.get(), x.xferVersion);
    x.extra = xf::encode(d, x.xferVersion, f->info, x.extraOffset());
    return x.serialize();
}

bool Saver::saveObj(const ServerObject& o, std::vector<map::ObjectRecord>& out, const std::vector<const ServerObject*>* items)
{
    // xfer_saveObj: nothing for a destroyed one; else its TOC id (none: 0, not
    // written), its xfer, then what it holds, head first, each the same.
    if (o.gone() || o.pendingDelete) {
        return true;
    }
    const std::uint16_t id = o.type < ids_.size() ? ids_[o.type] : 0;
    if (id == 0) {
        return false;
    }
    std::vector<const ServerObject*> held;
    if (items) {
        held = *items;
    } else {
        for (const std::uint32_t e : o.inventory) {
            if (const ServerObject* i = find(e)) {
                held.push_back(i);
            }
        }
    }
    map::ObjectRecord rec;
    rec.tocId = id;
    rec.data = recordData(o, held.size());
    rec.size = static_cast<std::uint32_t>(rec.data.size());
    out.push_back(std::move(rec));
    for (const ServerObject* i : held) {
        saveObj(*i, out);
    }
    return true;
}

map::ObjectRecord Saver::clientRecord(const World::ClientOnly& o, const TypeFacts& f)
{
    // A fresh object of the drawable's type (newObjectWithTypeInd),
    // at its truncated place + 0.5, its extent, script id (and net code) the drawable's
    // NetCode32 (the map's number), its flags and xStatus the drawable's: the type's, and the
    // map record's over them (spriteLoadFromMap); then saved. The fresh one takes a
    // script id from the counter.
    world_.newScriptId();
    const map::ObjectXfer& rec = *o.record;
    ServerObject fresh = describeType(world_, things_, o.typeName);
    fresh.fileExtent = MapNumber{rec.header.extent};
    fresh.extent = fresh.fileExtent.asExtent(); // (a drawable's: its map number its key and net code)
    fresh.scriptId = static_cast<std::int32_t>(rec.header.extent);
    fresh.x = static_cast<float>(static_cast<int>(rec.header.x)) + 0.5f;
    fresh.y = static_cast<float>(static_cast<int>(rec.header.y)) + 0.5f;
    fresh.flags = f.clientFlags;
    fresh.xStatus = 0;
    if (rec.header.details) {
        fresh.flags = (f.clientFlags & 0xEEBF7E9Du) | rec.header.details->flags;
        fresh.xStatus = rec.header.details->statusFlags;
    }
    fresh.timer = 0;   // (a fresh object's)
    fresh.decayAt = 0;
    fresh.record.reset(); // (no pickup script: a fresh object's index is -1)
    map::ObjectRecord r;
    r.tocId = ids_[o.type];
    map::ObjectXfer x;
    x.xferVersion = 0x3C;
    x.header = header(fresh, 0);
    x.header.extent = rec.header.extent; // (a map number past 0x4000 kept as it is)
    x.xferVersion = saveVersion(f.xfer);
    // Its xfer from the fresh object's own fields, but an InvisibleLight's light: the drawable's (the map's).
    const xf::Data d = data(fresh, f, f.xfer == "InvisibleLightXfer" ? &rec : nullptr, x.xferVersion);

    x.extra = xf::encode(d, x.xferVersion, f.info, x.extraOffset());
    r.data = x.serialize();
    r.size = static_cast<std::uint32_t>(r.data.size());
    return r;
}

SavedObjects Saver::run()
{
    const std::vector<ServerObject*> objects = list(false);
    const std::vector<ServerObject*> missiles = list(true);
    // The host client's client-only drawables: the drawable list's order (each new one at its
    // head: the map file's reversed).
    std::vector<const World::ClientOnly*> drawables;
    if (!(world_.gameFlags & 0x200000u)) {  // GameFlag::SoloOther: no client-only drawables saved
        for (auto it = world_.clientOnly.rbegin(); it != world_.clientOnly.rend(); ++it) {
            const TypeFacts* f = facts(it->type);
            if (it->record && f && f->icon) {
                drawables.push_back(&*it);
            }
        }
    }

    // ObjectTOC: the ids in the order met.
    // Every type met (objects, what they carry, a generator's queued creatures and their items) gets an id.
    for (const ServerObject* o : objects) {
        assign(o->type);
        for (const std::uint32_t e : o->inventory) {
            if (const ServerObject* i = find(e)) {
                assign(i->type);
            }
        }
        if ((o->classes & oc::MonsterGenerator) && o->generator) {
            for (const auto& group : o->generator->groups) {
                for (const auto& tp : group) {
                    if (tp.object) {
                        assign(tp.object->type);
                        for (const auto& i : tp.items) {
                            if (i) {
                                assign(i->type);
                            }
                        }
                    }
                }
            }
        }
    }
    for (const ServerObject* o : missiles) {
        assign(o->type);
    }
    for (const World::ClientOnly* o : drawables) {
        assign(o->type);
    }
    SavedObjects out;
    out.toc.version = 1;
    // The table lists the ids by type index (ids_ is 1-based: index 0 unused).
    for (std::size_t t = 1; t < ids_.size(); ++t) {
        if (ids_[t] != 0) {
            out.toc.entries.push_back({ids_[t], things_.things[t - 1].name});
        }
    }

    // ObjectData.
    out.data.version = 1;
    auto& records = out.data.records;
    // Only types with a menu icon are written (TypeFacts::icon), as the exe saves them.
    for (const ServerObject* o : objects) {
        const TypeFacts* f = facts(*o);
        if (f && f->icon) {
            saveObj(*o, records);
        }
    }
    for (ServerObject* o : missiles) {
        if ((o->classes & oc::Weapon) && (o->subclasses & 0x40u)) {
            // A thrown chakram: the one it carries, put at its place.
            ServerObject* carried = o->inventory.empty() ? nullptr : world_.byExtent(o->inventory.front());
            if (carried) {
                carried->x = o->x;
                carried->y = o->y;
                saveObj(*carried, records);
            }
            continue;
        }
        const TypeFacts* f = facts(*o);
        if (f && f->icon) {
            saveObj(*o, records);
        }
    }
    for (const World::ClientOnly* o : drawables) {
        records.push_back(clientRecord(*o, *facts(o->type)));
    }
    return out;
}

} // namespace

SavedObjects saveObjects(World& world, const things::ThingDb& things, const things::ModifierDb* modifiers, std::uint32_t frame)
{
    return Saver(world, things, modifiers, frame).run();
}

std::vector<std::uint8_t> objectRecord(World& world, const things::ThingDb& things, const things::ModifierDb* modifiers, std::uint32_t frame,
                                       const ServerObject& o)
{
    return Saver(world, things, modifiers, frame).record(o);
}

void readLoadedEnchantments(Fight& fight)
{
    World& world = fight.world;
    if (!fight.things) {
        return;
    }
    Spells spells{world, *fight.things, fight.modifiers, fight.rng, fight.frame};
    for (const std::uint32_t e : std::vector<std::uint32_t>(world.loadOrder)) {
        ServerObject* o = world.byExtent(e);
        if (!o || !o->monster || !o->record || o->record->xferVersion < 0x3C) {
            continue;
        }
        const things::ThingDef* t = fight.things->findThing(o->typeName);
        const auto codec = t ? t->lastAttribute("XFER").value_or("") : std::string_view();
        // MonsterXfer's from version 0x3E (poison 0x40), NPCXfer's from 0x3D (poison 0x3E).
        const map::ObjectXfer& x = *o->record;
        xf::Buffs buffs;
        std::uint8_t poison = 0;
        try {
            if (codec == "MonsterXfer" && 0x3D < x.xferVersion) {
                auto m = std::get<xf::Monster>(xf::decode("MonsterXfer", x.xferVersion, x.extra, {things::thingClasses(*t), things::thingSubclasses(*t)}, x.extraOffset()));
                buffs = std::move(m.buffs);
                poison = 0x3F < x.xferVersion ? m.poison : 0;
            } else if (codec == "NPCXfer" && 0x3C < x.xferVersion) {
                auto n = std::get<xf::Npc>(xf::decode("NPCXfer", x.xferVersion, x.extra, {things::thingClasses(*t), things::thingSubclasses(*t)}, x.extraOffset()));
                buffs = std::move(n.buffs);
                poison = 0x3D < x.xferVersion ? n.poison : 0;
            } else {
                continue;
            }
        } catch (const std::exception&) {
            continue;
        }
        // Each enchantment put back by casting its spell on it at the saved power, then its time left.
        for (const auto& b : buffs.buffs) {
            const auto name = std::ranges::find(kEnchantNames, b.enchantment);
            if (name == std::end(kEnchantNames)) {
                break; // (enchantByName: -1, the xfer fails)
            }
            const int ench = static_cast<int>(name - std::begin(kEnchantNames));
            if (const std::uint32_t spell = enchantmentSpell(ench); spell != 0) {
                spells.cast(spell, *o, o, o->x, o->y, b.power);
            }
            o->buffTime[static_cast<std::size_t>(ench)] = static_cast<std::uint16_t>(b.timer); // (BuffsDur)
            if (ench == Enchant::Shield && 1 < buffs.version) {
                for (DurationSpell& s : world.durationSpells()) {
                    if (!s.cancelled && s.spell == 0x33 && s.target != 0 && s.target == o->extent) {
                        s.value = static_cast<int>(b.shieldHealth);
                        break;
                    }
                }
            }
        }
        if (poison != 0) {
            fight.setPoison(*o, poison);
        }
    }
}
} // namespace nox::server
