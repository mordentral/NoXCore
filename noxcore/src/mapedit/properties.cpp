// The map editor's property tables (properties.hpp).
//
// Each field's meaning is the game's: the xfer reader that stores it and the code that uses it,
// named in the help text by what it does (our server port follows the same readers: world.cpp's
// readObject, objects.cpp, monsters.cpp, rewards.cpp, the client's light drawing).
#include "noxcore/mapedit/properties.hpp"

#include "doc_types.hpp"

#include "noxcore/map/objects.hpp"
#include "noxcore/mapedit/object_edit.hpp"
#include "noxcore/mapedit/op_label.hpp"
#include "noxcore/server/enchant.hpp"
#include "noxcore/server/monsters.hpp"
#include "noxcore/things/field_guide.hpp"
#include "noxcore/things/modifier.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/things/sound_sets.hpp"
#include "noxcore/things/spell_ids.hpp"
#include "noxcore/things/thing_db.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <cmath>
#include <map>
#include <numbers>
#include <span>

namespace nox::mapedit {

namespace {

namespace xf = map::xfer;
using Kind = FieldKind;
using nox::l10n::Key;

constexpr double kTwoPi = 2 * std::numbers::pi;

/// No text (a field or value without help).
constexpr Key kNoText{"", ""};
/// A game name (a flag's or class's as the game's files name it), shown as it is.
constexpr Key game(const char* name)
{
    return {"", name};
}

// --- The panel's sections --------------------------------------------------------------------------------

namespace grp {
constexpr Key kHeader = NOX_TRK("editor.prop.group.header", "Header");
constexpr Key kDoor = NOX_TRK("editor.prop.group.door", "Door");
constexpr Key kTrigger = NOX_TRK("editor.prop.group.trigger", "Trigger");
constexpr Key kPlate = NOX_TRK("editor.prop.group.pressure_plate", "Pressure plate");
constexpr Key kLight = NOX_TRK("editor.prop.group.light", "Light");
constexpr Key kLightCone = NOX_TRK("editor.prop.group.light_cone", "Light cone");
constexpr Key kMonster = NOX_TRK("editor.prop.group.monster", "Monster");
constexpr Key kNpc = NOX_TRK("editor.prop.group.npc", "NPC");
constexpr Key kBehaviour = NOX_TRK("editor.prop.group.behaviour", "Behaviour");
constexpr Key kHandlers = NOX_TRK("editor.prop.group.script_handlers", "Script handlers");
constexpr Key kDelays = NOX_TRK("editor.prop.group.spell_delays", "Spell delays");
constexpr Key kSpells = NOX_TRK("editor.prop.group.spells", "Spells");
constexpr Key kCombat = NOX_TRK("editor.prop.group.combat", "Combat");
constexpr Key kAdvanced = NOX_TRK("editor.prop.group.advanced", "Advanced");
constexpr Key kDefaults = NOX_TRK("editor.prop.group.defaults", "Defaults");
constexpr Key kLook = NOX_TRK("editor.prop.group.look", "Look");
constexpr Key kShop = NOX_TRK("editor.prop.group.shop", "Shop");
constexpr Key kGenerator = NOX_TRK("editor.prop.group.generator", "Generator");
constexpr Key kSlots = NOX_TRK("editor.prop.group.slots", "Slots");
constexpr Key kReward = NOX_TRK("editor.prop.group.quest_reward", "Quest reward");
constexpr Key kReadable = NOX_TRK("editor.prop.group.readable", "Readable");
constexpr Key kExit = NOX_TRK("editor.prop.group.exit", "Exit");
constexpr Key kPit = NOX_TRK("editor.prop.group.pit", "Pit");
constexpr Key kTeleporter = NOX_TRK("editor.prop.group.teleporter", "Teleporter");
constexpr Key kElevator = NOX_TRK("editor.prop.group.elevator", "Elevator");
constexpr Key kShaft = NOX_TRK("editor.prop.group.elevator_shaft", "Elevator shaft");
constexpr Key kMover = NOX_TRK("editor.prop.group.mover", "Mover");
constexpr Key kGlyph = NOX_TRK("editor.prop.group.glyph", "Glyph");
constexpr Key kSentry = NOX_TRK("editor.prop.group.sentry", "Sentry");
constexpr Key kSpellBook = NOX_TRK("editor.prop.group.spell_book", "Spell book");
constexpr Key kAbilityBook = NOX_TRK("editor.prop.group.ability_book", "Ability book");
constexpr Key kFieldGuide = NOX_TRK("editor.prop.group.field_guide", "Field guide");
constexpr Key kPedestal = NOX_TRK("editor.prop.group.spell_pedestal", "Spell pedestal");
constexpr Key kItem = NOX_TRK("editor.prop.group.item", "Item");
constexpr Key kGold = NOX_TRK("editor.prop.group.gold", "Gold");
constexpr Key kObelisk = NOX_TRK("editor.prop.group.obelisk", "Obelisk");
constexpr Key kCloud = NOX_TRK("editor.prop.group.toxic_cloud", "Toxic cloud");
} // namespace grp

// --- Builders ----------------------------------------------------------------------------------------

/// A field of xfer struct T, read and written through lambdas on the struct.
template <class T, class G, class S>
FieldDesc on(FieldDesc d, G get, S set)
{
    d.get = [get](const DocObject& o) -> FieldValue {
        if (const T* t = std::get_if<T>(&o.data)) return get(*t);
        return {};
    };
    d.set = [set](DocObject& o, const FieldValue& v) {
        if (T* t = std::get_if<T>(&o.data)) set(*t, v);
    };
    return d;
}

FieldDesc desc(std::string id, Key group, Key label, Kind kind, Key help, double lo = 0, double hi = 0)
{
    FieldDesc d;
    d.id = std::move(id);
    d.group = group;
    d.label = label;
    d.kind = kind;
    d.help = help;
    d.min = lo;
    d.max = hi;
    return d;
}

FieldValue iv(std::int64_t i)
{
    FieldValue v;
    v.i = i;
    return v;
}
FieldValue fv(double f)
{
    FieldValue v;
    v.f = f;
    return v;
}
FieldValue sv(std::string s)
{
    FieldValue v;
    v.s = std::move(s);
    return v;
}
FieldValue rgbv(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    FieldValue v;
    v.rgb = {r, g, b};
    return v;
}

template <class N>
N clampTo(std::int64_t v, double lo, double hi)
{
    return static_cast<N>(std::clamp<std::int64_t>(v, static_cast<std::int64_t>(lo), static_cast<std::int64_t>(hi)));
}

float bitsFloat(std::uint32_t b)
{
    return std::bit_cast<float>(b);
}
std::uint32_t floatBits(double f)
{
    return std::bit_cast<std::uint32_t>(static_cast<float>(f));
}

/// A whole number member.
template <class T, class M>
FieldDesc num(std::string id, Key group, Key label, Key help, M T::*m, double lo, double hi)
{
    return on<T>(desc(std::move(id), group, label, Kind::Int, help, lo, hi),
                 [m](const T& t) { return iv(static_cast<std::int64_t>(t.*m)); },
                 [m, lo, hi](T& t, const FieldValue& v) { t.*m = clampTo<M>(v.i, lo, hi); });
}

/// A 16-bit word the game reads signed (sign-extended where it's used), shown and set as
/// -32768..32767 and stored as the same word: a map's bytes round-trip unchanged.
template <class T>
FieldDesc snum16(std::string id, Key group, Key label, Key help, std::uint16_t T::*m)
{
    return on<T>(desc(std::move(id), group, label, Kind::Int, help, -32768, 32767),
                 [m](const T& t) { return iv(static_cast<std::int16_t>(t.*m)); },
                 [m](T& t, const FieldValue& v) { t.*m = static_cast<std::uint16_t>(clampTo<std::int16_t>(v.i, -32768, 32767)); });
}

/// A float member.
template <class T>
FieldDesc flt(std::string id, Key group, Key label, Key help, float T::*m, double lo, double hi)
{
    return on<T>(desc(std::move(id), group, label, Kind::Float, help, lo, hi),
                 [m](const T& t) { return fv(t.*m); },
                 [m, lo, hi](T& t, const FieldValue& v) { t.*m = static_cast<float>(std::clamp(v.f, lo, hi)); });
}

/// A float kept as its bits in a u32 member.
template <class T>
FieldDesc fbits(std::string id, Key group, Key label, Key help, std::uint32_t T::*m, double lo, double hi)
{
    return on<T>(desc(std::move(id), group, label, Kind::Float, help, lo, hi),
                 [m](const T& t) { return fv(bitsFloat(t.*m)); },
                 [m, lo, hi](T& t, const FieldValue& v) { t.*m = floatBits(std::clamp(v.f, lo, hi)); });
}

/// A named bit with its texts (the tables below; FlagBit is their English).
struct Bit {
    std::uint32_t bit = 0;
    Key name;
    Key help;
};

std::vector<NamedValue> named(std::span<const Bit> bits)
{
    std::vector<NamedValue> out;
    out.reserve(bits.size());
    for (const auto& b : bits) out.push_back({b.bit, b.name, b.help});
    return out;
}

std::vector<FlagBit> english(std::span<const Bit> bits)
{
    std::vector<FlagBit> out;
    out.reserve(bits.size());
    for (const auto& b : bits) out.push_back({b.bit, b.name.en, b.help.en});
    return out;
}

/// A flags member (unknown bits kept: the panel sets bits under a mask).
template <class T, class M>
FieldDesc flags(std::string id, Key group, Key label, Key help, M T::*m, std::vector<NamedValue> names)
{
    FieldDesc d = on<T>(desc(std::move(id), group, label, Kind::Flags, help, 0,
                             static_cast<double>(std::numeric_limits<M>::max())),
                        [m](const T& t) { return iv(static_cast<std::int64_t>(t.*m)); },
                        [m](T& t, const FieldValue& v) { t.*m = static_cast<M>(v.i); });
    d.names = std::move(names);
    return d;
}

/// An enumerated member.
template <class T, class M>
FieldDesc enumf(std::string id, Key group, Key label, Key help, M T::*m, std::vector<NamedValue> names)
{
    FieldDesc d = on<T>(desc(std::move(id), group, label, Kind::Enum, help, 0,
                             static_cast<double>(std::numeric_limits<M>::max())),
                        [m](const T& t) { return iv(static_cast<std::int64_t>(t.*m)); },
                        [m](T& t, const FieldValue& v) { t.*m = static_cast<M>(v.i); });
    d.names = std::move(names);
    return d;
}

/// A string member of some kind (Text, Spell, MapName...), at most `maxBytes`.
template <class T>
FieldDesc str(std::string id, Key group, Key label, Kind kind, Key help, std::string T::*m, std::size_t maxBytes)
{
    return on<T>(desc(std::move(id), group, label, kind, help, 0, static_cast<double>(maxBytes)),
                 [m](const T& t) { return sv(t.*m); },
                 [m, maxBytes](T& t, const FieldValue& v) { t.*m = v.s.substr(0, maxBytes); });
}

/// A script handler member (the function's name; its stored flags word kept).
template <class T, class H>
FieldDesc handler(std::string id, Key group, Key label, Key help, H get)
{
    return on<T>(desc(std::move(id), group, label, Kind::Function, help, 0, 1023),
                 [get](const T& t) { return sv(get(const_cast<T&>(t)).function); },
                 [get](T& t, const FieldValue& v) { get(t).function = v.s.substr(0, 1023); });
}

template <class T>
FieldDesc color(std::string id, Key group, Key label, Key help, std::array<std::uint8_t, 3> T::*m)
{
    return on<T>(desc(std::move(id), group, label, Kind::Color, help),
                 [m](const T& t) { return rgbv((t.*m)[0], (t.*m)[1], (t.*m)[2]); }, [m](T& t, const FieldValue& v) { t.*m = v.rgb; });
}

FieldDesc& adv(FieldDesc& d)
{
    d.advanced = true;
    return d;
}

std::vector<NamedValue> range(std::initializer_list<Key> names, std::int64_t first = 0)
{
    std::vector<NamedValue> out;
    std::int64_t v = first;
    for (const Key& n : names) out.push_back({v++, n, kNoText});
    return out;
}

// --- Tables of names -------------------------------------------------------------------------------

/// The object CLASS bits by name (thing.bin's names).
const std::vector<NamedValue>& classBits()
{
    namespace oc = things::ObjectClass;
    static const Bit bits[] = {
        {oc::Missile, game("MISSILE"), kNoText},
        {oc::Monster, game("MONSTER"), NOX_TRK("editor.prop.class.monster.help", "Creatures and NPCs.")},
        {oc::Player, game("PLAYER"), kNoText},
        {oc::Obstacle, game("OBSTACLE"), NOX_TRK("editor.prop.class.obstacle.help", "Pushable obstacles.")},
        {oc::Food, game("FOOD"), kNoText},             {oc::Exit, game("EXIT"), kNoText},
        {oc::Key, game("KEY"), kNoText},               {oc::Door, game("DOOR"), kNoText},
        {oc::InfoBook, game("INFO_BOOK"), kNoText},    {oc::Trigger, game("TRIGGER"), kNoText},
        {oc::Transporter, game("TRANSPORTER"), kNoText}, {oc::Hole, game("HOLE"), kNoText},
        {oc::Wand, game("WAND"), kNoText},             {oc::Fire, game("FIRE"), kNoText},
        {oc::Elevator, game("ELEVATOR"), kNoText},     {oc::ElevatorShaft, game("ELEVATOR_SHAFT"), kNoText},
        {oc::Dangerous, game("DANGEROUS"), kNoText},   {oc::MonsterGenerator, game("MONSTERGENERATOR"), kNoText},
        {oc::Readable, game("READABLE"), kNoText},     {oc::Light, game("LIGHT"), kNoText},
        {oc::Simple, game("SIMPLE"), kNoText},         {oc::Complex, game("COMPLEX"), kNoText},
        {oc::Immobile, game("IMMOBILE"), kNoText},     {oc::VisibleEnable, game("VISIBLE_ENABLE"), kNoText},
        {oc::Weapon, game("WEAPON"), kNoText},         {oc::Armor, game("ARMOR"), kNoText},
        {oc::NotStackable, game("NOT_STACKABLE"), kNoText}, {oc::Treasure, game("TREASURE"), kNoText},
        {oc::Flag, game("FLAG"), kNoText},             {oc::ClientPersist, game("CLIENT_PERSIST"), kNoText},
        {oc::ClientPredict, game("CLIENT_PREDICT"), kNoText},
        {oc::Pickup, game("PICKUP"), NOX_TRK("editor.prop.class.pickup.help", "Anything that can be picked up.")},
    };
    static const std::vector<NamedValue> values = named(bits);
    return values;
}

std::vector<NamedValue> teamValues()
{
    std::vector<NamedValue> out;
    for (const auto& t : teamNames()) out.push_back({t.team, t.name, kNoText});
    return out;
}

// (The flag tables, below with their FlagBit English.)
const std::vector<NamedValue>& objectFlagValues();
const std::vector<NamedValue>& xStatusValues();
const std::vector<NamedValue>& monsterStatusValues();

/// The direction table's 8 grid steps from east, clockwise on screen (placement.cpp's order).
constexpr std::array<std::array<std::int32_t, 2>, 8> kSteps = {{{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}}};

std::int64_t stepOf(const std::array<std::int32_t, 2>& d)
{
    for (std::size_t i = 0; i < kSteps.size(); ++i) {
        if (kSteps[i] == d) return static_cast<std::int64_t>(i);
    }
    return 0;
}

// --- The header ------------------------------------------------------------------------------------------

void headerFields(const DocObject& o, std::vector<FieldDesc>& out)
{
    {
        FieldDesc d = desc("header.type", grp::kHeader, NOX_TRK("editor.prop.header.type.label", "Type"), Kind::Thing,
                           NOX_TRK("editor.prop.header.type.help",
                                   "The thing.bin type. Changing it gives the object the new type's fresh data (what the game gives a new one: "
                                   "monster.bin's numbers, a plate's colours...), keeping its place, extent and header."));
        d.special = FieldDesc::Special::Type;
        d.get = [](const DocObject& x) { return sv(x.type); };
        d.set = [](DocObject&, const FieldValue&) {}; // (changeTypeOp)
        out.push_back(std::move(d));
    }
    {
        FieldDesc d = desc("header.position", grp::kHeader, NOX_TRK("editor.prop.header.position.label", "Position"), Kind::Point,
                           NOX_TRK("editor.prop.header.position.help", "Where it stands, in world units (23 a wall cell)."), 0, kWorldSize - 1);
        d.get = [](const DocObject& x) {
            FieldValue v;
            v.f = x.header.x;
            v.g = x.header.y;
            return v;
        };
        d.set = [](DocObject& x, const FieldValue& v) {
            x.header.x = static_cast<float>(std::clamp(v.f, 0.0, static_cast<double>(kWorldSize - 1)));
            x.header.y = static_cast<float>(std::clamp(v.g, 0.0, static_cast<double>(kWorldSize - 1)));
        };
        out.push_back(std::move(d));
    }
    {
        FieldDesc d = desc("header.extent", grp::kHeader, NOX_TRK("editor.prop.header.extent.label", "Extent"), Kind::Int,
                           NOX_TRK("editor.prop.header.extent.help",
                                   "The map's number for it: links (teleporters, elevators, movers) and object groups name it. Changing it "
                                   "rewrites every link and group member that named it. Must be unique."),
                           1, 0x7FFFFFFF);
        d.special = FieldDesc::Special::Extent;
        d.get = [](const DocObject& x) { return iv(x.header.extent); };
        d.set = [](DocObject&, const FieldValue&) {}; // (setExtentOp)
        out.push_back(std::move(d));
    }
    {
        FieldDesc d = desc("header.details", grp::kHeader, NOX_TRK("editor.prop.header.details.label", "Header details"), Kind::Bool,
                           NOX_TRK("editor.prop.header.details.help",
                                   "Whether the record stores the header's details (the old editor's \"Extra Bytes\"): script name, team, flags, "
                                   "handler, inventory. Without them the object takes its type's flags."));
        d.special = FieldDesc::Special::Details;
        d.get = [](const DocObject& x) { return iv(x.header.details ? 1 : 0); };
        d.set = [](DocObject&, const FieldValue&) {}; // (setDetailsOp)
        out.push_back(std::move(d));
    }
    {
        FieldDesc d = desc("header.scriptId", grp::kHeader, NOX_TRK("editor.prop.header.script_id.label", "Script id"), Kind::Int,
                           NOX_TRK("editor.prop.header.script_id.help",
                                   "The script's handle for it. 0: the game gives one when the map loads (every map the editor writes has 0 for "
                                   "new objects). The game rewrites it."),
                           0, 0xFFFFFFFF);
        d.readOnly = true;
        d.advanced = true;
        d.get = [](const DocObject& x) { return iv(x.header.scriptId); };
        d.set = [](DocObject&, const FieldValue&) {};
        out.push_back(std::move(d));
    }
    if (!o.header.details) return;
    const auto details = [](FieldDesc d, auto get, auto set) {
        d.get = [get](const DocObject& x) { return x.header.details ? get(*x.header.details) : FieldValue{}; };
        d.set = [set](DocObject& x, const FieldValue& v) {
            if (x.header.details) set(*x.header.details, v);
        };
        return d;
    };
    out.push_back(details(desc("header.scriptName", grp::kHeader, NOX_TRK("editor.prop.header.script_name.label", "Script name"), Kind::Text,
                               NOX_TRK("editor.prop.header.script_name.help",
                                       "The name scripts find it by (Object(\"name\")); the game prefixes it with the map's name. At most 255 bytes."),
                               0, 255),
                          [](const map::ObjectHeader::Details& d) { return sv(d.scriptName); },
                          [](map::ObjectHeader::Details& d, const FieldValue& v) { d.scriptName = v.s.substr(0, 255); }));
    {
        FieldDesc d = details(desc("header.team", grp::kHeader, NOX_TRK("editor.prop.header.team.label", "Team"), Kind::Enum,
                                   NOX_TRK("editor.prop.header.team.help",
                                           "Its team: a team's players can't hurt or set off what's on their team; team-only items. 0 none."),
                                   0, 255),
                              [](const map::ObjectHeader::Details& x) { return iv(x.team); },
                              [](map::ObjectHeader::Details& x, const FieldValue& v) { x.team = clampTo<std::uint8_t>(v.i, 0, 255); });
        d.names = teamValues();
        out.push_back(std::move(d));
    }
    {
        FieldDesc d = details(desc("header.flags", grp::kHeader, NOX_TRK("editor.prop.header.flags.label", "Create flags"), Kind::Flags,
                                   NOX_TRK("editor.prop.header.flags.help",
                                           "The object's flags. The game's save writes only the authoring bits (mask 0x11408162); on load the stored "
                                           "value is ORed in whole, so a runtime-only bit stored in a map is set too. Unknown bits are kept."),
                                   0, 0xFFFFFFFF),
                              [](const map::ObjectHeader::Details& x) { return iv(x.flags); },
                              [](map::ObjectHeader::Details& x, const FieldValue& v) { x.flags = static_cast<std::uint32_t>(v.i); });
        d.names = objectFlagValues();
        out.push_back(std::move(d));
    }
    {
        FieldDesc d = details(desc("header.xStatus", grp::kHeader, NOX_TRK("editor.prop.header.xstatus.label", "Status (xStatus)"), Kind::Flags,
                                   NOX_TRK("editor.prop.header.xstatus.help",
                                           "The object's status bits (the old editor's \"AnimFlags\"). The save writes bits 0x5E; the load ORs the "
                                           "stored value in."),
                                   0, 0xFFFFFFFF),
                              [](const map::ObjectHeader::Details& x) { return iv(x.statusFlags); },
                              [](map::ObjectHeader::Details& x, const FieldValue& v) { x.statusFlags = static_cast<std::uint32_t>(v.i); });
        d.names = xStatusValues();
        out.push_back(std::move(d));
    }
    if (o.header.details->onPickup) {
        out.push_back(details(desc("header.onPickup", grp::kHeader, NOX_TRK("editor.prop.header.on_pickup.label", "Pickup handler"), Kind::Function,
                                   NOX_TRK("editor.prop.header.on_pickup.help", "A script function called when a player picks it up (header version 0x3F on)."),
                                   0, 1023),
                              [](const map::ObjectHeader::Details& x) { return sv(x.onPickup ? x.onPickup->function : std::string()); },
                              [](map::ObjectHeader::Details& x, const FieldValue& v) {
                                  if (x.onPickup) x.onPickup->function = v.s.substr(0, 1023);
                              }));
    }
    if (o.header.details->decayFrames) {
        out.push_back(details(desc("header.decay", grp::kHeader, NOX_TRK("editor.prop.header.decay.label", "Decay frames"), Kind::Int,
                                   NOX_TRK("editor.prop.header.decay.help",
                                           "Frames left before a TRANSIENT object decays (header version 0x40); 0 or less: none. (The old editor's "
                                           "0xFFFFFFFB default is just -5.)"),
                                   -0x7FFFFFFF, 0x7FFFFFFF),
                              [](const map::ObjectHeader::Details& x) { return iv(x.decayFrames ? *x.decayFrames : 0); },
                              [](map::ObjectHeader::Details& x, const FieldValue& v) {
                                  if (x.decayFrames) x.decayFrames = static_cast<std::int32_t>(std::clamp<std::int64_t>(v.i, -0x7FFFFFFF, 0x7FFFFFFF));
                              }));
    }
}

// --- Per xfer -----------------------------------------------------------------------------------------------

void doorFields(std::vector<FieldDesc>& out, std::uint16_t v)
{
    const std::vector<NamedValue> ways = {{0, game("0"), kNoText},
                                          {8, NOX_TRK("editor.prop.door.direction.quarter", "8 (a quarter turn)"), kNoText},
                                          {16, NOX_TRK("editor.prop.door.direction.half", "16 (a half turn)"), kNoText},
                                          {24, NOX_TRK("editor.prop.door.direction.three_quarters", "24 (three quarters)"), kNoText}};
    out.push_back(enumf<xf::Door>("door.lock", grp::kDoor, NOX_TRK("editor.prop.door.lock.label", "Lock"),
                                  NOX_TRK("editor.prop.door.lock.help", "The key it needs (Silver, Gold, Ruby, Sapphire); a mechanism lock opens "
                                                                        "only by script (UnlockDoor)."),
                                  &xf::Door::lock,
                                  range({NOX_TRK("editor.prop.door.lock.none", "None"), NOX_TRK("editor.prop.door.lock.silver", "Silver key"),
                                         NOX_TRK("editor.prop.door.lock.gold", "Gold key"), NOX_TRK("editor.prop.door.lock.ruby", "Ruby key"),
                                         NOX_TRK("editor.prop.door.lock.sapphire", "Sapphire key"),
                                         NOX_TRK("editor.prop.door.lock.mechanism", "Mechanism (script only)")})));
    if (v >= 0x29) {
        // Both directions together, as every stock door has them (the shut way the wall joins by).
        FieldDesc d = on<xf::Door>(desc("door.direction", grp::kDoor, NOX_TRK("editor.prop.door.direction.label", "Hangs"), Kind::Enum,
                                        NOX_TRK("editor.prop.door.direction.help",
                                                "Which way the door hangs when shut (0..31, 8 a quarter turn: every stock door uses 0, 8, 16 or 24). Sets "
                                                "the shut direction and the current swing together."),
                                        0, 31),
                                   [](const xf::Door& t) { return iv(t.direction); },
                                   [](xf::Door& t, const FieldValue& x) { t.direction = t.currentDirection = static_cast<std::uint32_t>(x.i & 31); });
        d.names = ways;
        out.push_back(std::move(d));
    }
    FieldDesc cur = num<xf::Door>("door.current", grp::kDoor, NOX_TRK("editor.prop.door.current.label", "Current swing"),
                                  NOX_TRK("editor.prop.door.current.help", "The door's swing now (0..31); equal to the shut direction for a shut door."),
                                  &xf::Door::currentDirection, 0, 31);
    out.push_back(adv(cur));
}

void triggerFields(std::vector<FieldDesc>& out, std::uint16_t v, bool plate)
{
    const Key g = plate ? grp::kPlate : grp::kTrigger;
    out.push_back(num<xf::Trigger>("trigger.sizeX", g, NOX_TRK("editor.prop.trigger.size_x.label", "Width"),
                                   NOX_TRK("editor.prop.trigger.size_x.help", "The box's side along one diagonal (world units; the game takes at most 60)."),
                                   &xf::Trigger::sizeX, 1, 60));
    out.push_back(num<xf::Trigger>("trigger.sizeY", g, NOX_TRK("editor.prop.trigger.size_y.label", "Height"),
                                   NOX_TRK("editor.prop.trigger.size_y.help", "The box's other side (at most 60)."), &xf::Trigger::sizeY, 1, 60));
    out.push_back(color<xf::Trigger>("trigger.edge", g, NOX_TRK("editor.prop.trigger.edge.label", "Outline colour"),
                                     NOX_TRK("editor.prop.trigger.edge.help",
                                             "The plate's outline, drawn first (PressurePlateDraw draws the diamond twice, never filled). All six colour "
                                             "bytes 0: the plate isn't drawn."),
                                     &xf::Trigger::edgeColor));
    out.push_back(color<xf::Trigger>("trigger.back", g, NOX_TRK("editor.prop.trigger.back.label", "Shadow colour"),
                                     NOX_TRK("editor.prop.trigger.back.help", "The plate's second outline, one pixel to the right (its shadow)."),
                                     &xf::Trigger::backColor));
    out.push_back(handler<xf::Trigger>("trigger.onPressed", g, NOX_TRK("editor.prop.trigger.on_pressed.label", "Pressed"),
                                       NOX_TRK("editor.prop.trigger.on_pressed.help", "Called when it's pressed."),
                                       [](xf::Trigger& t) -> map::ScriptHandler& { return t.onPressed; }));
    out.push_back(handler<xf::Trigger>("trigger.onReleased", g, NOX_TRK("editor.prop.trigger.on_released.label", "Released"),
                                       NOX_TRK("editor.prop.trigger.on_released.help", "Called when it's let go (once what pressed it has left)."),
                                       [](xf::Trigger& t) -> map::ScriptHandler& { return t.onReleased; }));
    out.push_back(handler<xf::Trigger>("trigger.onCollided", g, NOX_TRK("editor.prop.trigger.on_collided.label", "Touched (filter)"),
                                       NOX_TRK("editor.prop.trigger.on_collided.help",
                                               "Called on each touch; when it returns 0 the touch doesn't count as a press (a filter)."),
                                       [](xf::Trigger& t) -> map::ScriptHandler& { return t.onCollided; }));
    out.push_back(flags<xf::Trigger>("trigger.allowed", g, NOX_TRK("editor.prop.trigger.allowed.label", "Pressed by (classes)"),
                                     NOX_TRK("editor.prop.trigger.allowed.help",
                                             "What may press it: none ticked = anything with mass; else only objects with one of these classes."),
                                     &xf::Trigger::allowedClasses, classBits()));
    out.push_back(flags<xf::Trigger>("trigger.ignored", g, NOX_TRK("editor.prop.trigger.ignored.label", "Ignores (classes)"),
                                     NOX_TRK("editor.prop.trigger.ignored.help", "What never presses it: objects with any of these classes."),
                                     &xf::Trigger::ignoredClasses, classBits()));
    {
        FieldDesc d = enumf<xf::Trigger>("trigger.allowedTeam", g, NOX_TRK("editor.prop.trigger.allowed_team.label", "Pressed by team"),
                                         NOX_TRK("editor.prop.trigger.allowed_team.help", "0: any team; else only that team."), &xf::Trigger::allowedTeam,
                                         teamValues());
        out.push_back(std::move(d));
        FieldDesc e = enumf<xf::Trigger>("trigger.ignoredTeam", g, NOX_TRK("editor.prop.trigger.ignored_team.label", "Ignores team"),
                                         NOX_TRK("editor.prop.trigger.ignored_team.help", "0: none; else that team never presses it."),
                                         &xf::Trigger::ignoredTeam, teamValues());
        out.push_back(std::move(e));
    }
    {
        const std::vector<NamedValue> bits = {
            {1, NOX_TRK("editor.prop.trigger.flags.pressed", "Pressed (runtime)"),
             NOX_TRK("editor.prop.trigger.flags.pressed_help", "Set while something presses it this frame.")},
            {2, NOX_TRK("editor.prop.trigger.flags.one_shot", "One shot"),
             NOX_TRK("editor.prop.trigger.flags.one_shot_help", "Once released it stays used up (state 5) until it is disabled and enabled again.")},
            {4, NOX_TRK("editor.prop.trigger.flags.pressed_last", "Pressed last frame (runtime)"), kNoText},
            {8, NOX_TRK("editor.prop.trigger.flags.initialised", "Initialised (runtime)"),
             NOX_TRK("editor.prop.trigger.flags.initialised_help", "Cleared while it's disabled, so it re-arms when enabled.")},
        };
        out.push_back(flags<xf::Trigger>("trigger.flags", g, NOX_TRK("editor.prop.trigger.flags.label", "Trigger flags"),
                                         NOX_TRK("editor.prop.trigger.flags.help", "Only One shot is a design setting; the others are its running state."),
                                         &xf::Trigger::unknown1, bits));
    }
    if (v > 0x3C) {
        FieldDesc s = enumf<xf::Trigger>("trigger.state", g, NOX_TRK("editor.prop.trigger.state.label", "State"),
                                         NOX_TRK("editor.prop.trigger.state.help", "Its state: up, pressed, or used up (a one-shot after its release)."),
                                         &xf::Trigger::state,
                                         {{0, NOX_TRK("editor.prop.trigger.state.up", "Up"), kNoText},
                                          {1, NOX_TRK("editor.prop.trigger.state.pressed", "Pressed"), kNoText},
                                          {5, NOX_TRK("editor.prop.trigger.state.used_up", "Used up"),
                                           NOX_TRK("editor.prop.trigger.state.used_up_help", "Ignores every touch.")}});
        out.push_back(adv(s));
        FieldDesc a = num<xf::Trigger>("trigger.anim", g, NOX_TRK("editor.prop.trigger.anim.label", "Drawn down"),
                                       NOX_TRK("editor.prop.trigger.anim.help", "Its drawing: 0 up, 1 down."), &xf::Trigger::animFrame, 0, 1);
        out.push_back(adv(a));
        FieldDesc u = num<xf::Trigger>("trigger.unknown2", g, NOX_TRK("editor.prop.trigger.unknown2.label", "Byte after the state"),
                                       NOX_TRK("editor.prop.trigger.unknown2.help", "Stored and kept; the trigger code never reads it."),
                                       &xf::Trigger::unknown2, 0, 255);
        out.push_back(adv(u));
    }
}

void lightFields(std::vector<FieldDesc>& out)
{
    const Key g = grp::kLight;
    out.push_back(enumf<xf::InvisibleLight>("light.type", g, NOX_TRK("editor.prop.light.type.label", "Type"),
                                            NOX_TRK("editor.prop.light.type.help", "0 none, 1 white, 2 coloured."), &xf::InvisibleLight::type,
                                            range({NOX_TRK("editor.prop.light.type.none", "None"), NOX_TRK("editor.prop.light.type.white", "White"),
                                                   NOX_TRK("editor.prop.light.type.coloured", "Coloured")})));
    {
        // The intensity sets the radius as the game does (lightRadiusFromIntensity), and the 16.16 copy.
        FieldDesc d = on<xf::InvisibleLight>(
            desc("light.intensity", g, NOX_TRK("editor.prop.light.intensity.label", "Intensity"), Kind::Float,
                 NOX_TRK("editor.prop.light.intensity.help",
                         "How bright (0..63); it sets the light's reach (the radius is worked out from it as the game does). Negative: an "
                         "anti-light (darkness)."),
                 -63, 63),
            [](const xf::InvisibleLight& t) { return fv(t.intensity); },
            [](xf::InvisibleLight& t, const FieldValue& x) {
                const float i = static_cast<float>(std::clamp(x.f, -63.0, 63.0));
                t.intensity = i;
                const float a = std::fabs(i);
                t.radius = a <= 1 ? 0 : static_cast<std::int32_t>(std::sqrt(((4 + std::min(a, 31.0f)) / 5 + 1) * a * a / 0.961f));
                t.intensityFx = static_cast<std::int32_t>(i * 65536 + 0.5f);
            });
        out.push_back(std::move(d));
    }
    FieldDesc r = num<xf::InvisibleLight>("light.radius", g, NOX_TRK("editor.prop.light.radius.label", "Radius"),
                                          NOX_TRK("editor.prop.light.radius.help", "The reach as stored (world units); the intensity sets it."),
                                          &xf::InvisibleLight::radius, 0, 2000);
    out.push_back(adv(r));
    out.push_back(on<xf::InvisibleLight>(desc("light.color", g, NOX_TRK("editor.prop.light.color.label", "Colour"), Kind::Color,
                                              NOX_TRK("editor.prop.light.color.help", "The light's colour.")),
                                         [](const xf::InvisibleLight& t) {
                                             return rgbv(clampTo<std::uint8_t>(t.color[0], 0, 255), clampTo<std::uint8_t>(t.color[1], 0, 255),
                                                         clampTo<std::uint8_t>(t.color[2], 0, 255));
                                         },
                                         [](xf::InvisibleLight& t, const FieldValue& x) {
                                             for (int i = 0; i < 3; ++i) t.color[static_cast<std::size_t>(i)] = x.rgb[static_cast<std::size_t>(i)];
                                         }));
    out.push_back(on<xf::InvisibleLight>(desc("light.anti", g, NOX_TRK("editor.prop.light.anti.label", "Anti-light"), Kind::Bool,
                                              NOX_TRK("editor.prop.light.anti.help",
                                                      "It darkens instead (the client takes the colour inverted). Not for a cone: the game's cone always lights.")),
                                         [](const xf::InvisibleLight& t) { return iv(t.isAnti != 0); },
                                         [](xf::InvisibleLight& t, const FieldValue& x) { t.isAnti = x.i ? 1 : 0; }));
    {
        const std::vector<NamedValue> bits = {
            {1, NOX_TRK("editor.prop.light.anim.color_loops", "Colour loops"),
             NOX_TRK("editor.prop.light.anim.color_loops_help", "The colour sequence jumps back to its first key (else it runs back down).")},
            {4, NOX_TRK("editor.prop.light.anim.intensity_loops", "Intensity loops"),
             NOX_TRK("editor.prop.light.anim.intensity_loops_help", "The intensity keys loop (else they run back down: pulsing).")},
            {0x10, NOX_TRK("editor.prop.light.anim.cone_loops", "Cone loops"), NOX_TRK("editor.prop.light.anim.cone_loops_help", "The cone width keys loop.")},
            {0x40, NOX_TRK("editor.prop.light.anim.aims", "Aims at the target"),
             NOX_TRK("editor.prop.light.anim.aims_help",
                     "The cone points at the target object (an extent); an all-round light becomes a cone. Needs a key (below).")},
            {0x80, NOX_TRK("editor.prop.light.anim.rotates", "Rotates"),
             NOX_TRK("editor.prop.light.anim.rotates_help", "The cone turns (not an all-round light). It turns, aims or changes width only if the light has at least one "
                                                            "colour, intensity or cone key.")},
            {0x100, NOX_TRK("editor.prop.light.anim.sweeps", "Rotation sweeps"),
             NOX_TRK("editor.prop.light.anim.sweeps_help", "Turns from its direction to the most, then jumps back to its direction (else all the way round).")},
        };
        out.push_back(flags<xf::InvisibleLight>("light.anim", g, NOX_TRK("editor.prop.light.anim.label", "Animation"),
                                                NOX_TRK("editor.prop.light.anim.help", "What animates and how."), &xf::InvisibleLight::animFlags, bits));
    }
    out.push_back(num<xf::InvisibleLight>("light.colorStep", g, NOX_TRK("editor.prop.light.color_step.label", "Colour step"),
                                          NOX_TRK("editor.prop.light.color_step.help", "Frames from one colour key to the next (30 a second)."),
                                          &xf::InvisibleLight::colorStep, 0, 65535));
    out.push_back(num<xf::InvisibleLight>("light.intensityStep", g, NOX_TRK("editor.prop.light.intensity_step.label", "Intensity step"),
                                          NOX_TRK("editor.prop.light.intensity_step.help", "Frames from one intensity key to the next."),
                                          &xf::InvisibleLight::intensityStep, 0, 65535));
    out.push_back(num<xf::InvisibleLight>("light.coneStep", g, NOX_TRK("editor.prop.light.cone_step.label", "Cone step"),
                                          NOX_TRK("editor.prop.light.cone_step.help", "Frames from one cone key to the next."), &xf::InvisibleLight::coneStep,
                                          0, 65535));
    const auto angle16 = [](const char* id, Key label, Key help, std::uint16_t xf::InvisibleLight::*m) {
        FieldDesc d = on<xf::InvisibleLight>(desc(id, grp::kLightCone, label, Kind::Angle, help, 0, kTwoPi),
                                             [m](const xf::InvisibleLight& t) { return fv((t.*m) * kTwoPi / 65536.0); },
                                             [m](xf::InvisibleLight& t, const FieldValue& x) {
                                                 double a = std::fmod(x.f, kTwoPi);
                                                 if (a < 0) a += kTwoPi;
                                                 t.*m = static_cast<std::uint16_t>(std::lround(a * 65536.0 / kTwoPi) & 0xFFFF);
                                             });
        return d;
    };
    out.push_back(angle16("light.direction", NOX_TRK("editor.prop.light.direction.label", "Direction"),
                          NOX_TRK("editor.prop.light.direction.help", "Where the cone points."), &xf::InvisibleLight::direction));
    out.push_back(angle16("light.cone", NOX_TRK("editor.prop.light.cone.label", "Cone width"),
                          NOX_TRK("editor.prop.light.cone.help",
                                  "How far it lights either side of its direction (half the cone); 0, or 180 degrees or more, lights nothing."),
                          &xf::InvisibleLight::cone));
    out.push_back(on<xf::InvisibleLight>(desc("light.omni", grp::kLightCone, NOX_TRK("editor.prop.light.omni.label", "All round"), Kind::Bool,
                                              NOX_TRK("editor.prop.light.omni.help",
                                                      "No cone: light all round (stored 0xFFFF). Turns off the cone's animation and rotation.")),
                                         [](const xf::InvisibleLight& t) { return iv(t.omni != 0); },
                                         [](xf::InvisibleLight& t, const FieldValue& x) { t.omni = x.i ? 0xFFFFu : 0u; }));
    {
        FieldDesc d = on<xf::InvisibleLight>(desc("light.target", grp::kLightCone, NOX_TRK("editor.prop.light.target.label", "Target"), Kind::Extent,
                                                  NOX_TRK("editor.prop.light.target.help",
                                                          "The object the cone aims at (with \"Aims at the target\"): a static object's extent.")),
                                             [](const xf::InvisibleLight& t) { return iv(t.target); },
                                             [](xf::InvisibleLight& t, const FieldValue& x) { t.target = static_cast<std::uint32_t>(x.i); });
        out.push_back(std::move(d));
    }
    // (The game's colorLightRotate sign-extends the speed: a negative one turns the cone the other
    // way. The most, like the direction it starts from, it takes unsigned.)
    out.push_back(snum16<xf::InvisibleLight>("light.rotationSpeed", grp::kLightCone, NOX_TRK("editor.prop.light.rotation_speed.label", "Rotation speed"),
                                             NOX_TRK("editor.prop.light.rotation_speed.help",
                                                     "Degrees a second, -32768 to 32767: positive turns one way, negative (-30) the other; 0 doesn't turn."),
                                             &xf::InvisibleLight::rotationSpeed));
    out.push_back(num<xf::InvisibleLight>("light.rotationMax", grp::kLightCone, NOX_TRK("editor.prop.light.rotation_max.label", "Rotation most"),
                                          NOX_TRK("editor.prop.light.rotation_max.help", "Degrees it sweeps to (with \"Rotation sweeps\"), 0 and up."),
                                          &xf::InvisibleLight::rotationMax, 0, 65535));
    FieldDesc u = num<xf::InvisibleLight>("light.unused", g, NOX_TRK("editor.prop.light.unused.label", "Unused byte"),
                                          NOX_TRK("editor.prop.light.unused.help", "Saved (0x80 by default), never read."), &xf::InvisibleLight::unused, 0, 255);
    out.push_back(adv(u));
}

template <class T>
void creatureFields(std::vector<FieldDesc>& out, std::uint16_t v, bool npc)
{
    const Key g = npc ? grp::kNpc : grp::kMonster;
    {
        FieldDesc d = on<T>(desc(std::string(npc ? "npc" : "monster") + ".direction", g, NOX_TRK("editor.prop.creature.direction.label", "Facing"), Kind::Direction,
                                 NOX_TRK("editor.prop.creature.direction.help", "Which way it faces at the start: one of the 8 grid steps.")),
                            [](const T& t) { return iv(stepOf(t.direction)); },
                            [](T& t, const FieldValue& x) { t.direction = kSteps[static_cast<std::size_t>(((x.i % 8) + 8) % 8)]; });
        out.push_back(std::move(d));
    }
    const std::string p = npc ? "npc." : "monster.";
    out.push_back(str<T>(p + "action", g, NOX_TRK("editor.prop.creature.action.label", "Starts by"), Kind::Action,
                         NOX_TRK("editor.prop.creature.action.help", "The action it starts with (MonsterInit pushes it: guard, escort, roam...)."), &T::action,
                         255));
    out.push_back(flags<T>(p + "status", grp::kBehaviour, NOX_TRK("editor.prop.creature.status.label", "Status"),
                           NOX_TRK("editor.prop.creature.status.help", "Its status flags (monster.bin's STATUS at the start)."), &T::statusFlags,
                           monsterStatusValues()));
    out.push_back(fbits<T>(p + "sight", grp::kBehaviour, NOX_TRK("editor.prop.creature.sight.label", "Sight range"),
                           NOX_TRK("editor.prop.creature.sight.help", "How far it sees (world units)."), &T::sightRange, 0, 2000));
    out.push_back(fbits<T>(p + "aggression", grp::kBehaviour, NOX_TRK("editor.prop.creature.aggression.label", "Aggression"),
                           NOX_TRK("editor.prop.creature.aggression.help", "How readily it attacks: 0 friendly ... 0.83 hostile (the AI's own numbers "
                                                                           "go higher)."),
                           &T::aggression, 0, 2));
    out.push_back(fbits<T>(p + "retreat", grp::kBehaviour, NOX_TRK("editor.prop.creature.retreat.label", "Retreats at"),
                           NOX_TRK("editor.prop.creature.retreat.help", "The health fraction (0-1) at which it retreats."), &T::retreatLevel, 0, 1));
    out.push_back(fbits<T>(p + "resume", grp::kBehaviour, NOX_TRK("editor.prop.creature.resume.label", "Fights again at"),
                           NOX_TRK("editor.prop.creature.resume.help", "The health fraction (0-1) at which it stops retreating."), &T::resumeLevel, 0, 1));
    out.push_back(str<T>(p + "escort", grp::kBehaviour, NOX_TRK("editor.prop.creature.escort.label", "Escorts"), Kind::Text,
                         NOX_TRK("editor.prop.creature.escort.help", "The script name of whom it escorts (with the escort action)."), &T::unknown341, 255));
    out.push_back(fbits<T>(p + "escortDistance", grp::kBehaviour, NOX_TRK("editor.prop.creature.escort_distance.label", "Escort distance"),
                           NOX_TRK("editor.prop.creature.escort_distance.help", "How close it keeps to whom it escorts."), &T::unknown329, 0, 2000));
    {
        const std::vector<NamedValue> bits = {{0x01, NOX_TRK("editor.prop.creature.roam.class1", "Route class 1"), kNoText},
                                              {0x02, NOX_TRK("editor.prop.creature.roam.class2", "Route class 2"), kNoText},
                                              {0x04, NOX_TRK("editor.prop.creature.roam.class3", "Route class 3"), kNoText},
                                              {0x08, NOX_TRK("editor.prop.creature.roam.class4", "Route class 4"), kNoText},
                                              {0x10, NOX_TRK("editor.prop.creature.roam.class5", "Route class 5"), kNoText},
                                              {0x20, NOX_TRK("editor.prop.creature.roam.class6", "Route class 6"), kNoText},
                                              {0x40, NOX_TRK("editor.prop.creature.roam.class7", "Route class 7"), kNoText},
                                              {0x80, NOX_TRK("editor.prop.creature.roam.default", "Default routes (0x80)"),
                                               NOX_TRK("editor.prop.creature.roam.default_help", "The links stock maps give every waypoint.")}};
        out.push_back(flags<T>(p + "roam", grp::kBehaviour, NOX_TRK("editor.prop.creature.roam.label", "Roams by"),
                               NOX_TRK("editor.prop.creature.roam.help", "Which waypoint links it roams along: the link flags it matches (SetRoamFlag)."),
                               &T::unknown333, bits));
    }
    const Key h = grp::kHandlers;
    out.push_back(handler<T>(p + "onSighted", h, NOX_TRK("editor.prop.creature.on_sighted.label", "Enemy sighted"), kNoText,
                             [](T& t) -> map::ScriptHandler& { return t.scripts.enemySighted; }));
    out.push_back(handler<T>(p + "onDeath", h, NOX_TRK("editor.prop.creature.on_death.label", "Death"), kNoText,
                             [](T& t) -> map::ScriptHandler& { return t.scripts.death; }));
    out.push_back(handler<T>(p + "onLooking", h, NOX_TRK("editor.prop.creature.on_looking.label", "Looking for enemy"), kNoText,
                             [](T& t) -> map::ScriptHandler& { return t.scripts.lookingForEnemy; }));
    out.push_back(handler<T>(p + "onFocus", h, NOX_TRK("editor.prop.creature.on_focus.label", "Change of focus"), kNoText,
                             [](T& t) -> map::ScriptHandler& { return t.scripts.changeFocus; }));
    out.push_back(handler<T>(p + "onHit", h, NOX_TRK("editor.prop.creature.on_hit.label", "Is hit"), kNoText,
                             [](T& t) -> map::ScriptHandler& { return t.scripts.isHit; }));
    out.push_back(handler<T>(p + "onRetreat", h, NOX_TRK("editor.prop.creature.on_retreat.label", "Retreat"), kNoText,
                             [](T& t) -> map::ScriptHandler& { return t.scripts.retreat; }));
    out.push_back(handler<T>(p + "onCollision", h, NOX_TRK("editor.prop.creature.on_collision.label", "Collision"), kNoText,
                             [](T& t) -> map::ScriptHandler& { return t.scripts.collision; }));
    out.push_back(handler<T>(p + "onHear", h, NOX_TRK("editor.prop.creature.on_hear.label", "Hears enemy"), kNoText,
                             [](T& t) -> map::ScriptHandler& { return t.scripts.hearEnemy; }));
    out.push_back(handler<T>(p + "onWaypoint", h, NOX_TRK("editor.prop.creature.on_waypoint.label", "End of waypoint"), kNoText,
                             [](T& t) -> map::ScriptHandler& { return t.scripts.endOfWaypoint; }));
    out.push_back(handler<T>(p + "onLost", h, NOX_TRK("editor.prop.creature.on_lost.label", "Lost enemy"), kNoText,
                             [](T& t) -> map::ScriptHandler& { return t.scripts.lostEnemy; }));
    out.push_back(on<T>(desc(p + "lookDelay", h, NOX_TRK("editor.prop.creature.look_delay.label", "Look delay"), Kind::Int,
                             NOX_TRK("editor.prop.creature.look_delay.help", "Frames between its looks for an enemy (stored with the handlers)."), 0, 65535),
                        [](const T& t) { return iv(t.scripts.unknown305); },
                        [](T& t, const FieldValue& x) { t.scripts.unknown305 = clampTo<std::uint16_t>(x.i, 0, 65535); }));
    // Per use: the least and the most frames between casts.
    static const Key kDelay[5][2] = {
        {NOX_TRK("editor.prop.creature.delay.counter_least", "Counter (least)"), NOX_TRK("editor.prop.creature.delay.counter_most", "Counter (most)")},
        {NOX_TRK("editor.prop.creature.delay.self_least", "On self (least)"), NOX_TRK("editor.prop.creature.delay.self_most", "On self (most)")},
        {NOX_TRK("editor.prop.creature.delay.attack_least", "Attack (least)"), NOX_TRK("editor.prop.creature.delay.attack_most", "Attack (most)")},
        {NOX_TRK("editor.prop.creature.delay.hold_least", "Hold ground (least)"), NOX_TRK("editor.prop.creature.delay.hold_most", "Hold ground (most)")},
        {NOX_TRK("editor.prop.creature.delay.escape_least", "Escape (least)"), NOX_TRK("editor.prop.creature.delay.escape_most", "Escape (most)")},
    };
    static const Key kDelayHelp[5] = {
        NOX_TRK("editor.prop.creature.delay.counter_help", "Frames between casts of its Counter spells: the least and the most."),
        NOX_TRK("editor.prop.creature.delay.self_help", "Frames between casts of its On self spells: the least and the most."),
        NOX_TRK("editor.prop.creature.delay.attack_help", "Frames between casts of its Attack spells: the least and the most."),
        NOX_TRK("editor.prop.creature.delay.hold_help", "Frames between casts of its Hold ground spells: the least and the most."),
        NOX_TRK("editor.prop.creature.delay.escape_help", "Frames between casts of its Escape spells: the least and the most."),
    };
    for (int k = 0; k < 5; ++k) {
        for (int m = 0; m < 2; ++m) {
            const std::size_t at = static_cast<std::size_t>(k * 2 + m);
            out.push_back(on<T>(desc(p + "delay" + std::to_string(at), grp::kDelays, kDelay[k][m], Kind::Int, kDelayHelp[k], 0, 65535),
                                [at](const T& t) { return iv(t.spellDelays[at]); },
                                [at](T& t, const FieldValue& x) { t.spellDelays[at] = clampTo<std::uint16_t>(x.i, 0, 65535); }));
        }
    }
    out.push_back(num<T>(p + "spellPower", grp::kSpells, NOX_TRK("editor.prop.creature.spell_power.label", "Spell power"),
                         NOX_TRK("editor.prop.creature.spell_power.help", "The level its spells are cast at (1-5)."), &T::unknown510, 1, 5));
    out.push_back(fbits<T>(p + "aim", grp::kSpells, NOX_TRK("editor.prop.creature.aim.label", "Aim"),
                           NOX_TRK("editor.prop.creature.aim.help", "Missile and spell aim accuracy, 0-1."), &T::unknown330, 0, 1));
    static const Key kBomb[3] = {NOX_TRK("editor.prop.creature.bomb1.label", "Bomber spell 1"), NOX_TRK("editor.prop.creature.bomb2.label", "Bomber spell 2"),
                                 NOX_TRK("editor.prop.creature.bomb3.label", "Bomber spell 3")};
    for (int k = 0; k < 3; ++k) {
        const std::size_t at = static_cast<std::size_t>(k);
        out.push_back(on<T>(desc(p + "bomb" + std::to_string(k), grp::kSpells, kBomb[k], Kind::Spell,
                                 NOX_TRK("editor.prop.creature.bomb.help", "A bomber's spells (it carries a glyph of them)."), 0, 255),
                            [at](const T& t) { return sv(t.defaultSpells[at]); },
                            [at](T& t, const FieldValue& x) { t.defaultSpells[at] = x.s; }));
    }
    out.push_back(fbits<T>(p + "startHealth", grp::kCombat, NOX_TRK("editor.prop.creature.start_health.label", "Starting health"),
                           NOX_TRK("editor.prop.creature.start_health.help", "Its health at the start as a fraction of its most."), &T::unknown338, 0, 1));
    out.push_back(on<T>(desc(p + "immortal", grp::kCombat, NOX_TRK("editor.prop.creature.immortal.label", "Immortal"), Kind::Bool,
                             NOX_TRK("editor.prop.creature.immortal.help",
                                     "The map's health kept: the host gives it no health at all, so it can't be killed (the story's maidens and captains).")),
                        [](const T& t) { return iv(t.unknown361 != 0); }, [](T& t, const FieldValue& x) { t.unknown361 = x.i ? 1 : 0; }));
    constexpr Key kPoisoned = NOX_TRK("editor.prop.creature.poison.label", "Poisoned");
    constexpr Key kPoisonHelp = NOX_TRK("editor.prop.creature.poison.help", "Its poison level at the start (0 none).");
    if constexpr (std::is_same_v<T, xf::Monster>) {
        if (v >= 0x40) out.push_back(num<T>(p + "poison", grp::kCombat, kPoisoned, kPoisonHelp, &T::poison, 0, 255));
    } else {
        if (v >= 0x3E) out.push_back(num<T>(p + "poison", grp::kCombat, kPoisoned, kPoisonHelp, &T::poison, 0, 255));
    }
    FieldDesc poly = num<T>(p + "polygon", grp::kAdvanced, NOX_TRK("editor.prop.creature.polygon.label", "Polygon"),
                            NOX_TRK("editor.prop.creature.polygon.help", "The polygon it was in when saved (0xDEADFACE: unknown). The game rewrites it."),
                            &T::unknown0, 0, 0xFFFFFFFF);
    poly.readOnly = true;
    out.push_back(adv(poly));
    FieldDesc sub = flags<T>(p + "subclass", grp::kAdvanced, NOX_TRK("editor.prop.creature.subclass.label", "Subclass bits added"),
                             NOX_TRK("editor.prop.creature.subclass.help", "SUBCLASS bits (0x180) added to its type's."), &T::subclassBits, {});
    out.push_back(adv(sub));
    FieldDesc u1 = num<T>(p + "unknown1", grp::kAdvanced, NOX_TRK("editor.prop.creature.unknown1.label", "Word after the handlers"),
                          NOX_TRK("editor.prop.creature.unknown1.help", "Read and dropped by the game."), &T::unknown1, 0, 0xFFFFFFFF);
    out.push_back(adv(u1));
}

/// A maiden's or NPC's colours by slot.
const Key kColourNames[6] = {
    NOX_TRK("editor.prop.creature.color.hair", "Hair colour"),         NOX_TRK("editor.prop.creature.color.hair2", "Hair 2 colour"),
    NOX_TRK("editor.prop.creature.color.sideburns", "Sideburns colour"), NOX_TRK("editor.prop.creature.color.beard", "Beard colour"),
    NOX_TRK("editor.prop.creature.color.skin", "Skin colour"),         NOX_TRK("editor.prop.creature.color.skin2", "Skin 2 colour"),
};

void monsterOnly(std::vector<FieldDesc>& out, const DocObject& o, std::uint16_t v)
{
    const auto& m = std::get<xf::Monster>(o.data);
    out.push_back(num<xf::Monster>("monster.health", grp::kCombat, NOX_TRK("editor.prop.monster.health.label", "Health"),
                                   NOX_TRK("editor.prop.monster.health.help", "Its health (monster.bin's for a new one)."), &xf::Monster::health, 0, 65535));
    static const Key kFrom[4] = {NOX_TRK("editor.prop.monster.from_type.resume", "Resume level from its type"),
                                 NOX_TRK("editor.prop.monster.from_type.retreat", "Retreat level from its type"),
                                 NOX_TRK("editor.prop.monster.from_type.status", "Status from its type"),
                                 NOX_TRK("editor.prop.monster.from_type.spells", "Spells from its type")};
    for (int k = 0; k < 4; ++k) {
        const std::size_t at = static_cast<std::size_t>(k);
        out.push_back(on<xf::Monster>(desc("monster.fromType" + std::to_string(k), grp::kDefaults, kFrom[k], Kind::Bool,
                                           NOX_TRK("editor.prop.monster.from_type.help", "Ticked: the game takes this from monster.bin when it spawns, not from the map.")),
                                      [at](const xf::Monster& t) { return iv(t.unknown337[at] != 0); },
                                      [at](xf::Monster& t, const FieldValue& x) { t.unknown337[at] = x.i ? 1 : 0; }));
    }
    if (m.colors) {
        for (int k = 0; k < 6; ++k) {
            const std::size_t at = static_cast<std::size_t>(k);
            out.push_back(on<xf::Monster>(desc("monster.color" + std::to_string(k), grp::kLook, kColourNames[k], Kind::Color,
                                               NOX_TRK("editor.prop.monster.color.help", "A maiden's colour.")),
                                          [at](const xf::Monster& t) { return t.colors ? rgbv((*t.colors)[at][0], (*t.colors)[at][1], (*t.colors)[at][2]) : FieldValue{}; },
                                          [at](xf::Monster& t, const FieldValue& x) {
                                              if (t.colors) (*t.colors)[at] = x.rgb;
                                          }));
        }
    }
    constexpr Key kVoice = NOX_TRK("editor.prop.creature.voice.label", "Voice");
    constexpr Key kVoiceHelp = NOX_TRK("editor.prop.creature.voice.help", "Its sound set (soundset.bin).");
    if (m.voiceSet) {
        out.push_back(on<xf::Monster>(desc("monster.voice", grp::kLook, kVoice, Kind::Voice, kVoiceHelp),
                                      [](const xf::Monster& t) { return sv(t.voiceSet ? *t.voiceSet : std::string()); },
                                      [](xf::Monster& t, const FieldValue& x) {
                                          if (t.voiceSet) t.voiceSet = x.s;
                                      }));
    }
    if (m.voiceSet2 && v > 0x3E) {
        out.push_back(on<xf::Monster>(desc("monster.voice2", grp::kLook, kVoice, Kind::Voice,
                                           NOX_TRK("editor.prop.monster.voice2.help", "A wounded NPC's sound set (soundset.bin).")),
                                      [](const xf::Monster& t) { return sv(t.voiceSet2 ? *t.voiceSet2 : std::string()); },
                                      [](xf::Monster& t, const FieldValue& x) {
                                          if (t.voiceSet2) t.voiceSet2 = x.s;
                                      }));
    }
    if (m.shop) {
        out.push_back(on<xf::Monster>(desc("monster.shopBuy", grp::kShop, NOX_TRK("editor.prop.monster.shop_buy.label", "Buys at"), Kind::Float,
                                           NOX_TRK("editor.prop.monster.shop_buy.help", "What it pays, as a fraction of an item's worth."), 0, 10),
                                      [](const xf::Monster& t) { return fv(t.shop ? bitsFloat(t.shop->unknown1) : 0); },
                                      [](xf::Monster& t, const FieldValue& x) {
                                          if (t.shop) t.shop->unknown1 = floatBits(std::clamp(x.f, 0.0, 10.0));
                                      }));
        if (v > 0x3C) {
            out.push_back(on<xf::Monster>(desc("monster.shopSell", grp::kShop, NOX_TRK("editor.prop.monster.shop_sell.label", "Sells at"), Kind::Float,
                                               NOX_TRK("editor.prop.monster.shop_sell.help", "What it asks, as a multiple of an item's worth."), 0, 10),
                                          [](const xf::Monster& t) { return fv(t.shop ? bitsFloat(t.shop->unknown2) : 0); },
                                          [](xf::Monster& t, const FieldValue& x) {
                                              if (t.shop) t.shop->unknown2 = floatBits(std::clamp(x.f, 0.0, 10.0));
                                          }));
        }
        out.push_back(on<xf::Monster>(desc("monster.shopGreeting", grp::kShop, NOX_TRK("editor.prop.monster.shop_greeting.label", "Greeting"), Kind::StringKey,
                                           NOX_TRK("editor.prop.monster.shop_greeting.help", "Its greeting: a string table key."), 0, 255),
                                      [](const xf::Monster& t) { return sv(t.shop ? t.shop->name : std::string()); },
                                      [](xf::Monster& t, const FieldValue& x) {
                                          if (t.shop) t.shop->name = x.s.substr(0, 255);
                                      }));
    }
}

void npcOnly(std::vector<FieldDesc>& out)
{
    for (int k = 0; k < 6; ++k) {
        const std::size_t at = static_cast<std::size_t>(k);
        out.push_back(on<xf::Npc>(desc("npc.color" + std::to_string(k), grp::kLook, kColourNames[k], Kind::Color,
                                       NOX_TRK("editor.prop.npc.color.help", "Its colour (setNPCColor).")),
                                  [at](const xf::Npc& t) { return rgbv(t.colors[at][0], t.colors[at][1], t.colors[at][2]); },
                                  [at](xf::Npc& t, const FieldValue& x) { t.colors[at] = x.rgb; }));
    }
    out.push_back(str<xf::Npc>("npc.voice", grp::kLook, NOX_TRK("editor.prop.creature.voice.label", "Voice"), Kind::Voice,
                               NOX_TRK("editor.prop.creature.voice.help", "Its sound set (soundset.bin)."), &xf::Npc::voiceSet, 255));
    out.push_back(num<xf::Npc>("npc.healthMax", grp::kCombat, NOX_TRK("editor.prop.npc.health_max.label", "Most health"),
                               NOX_TRK("editor.prop.npc.health_max.help", "Its most health."), &xf::Npc::healthMax, 0, 65535));
    out.push_back(num<xf::Npc>("npc.health", grp::kCombat, NOX_TRK("editor.prop.npc.health.label", "Health"),
                               NOX_TRK("editor.prop.npc.health.help", "Its health now."), &xf::Npc::healthCurrent, 0, 65535));
    out.push_back(num<xf::Npc>("npc.strength", grp::kCombat, NOX_TRK("editor.prop.npc.strength.label", "Strength"),
                               NOX_TRK("editor.prop.npc.strength.help", "Its strength (as a player's: damage and what it can carry)."), &xf::Npc::unknown331, 0,
                               255));
    out.push_back(fbits<xf::Npc>("npc.speed", grp::kCombat, NOX_TRK("editor.prop.npc.speed.label", "Speed"),
                                 NOX_TRK("editor.prop.npc.speed.help", "Its speed stat: base speed = this / 2 + 1.7."), &xf::Npc::unknown332, 0, 10));
    out.push_back(fbits<xf::Npc>("npc.experience", grp::kCombat, NOX_TRK("editor.prop.npc.experience.label", "Experience"),
                                 NOX_TRK("editor.prop.npc.experience.help", "What killing it is worth."), &xf::Npc::experience, 0, 1e7));
}

void generatorFields(std::vector<FieldDesc>& out, std::uint16_t v)
{
    const Key g = grp::kGenerator;
    out.push_back(handler<xf::MonsterGenerator>("gen.onHurt", g, NOX_TRK("editor.prop.gen.on_hurt.label", "Damaged"), kNoText,
                                                [](xf::MonsterGenerator& t) -> map::ScriptHandler& { return t.scripts[0]; }));
    out.push_back(handler<xf::MonsterGenerator>("gen.onDie", g, NOX_TRK("editor.prop.gen.on_die.label", "Destroyed"), kNoText,
                                                [](xf::MonsterGenerator& t) -> map::ScriptHandler& { return t.scripts[1]; }));
    out.push_back(handler<xf::MonsterGenerator>("gen.onCollide", g, NOX_TRK("editor.prop.gen.on_collide.label", "Collided"), kNoText,
                                                [](xf::MonsterGenerator& t) -> map::ScriptHandler& { return t.scripts[2]; }));
    out.push_back(handler<xf::MonsterGenerator>("gen.onSpawn", g, NOX_TRK("editor.prop.gen.on_spawn.label", "Spawned a monster"), kNoText,
                                                [](xf::MonsterGenerator& t) -> map::ScriptHandler& { return t.scripts[3]; }));
    if (v > 0x3E) {
        out.push_back(flags<xf::MonsterGenerator>(
            "gen.mode", g, NOX_TRK("editor.prop.gen.mode.label", "Spawns when"),
            NOX_TRK("editor.prop.gen.mode.help", "None ticked: it never spawns. The old editor's \"algorithm\" 0-3 (3 acts as 1)."), &xf::MonsterGenerator::unknown6,
            {{2, NOX_TRK("editor.prop.gen.mode.near", "A player is near"),
              NOX_TRK("editor.prop.gen.mode.near_help", "Any live player within 300 units (then the next bit is ignored).")},
             {4, NOX_TRK("editor.prop.gen.mode.sees", "A player can see it"),
              NOX_TRK("editor.prop.gen.mode.sees_help", "A random player within a screen's distance with a clear line: the monster comes out toward them.")}}));
    }
    out.push_back(num<xf::MonsterGenerator>("gen.alive", g, NOX_TRK("editor.prop.gen.alive.label", "Alive now"),
                                            NOX_TRK("editor.prop.gen.alive.help", "How many of its monsters are alive (the game keeps count)."),
                                            &xf::MonsterGenerator::unknown2, 0, 255));
    out.back().readOnly = true;
    out.back().advanced = true;
    out.push_back(num<xf::MonsterGenerator>("gen.most", g, NOX_TRK("editor.prop.gen.most.label", "Most alive"),
                                            NOX_TRK("editor.prop.gen.most.help", "The most of its monsters alive at once (the limit below sets it on load)."),
                                            &xf::MonsterGenerator::unknown3, 0, 255));
    FieldDesc last = num<xf::MonsterGenerator>("gen.last", g, NOX_TRK("editor.prop.gen.last.label", "Last spawn frame"),
                                               NOX_TRK("editor.prop.gen.last.help", "The frame it last spawned (running state)."), &xf::MonsterGenerator::unknown4,
                                               0, 0xFFFFFFFF);
    out.push_back(adv(last));
    // Per slot (quest difficulty variant): the rate and the limit.
    static const Key kRate[3] = {NOX_TRK("editor.prop.gen.rate1.label", "Spawn rate, variant 1"), NOX_TRK("editor.prop.gen.rate2.label", "Spawn rate, variant 2"),
                                 NOX_TRK("editor.prop.gen.rate3.label", "Spawn rate, variant 3")};
    static const Key kLimit[3] = {NOX_TRK("editor.prop.gen.limit1.label", "Limit, variant 1"), NOX_TRK("editor.prop.gen.limit2.label", "Limit, variant 2"),
                                  NOX_TRK("editor.prop.gen.limit3.label", "Limit, variant 3")};
    constexpr Key kHigh = NOX_TRK("editor.prop.gen.level.high", "High");
    constexpr Key kNormal = NOX_TRK("editor.prop.gen.level.normal", "Normal");
    constexpr Key kLow = NOX_TRK("editor.prop.gen.level.low", "Low");
    for (int k = 0; k < 3; ++k) {
        const std::size_t at = static_cast<std::size_t>(k);
        FieldDesc rate = on<xf::MonsterGenerator>(
            desc("gen.rate" + std::to_string(k), grp::kSlots, kRate[k], Kind::Enum,
                 NOX_TRK("editor.prop.gen.rate.help",
                         "How often it spawns (gamedata's SpawnRate values; a quest's hardcore stages shorten it). The game uses slot 1 outside "
                         "quests; a quest picks one of the three at random each time the map loads."),
                 0, 255),
            [at](const xf::MonsterGenerator& t) { return iv(at < t.unknown1.size() ? t.unknown1[at] : 0); },
            [at](xf::MonsterGenerator& t, const FieldValue& x) {
                if (t.unknown1.size() <= at) t.unknown1.resize(at + 1, 0);
                t.unknown1[at] = clampTo<std::uint8_t>(x.i, 0, 255);
            });
        rate.names = range({kHigh, kNormal, kLow, NOX_TRK("editor.prop.gen.level.very_low", "Very low"),
                            NOX_TRK("editor.prop.gen.level.very_very_low", "Very very low")});
        out.push_back(std::move(rate));
        if (v > 0x3D) {
            FieldDesc most = on<xf::MonsterGenerator>(
                desc("gen.limit" + std::to_string(k), grp::kSlots, kLimit[k], Kind::Enum,
                     NOX_TRK("editor.prop.gen.limit.help",
                             "The most alive at once for this slot (gamedata's GeneratorMaxActiveCreatures: High, Normal, Low, Singular); another "
                             "value keeps \"Most alive\"."),
                     0, 255),
                [at](const xf::MonsterGenerator& t) { return iv(at < t.unknown5.size() ? t.unknown5[at] : 255); },
                [at](xf::MonsterGenerator& t, const FieldValue& x) {
                    if (t.unknown5.size() <= at) t.unknown5.resize(at + 1, 255);
                    t.unknown5[at] = clampTo<std::uint8_t>(x.i, 0, 255);
                });
            most.names = {{0, kHigh, kNoText},
                          {1, kNormal, kNoText},
                          {2, kLow, kNoText},
                          {3, NOX_TRK("editor.prop.gen.level.singular", "Singular"), kNoText},
                          {255, NOX_TRK("editor.prop.gen.limit.keep", "Keep \"Most alive\""), kNoText}};
            out.push_back(std::move(most));
        }
    }
}

void rewardFields(std::vector<FieldDesc>& out, std::uint16_t v)
{
    const Key g = grp::kReward;
    out.push_back(flags<xf::RewardMarker>("reward.kinds", g, NOX_TRK("editor.prop.reward.kinds.label", "Can give"),
                                          NOX_TRK("editor.prop.reward.kinds.help", "The kinds of reward it may make (weighted 16/2/2/24/16/23/16/1 in that order)."),
                                          &xf::RewardMarker::rewardType,
                                          {{1, NOX_TRK("editor.prop.reward.kinds.spell_book", "Spell book"), kNoText},
                                           {2, NOX_TRK("editor.prop.reward.kinds.ability_book", "Ability book"), kNoText},
                                           {4, NOX_TRK("editor.prop.reward.kinds.field_guide", "Field guide"), kNoText},
                                           {8, NOX_TRK("editor.prop.reward.kinds.weapon", "Weapon"), kNoText},
                                           {0x10, NOX_TRK("editor.prop.reward.kinds.armour", "Armour"), kNoText},
                                           {0x20, NOX_TRK("editor.prop.reward.kinds.gold", "Gold or gem"), kNoText},
                                           {0x40, NOX_TRK("editor.prop.reward.kinds.potion", "Potion"), kNoText},
                                           {0x80, NOX_TRK("editor.prop.reward.kinds.gold2", "Gold or gem (2)"),
                                            NOX_TRK("editor.prop.reward.kinds.gold2_help", "Also the quest ankh's candidates.")}}));
    out.push_back(flags<xf::RewardMarker>("reward.lists", g, NOX_TRK("editor.prop.reward.lists.label", "Use the map's lists"),
                                          NOX_TRK("editor.prop.reward.lists.help", "Books come from the lists below (else from the stage's tables)."),
                                          &xf::RewardMarker::unknown1,
                                          {{1, NOX_TRK("editor.prop.reward.lists.spells", "Spells"), kNoText},
                                           {2, NOX_TRK("editor.prop.reward.lists.abilities", "Abilities"), kNoText},
                                           {4, NOX_TRK("editor.prop.reward.lists.field_guides", "Field guides"), kNoText}}));
    if (v > 0x3D) {
        out.push_back(enumf<xf::RewardMarker>("reward.chance", g, NOX_TRK("editor.prop.reward.chance.label", "Chance"),
                                              NOX_TRK("editor.prop.reward.chance.help", "How likely it makes anything at all (the game's roll out of 100)."),
                                              &xf::RewardMarker::activateChance,
                                              {{0, NOX_TRK("editor.prop.reward.chance.always", "Always"), kNoText},
                                               {1, game("75%"), kNoText},
                                               {2, game("50%"), kNoText},
                                               {3, game("25%"), kNoText},
                                               {4, game("5%"), kNoText}}));
    }
    if (v > 0x3E) {
        out.push_back(flags<xf::RewardMarker>("reward.flags", g, NOX_TRK("editor.prop.reward.flags.label", "Flags"), kNoText, &xf::RewardMarker::rareOrSpecial,
                                              {{1, NOX_TRK("editor.prop.reward.flags.always", "Always placed"),
                                                NOX_TRK("editor.prop.reward.flags.always_help", "Never left out when the level picks its rewards.")},
                                               {0x80, NOX_TRK("editor.prop.reward.flags.chosen", "Chosen (runtime)"),
                                                NOX_TRK("editor.prop.reward.flags.chosen_help", "Set while the level picks.")}}));
    }
}

} // namespace

// --- Names -------------------------------------------------------------------------------------------------

namespace {

// The authoring bits first (the save's mask 0x11408162), then the runtime-only ones.
const Bit kObjectFlags[] = {
    {0x2, game("NO_UPDATE"), NOX_TRK("editor.prop.flag.no_update.help", "Never updated (frozen).")},
    {0x20, game("DESTROYED"), NOX_TRK("editor.prop.flag.destroyed.help", "Already destroyed: the load doesn't place it.")},
    {0x40, game("NO_COLLIDE"), NOX_TRK("editor.prop.flag.no_collide.help", "Nothing collides with it.")},
    {0x100, game("EQUIPPED"), NOX_TRK("editor.prop.flag.equipped.help", "Worn or wielded by whoever carries it.")},
    {0x8000, game("DEAD"), NOX_TRK("editor.prop.flag.dead.help", "A dead creature (a body).")},
    {0x400000, game("TRANSIENT"), NOX_TRK("editor.prop.flag.transient.help", "Decays after its decay frames.")},
    {0x1000000, game("ENABLED"), NOX_TRK("editor.prop.flag.enabled.help", "On (doors, lights, generators, triggers...; scripts turn it on and off).")},
    {0x10000000, game("NO_AUTO_DROP"), NOX_TRK("editor.prop.flag.no_auto_drop.help", "Not dropped when its carrier dies.")},
    // Runtime-only (the save never writes them; a map that stores one sets it).
    {0x1, game("BELOW"), kNoText},
    {0x4, game("ACTIVE"), NOX_TRK("editor.prop.flag.active.help", "In the world: the load doesn't place an object stored with it.")},
    {0x8, game("ALLOW_OVERLAP"), kNoText},
    {0x10, game("SHORT"), kNoText},
    {0x80, game("MISSILE_HIT"), kNoText},
    {0x200, game("PARTITIONED"), kNoText},
    {0x400, game("NO_COLLIDE_OWNER"), kNoText},
    {0x800, game("OWNER_VISIBLE"), kNoText},
    {0x1000, game("EDIT_VISIBLE"), kNoText},
    {0x2000, game("NO_PUSH_CHARACTERS"), kNoText},
    {0x4000, game("AIRBORNE"), kNoText},
    {0x10000, game("SHADOW"), kNoText},
    {0x20000, game("FALLING"), kNoText},
    {0x40000, game("IN_HOLE"), kNoText},
    {0x80000, game("RESPAWN"), kNoText},
    {0x100000, game("ON_OBJECT"), kNoText},
    {0x200000, game("SIGHT_DESTROY"), kNoText},
    {0x800000, game("BOUNCY"), kNoText},
    {0x2000000, game("PENDING"), kNoText},
    {0x4000000, game("TRANSLUCENT"), kNoText},
    {0x8000000, game("STILL"), kNoText},
    {0x20000000, game("FLICKER"), kNoText},
    {0x40000000, game("SELECTED"), kNoText},
    {0x80000000u, game("MARKED"), kNoText},
};

const Bit kXStatus[] = {
    {0x2, NOX_TRK("editor.prop.xstatus.shut", "Shut"), NOX_TRK("editor.prop.xstatus.shut_help", "A chest's, breakable's or trap door's state: shut.")},
    {0x4, NOX_TRK("editor.prop.xstatus.opening", "Opening"), NOX_TRK("editor.prop.xstatus.opening_help", "... opening.")},
    {0x8, NOX_TRK("editor.prop.xstatus.open", "Open"), NOX_TRK("editor.prop.xstatus.open_help", "... open.")},
    {0x10, NOX_TRK("editor.prop.xstatus.dialog", "Has a dialog"), NOX_TRK("editor.prop.xstatus.dialog_help", "An NPC with a dialog.")},
    {0x40, NOX_TRK("editor.prop.xstatus.bit40", "Bit 0x40"), NOX_TRK("editor.prop.xstatus.bit40_help", "Saved; its meaning isn't known.")},
};

namespace ms = things::MonsterStatus;

const Bit kMonsterStatus[] = {
    {ms::DestroyWhenDead, game("DESTROY_WHEN_DEAD"), NOX_TRK("editor.prop.status.destroy_when_dead.help", "Its body vanishes.")},
    {ms::Check, game("CHECK"), kNoText},
    {ms::CanBlock, game("CAN_BLOCK"), kNoText},
    {ms::CanDodge, game("CAN_DODGE"), kNoText},
    {1u << 4, game("(bit 4)"), NOX_TRK("editor.prop.status.bit4.help", "Not used.")},
    {ms::CanCastSpells, game("CAN_CAST_SPELLS"), kNoText},
    {ms::HoldYourGround, game("HOLD_YOUR_GROUND"), kNoText},
    {ms::Summoned, game("SUMMONED"), kNoText},
    {ms::Alert, game("ALERT"), kNoText},
    {ms::Injured, game("INJURED"), NOX_TRK("editor.prop.status.injured.help", "Hurt this frame (runtime).")},
    {ms::CanSeeFriends, game("CAN_SEE_FRIENDS"), kNoText},
    {ms::CanHealSelf, game("CAN_HEAL_SELF"), kNoText},
    {ms::CanHealOthers, game("CAN_HEAL_OTHERS"), kNoText},
    {ms::CanRun, game("CAN_RUN"), kNoText},
    {ms::Running, game("RUNNING"), kNoText},
    {ms::AlwaysRun, game("ALWAYS_RUN"), kNoText},
    {ms::NeverRun, game("NEVER_RUN"), kNoText},
    {ms::Bot, game("BOT"), kNoText},
    {ms::Morphed, game("MORPHED"), NOX_TRK("editor.prop.status.morphed.help", "A mimic as a chest.")},
    {ms::OnFire, game("ON_FIRE"), kNoText},
    {ms::StayDead, game("STAY_DEAD"), kNoText},
    {ms::Frustrated, game("FRUSTRATED"), kNoText},
};

const Bit kSpellUsage[] = {
    {0x08000000u, NOX_TRK("editor.prop.spell_use.counter", "Counter"), NOX_TRK("editor.prop.spell_use.counter_help", "Cast against an incoming spell.")},
    {0x10000000u, NOX_TRK("editor.prop.spell_use.self", "On self"), NOX_TRK("editor.prop.spell_use.self_help", "A buff on itself.")},
    {0x20000000u, NOX_TRK("editor.prop.spell_use.attack", "Attack"), kNoText},
    {0x40000000u, NOX_TRK("editor.prop.spell_use.hold", "Hold ground"), NOX_TRK("editor.prop.spell_use.hold_help", "Summons and defences while it holds.")},
    {0x80000000u, NOX_TRK("editor.prop.spell_use.escape", "Escape"), kNoText},
};

const std::vector<NamedValue>& objectFlagValues()
{
    static const std::vector<NamedValue> values = named(kObjectFlags);
    return values;
}

const std::vector<NamedValue>& xStatusValues()
{
    static const std::vector<NamedValue> values = named(kXStatus);
    return values;
}

const std::vector<NamedValue>& monsterStatusValues()
{
    static const std::vector<NamedValue> values = named(kMonsterStatus);
    return values;
}

} // namespace

const char* shownText(const nox::l10n::Key& text)
{
    if (!text.id || !*text.id) return text.en ? text.en : "";
    return nox::l10n::tr(text).c_str();
}

const std::vector<FlagBit>& objectFlagBits()
{
    static const std::vector<FlagBit> bits = english(kObjectFlags);
    return bits;
}

const std::vector<FlagBit>& xStatusBits()
{
    static const std::vector<FlagBit> bits = english(kXStatus);
    return bits;
}

const std::vector<TeamName>& teamNames()
{
    static const std::vector<TeamName> teams = {
        {0, NOX_TRK("editor.prop.team.none", "None"), {128, 128, 128}},
        {1, NOX_TRK("editor.prop.team.red", "1 (red)"), {230, 40, 40}},
        {2, NOX_TRK("editor.prop.team.blue", "2 (blue)"), {50, 90, 240}},
        {3, NOX_TRK("editor.prop.team.green", "3 (green)"), {40, 200, 60}},
        {4, NOX_TRK("editor.prop.team.cyan", "4 (cyan)"), {40, 220, 220}},
        {5, NOX_TRK("editor.prop.team.yellow", "5 (yellow)"), {240, 230, 50}},
        {6, NOX_TRK("editor.prop.team.violet", "6 (violet)"), {170, 70, 230}},
        {7, NOX_TRK("editor.prop.team.black", "7 (black)"), {30, 30, 30}},
        {8, NOX_TRK("editor.prop.team.white", "8 (white)"), {240, 240, 240}},
        {9, NOX_TRK("editor.prop.team.orange", "9 (orange)"), {250, 150, 40}},
    };
    return teams;
}

const std::vector<FlagBit>& monsterStatusBits()
{
    static const std::vector<FlagBit> bits = english(kMonsterStatus);
    return bits;
}

const std::vector<FlagBit>& spellUsageBits()
{
    static const std::vector<FlagBit> bits = english(kSpellUsage);
    return bits;
}

const std::vector<NamedValue>& spellUsageValues()
{
    static const std::vector<NamedValue> values = named(kSpellUsage);
    return values;
}

std::vector<std::string> actionNames()
{
    std::vector<std::string> out;
    for (int a = 0; a < server::MonsterAction::Invalid; ++a) {
        if (const char* n = server::MonsterAction::name(a)) out.emplace_back(n);
    }
    return out;
}

std::vector<std::string> enchantNames()
{
    std::vector<std::string> out;
    for (const auto n : server::kEnchantNames) {
        if (!n.empty()) out.emplace_back(n);
    }
    return out;
}

std::vector<std::string> namesFor(FieldKind kind, const FieldContext& ctx, std::string_view item, int slot)
{
    std::vector<std::string> out;
    switch (kind) {
    case Kind::Spell:
        for (std::uint32_t i = 1; i < things::kSpellCount; ++i) {
            const auto n = things::spellName(i);
            if (!n.empty()) out.emplace_back(n);
        }
        break;
    case Kind::Ability:
        out = {"ABILITY_BERSERKER_CHARGE", "ABILITY_WARCRY", "ABILITY_HARPOON", "ABILITY_TREAD_LIGHTLY", "ABILITY_EYE_OF_THE_WOLF"};
        break;
    case Kind::Creature:
        for (const auto n : things::kGuideNames) out.emplace_back(n);
        break;
    case Kind::Action: out = actionNames(); break;
    case Kind::Voice:
        if (ctx.soundSets) {
            for (const auto& s : ctx.soundSets->sets) {
                if (std::ranges::find(out, s.name) == out.end()) out.push_back(s.name);
            }
            std::ranges::sort(out);
        }
        break;
    case Kind::Modifier:
        if (ctx.modifiers) {
            const std::uint32_t wb = things::weaponBitOf(item);
            const std::uint32_t ab = things::armorBitOf(item);
            const char* section = slot == 0 ? "EFFECTIVENESS" : slot == 1 ? "MATERIAL" : "ENCHANTMENT";
            for (const auto& m : ctx.modifiers->modifiers) {
                if (m.section != section) continue;
                if (slot >= 2 && m.allowedPosition != 0 && m.allowedPosition != (slot == 2 ? 1 : 2)) continue;
                const bool fits = (wb && (m.allowedWeapons == 0 || (m.allowedWeapons & wb))) || (ab && (m.allowedArmor == 0 || (m.allowedArmor & ab))) ||
                                  (!wb && !ab);
                if (fits) out.push_back(m.name);
            }
        }
        break;
    case Kind::Thing:
        if (ctx.things) {
            for (const auto& t : ctx.things->things) out.push_back(t.name);
        }
        break;
    default: break;
    }
    return out;
}

// --- Versions -------------------------------------------------------------------------------------------

std::uint16_t currentXferVersion(std::string_view xfer)
{
    // (The version each xfer writes: the save's, object_save.cpp.)
    static constexpr std::pair<std::string_view, std::uint16_t> kVersions[] = {
        {"TriggerXfer", 0x3D},      {"ElevatorXfer", 0x3D}, {"SentryXfer", 0x3D}, {"AbilityRewardXfer", 0x3D},
        {"WeaponXfer", 0x40},       {"ArmorXfer", 0x3E},    {"ObeliskXfer", 0x3D}, {"ToxicCloudXfer", 0x3D},
        {"RewardMarkerXfer", 0x3F}, {"MonsterXfer", 0x40},  {"NPCXfer", 0x3E},    {"MonsterGeneratorXfer", 0x3F}};
    for (const auto& [name, v] : kVersions) {
        if (xfer == name) return v;
    }
    return 0x3C;
}

DocObject upgradedRecord(const DocObject& o, const things::ThingDb& things)
{
    if (std::holds_alternative<xf::Raw>(o.data)) throw FormatError("map editor: a record older than version 0x3C can't be brought up to date");
    DocObject u = o;
    const auto tx = detail::typeXfer(things, o.type);
    const std::uint16_t target = std::max(currentXferVersion(tx.xfer), o.xferVersion);
    if (auto* s = std::get_if<xf::Sentry>(&u.data); s && o.xferVersion <= 0x3C) s->currentPosition = s->basePosition;
    if (auto* g = std::get_if<xf::MonsterGenerator>(&u.data)) {
        if (o.xferVersion <= 0x3E && target > 0x3E) g->unknown6 = 2; // (what MonsterGeneratorCreate gives: spawn near players)
    }
    u.xferVersion = target;
    if (u.header.version < 0x40) {
        u.header.version = 0x40;
        if (u.header.details) {
            if (!u.header.details->onPickup) u.header.details->onPickup = map::ScriptHandler{};
            if (!u.header.details->decayFrames) u.header.details->decayFrames = 0;
        }
    }
    // (The new versions' other fields start at what a reader of the old record had: 0 / empty.)
    return u;
}

// --- The fields ------------------------------------------------------------------------------------------

std::vector<FieldDesc> objectFields(const DocObject& o, const FieldContext& ctx)
{
    std::vector<FieldDesc> out;
    headerFields(o, out);
    const std::uint16_t v = o.xferVersion;
    std::uint32_t classes = 0;
    if (ctx.things) {
        try {
            classes = detail::typeXfer(*ctx.things, o.type).info.classes;
        } catch (const std::exception&) {
        }
    }
    std::visit(
        [&](const auto& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, xf::Door>) {
                doorFields(out, v);
            } else if constexpr (std::is_same_v<T, xf::Trigger>) {
                triggerFields(out, v, o.type != "Trigger");
            } else if constexpr (std::is_same_v<T, xf::InvisibleLight>) {
                lightFields(out);
            } else if constexpr (std::is_same_v<T, xf::Readable>) {
                out.push_back(str<xf::Readable>("readable.text", grp::kReadable, NOX_TRK("editor.prop.readable.text.label", "Text"), Kind::StringKey,
                                                NOX_TRK("editor.prop.readable.text.help",
                                                        "What it says: a string table key (\"file:name\") the game looks up (a missing key shows the "
                                                        "key itself)."),
                                                &xf::Readable::text, 255));
            } else if constexpr (std::is_same_v<T, xf::Exit>) {
                out.push_back(str<xf::Exit>("exit.map", grp::kExit, NOX_TRK("editor.prop.exit.map.label", "Leads to"), Kind::MapName,
                                            NOX_TRK("editor.prop.exit.map.help", "The map it takes the players to (the game's buffer holds 79 bytes and the NUL)."),
                                            &xf::Exit::mapName, 79));
                out.push_back(on<xf::Exit>(desc("exit.point", grp::kExit, NOX_TRK("editor.prop.exit.point.label", "Return point"), Kind::Point,
                                                NOX_TRK("editor.prop.exit.point.help",
                                                        "Not used by the exit's own travel: in solo it's where the player comes back to on this map; a "
                                                        "quest's warp gate puts the players there."),
                                                0, kWorldSize - 1),
                                           [](const xf::Exit& t) {
                                               FieldValue r;
                                               r.f = t.x;
                                               r.g = t.y;
                                               return r;
                                           },
                                           [](xf::Exit& t, const FieldValue& r) {
                                               t.x = static_cast<float>(r.f);
                                               t.y = static_cast<float>(r.g);
                                           }));
            } else if constexpr (std::is_same_v<T, xf::Hole>) {
                out.push_back(on<xf::Hole>(desc("hole.fall", grp::kPit, NOX_TRK("editor.prop.hole.fall.label", "Falls to"), Kind::Point,
                                                NOX_TRK("editor.prop.hole.fall.help", "Where what falls in comes out (world units)."), 0, kWorldSize - 1),
                                           [](const xf::Hole& t) {
                                               FieldValue r;
                                               r.f = t.fallX;
                                               r.g = t.fallY;
                                               return r;
                                           },
                                           [](xf::Hole& t, const FieldValue& r) {
                                               t.fallX = static_cast<std::int32_t>(std::lround(r.f));
                                               t.fallY = static_cast<std::int32_t>(std::lround(r.g));
                                           }));
                out.push_back(handler<xf::Hole>("hole.script", grp::kPit, NOX_TRK("editor.prop.hole.script.label", "Stepped on"),
                                                NOX_TRK("editor.prop.hole.script.help", "Called the first time something steps on it while it's shut."),
                                                [](xf::Hole& t) -> map::ScriptHandler& { return t.script; }));
                out.push_back(num<xf::Hole>("hole.timeout", grp::kPit, NOX_TRK("editor.prop.hole.timeout.label", "Opens after"),
                                            NOX_TRK("editor.prop.hole.timeout.help", "Frames from that first step until it opens (30 a second); 0: never by time."),
                                            &xf::Hole::scriptTimeout, 0, 65535));
                FieldDesc a = num<xf::Hole>("hole.stepped", grp::kPit, NOX_TRK("editor.prop.hole.stepped.label", "Stepped on (state)"),
                                            NOX_TRK("editor.prop.hole.stepped.help", "Running state: it has been stepped on."), &xf::Hole::scriptActivated, 0, 1);
                out.push_back(adv(a));
                FieldDesc t = num<xf::Hole>("hole.opensAt", grp::kPit, NOX_TRK("editor.prop.hole.opens_at.label", "Opens at frame"),
                                            NOX_TRK("editor.prop.hole.opens_at.help", "Running state: the frame it opens (write 0)."), &xf::Hole::scriptTime, 0,
                                            0xFFFFFFFF);
                out.push_back(adv(t));
            } else if constexpr (std::is_same_v<T, xf::Transporter>) {
                FieldDesc d = num<xf::Transporter>("transporter.target", grp::kTeleporter, NOX_TRK("editor.prop.transporter.target.label", "Leads to"),
                                                   NOX_TRK("editor.prop.transporter.target.help", "The teleporter it sends to (an extent; 0 none)."),
                                                   &xf::Transporter::targetExtent, 0, 0x7FFFFFFF);
                d.kind = Kind::Extent;
                out.push_back(std::move(d));
            } else if constexpr (std::is_same_v<T, xf::Elevator>) {
                FieldDesc d = num<xf::Elevator>("elevator.link", grp::kElevator, NOX_TRK("editor.prop.elevator.link.label", "Shaft"),
                                                NOX_TRK("editor.prop.elevator.link.help", "Its shaft (an extent; 0 none)."), &xf::Elevator::linkedExtent, 0,
                                                0x7FFFFFFF);
                d.kind = Kind::Extent;
                out.push_back(std::move(d));
                if (v > 0x28) {
                    FieldDesc h = num<xf::Elevator>("elevator.height", grp::kElevator, NOX_TRK("editor.prop.elevator.height.label", "Height"),
                                                    NOX_TRK("editor.prop.elevator.height.help", "How high it is now (running state)."), &xf::Elevator::height, 0,
                                                    0xFFFFFFFF);
                    out.push_back(adv(h));
                }
                if (v > 0x3C) {
                    out.push_back(enumf<xf::Elevator>("elevator.status", grp::kElevator, NOX_TRK("editor.prop.elevator.status.label", "State"),
                                                      NOX_TRK("editor.prop.elevator.status.help", "Where it is at the start."), &xf::Elevator::status,
                                                      range({NOX_TRK("editor.prop.elevator.status.down", "Down"),
                                                             NOX_TRK("editor.prop.elevator.status.going_down", "Going down"),
                                                             NOX_TRK("editor.prop.elevator.status.up", "Up"),
                                                             NOX_TRK("editor.prop.elevator.status.going_up", "Going up")})));
                }
            } else if constexpr (std::is_same_v<T, xf::ElevatorShaft>) {
                FieldDesc d = num<xf::ElevatorShaft>("shaft.link", grp::kShaft, NOX_TRK("editor.prop.shaft.link.label", "Elevator"),
                                                     NOX_TRK("editor.prop.shaft.link.help", "Its elevator (an extent; 0 none)."),
                                                     &xf::ElevatorShaft::linkedExtent, 0, 0x7FFFFFFF);
                d.kind = Kind::Extent;
                out.push_back(std::move(d));
            } else if constexpr (std::is_same_v<T, xf::Mover>) {
                out.push_back(num<xf::Mover>("mover.speed", grp::kMover, NOX_TRK("editor.prop.mover.speed.label", "Speed"),
                                             NOX_TRK("editor.prop.mover.speed.help", "Its speed: a quarter of a world unit a frame for each."),
                                             &xf::Mover::movingSpeed, 0, 1000));
                FieldDesc w = num<xf::Mover>("mover.waypoint", grp::kMover, NOX_TRK("editor.prop.mover.waypoint.label", "Starts at"),
                                             NOX_TRK("editor.prop.mover.waypoint.help", "The waypoint it heads for first."), &xf::Mover::waypoint, 0, 0x7FFFFFFF);
                w.kind = Kind::Waypoint;
                out.push_back(std::move(w));
                FieldDesc c = num<xf::Mover>("mover.carries", grp::kMover, NOX_TRK("editor.prop.mover.carries.label", "Carries"),
                                             NOX_TRK("editor.prop.mover.carries.help", "The object it moves (an extent)."), &xf::Mover::movedExtent, 0, 0x7FFFFFFF);
                c.kind = Kind::Extent;
                out.push_back(std::move(c));
                out.push_back(enumf<xf::Mover>("mover.state", grp::kMover, NOX_TRK("editor.prop.mover.state.label", "State"),
                                               NOX_TRK("editor.prop.mover.state.help",
                                                       "Ready: when enabled it heads for its first waypoint. At a waypoint with no links it's done; one link, "
                                                       "it follows it; more, a random one but the way it came."),
                                               &xf::Mover::moveType,
                                               range({NOX_TRK("editor.prop.mover.state.ready", "Ready"), NOX_TRK("editor.prop.mover.state.moving", "Moving"),
                                                      NOX_TRK("editor.prop.mover.state.paused", "Paused"), NOX_TRK("editor.prop.mover.state.done", "Done")})));
                FieldDesc t = num<xf::Mover>("mover.target", grp::kMover, NOX_TRK("editor.prop.mover.target.label", "Heading for"),
                                             NOX_TRK("editor.prop.mover.target.help", "Running state: the waypoint it's heading for."), &xf::Mover::waypointStart, 0,
                                             0x7FFFFFFF);
                t.kind = Kind::Waypoint;
                out.push_back(adv(t));
                FieldDesc p = num<xf::Mover>("mover.previous", grp::kMover, NOX_TRK("editor.prop.mover.previous.label", "Came from"),
                                             NOX_TRK("editor.prop.mover.previous.help", "Running state: the waypoint it came from."), &xf::Mover::waypointEnd, 0,
                                             0x7FFFFFFF);
                p.kind = Kind::Waypoint;
                out.push_back(adv(p));
                FieldDesc sb = flt<xf::Mover>("mover.speedBase", grp::kMover, NOX_TRK("editor.prop.mover.speed_base.label", "Base speed"),
                                              NOX_TRK("editor.prop.mover.speed_base.help", "The object's speeds (running state)."), &xf::Mover::speedBase, 0, 1000);
                out.push_back(adv(sb));
                FieldDesc sc = flt<xf::Mover>("mover.speedCurrent", grp::kMover, NOX_TRK("editor.prop.mover.speed_current.label", "Speed now"), kNoText,
                                              &xf::Mover::speedCurrent, 0, 1000);
                out.push_back(adv(sc));
            } else if constexpr (std::is_same_v<T, xf::Glyph>) {
                out.push_back(on<xf::Glyph>(desc("glyph.target", grp::kGlyph, NOX_TRK("editor.prop.glyph.target.label", "Target"), Kind::Point,
                                                 NOX_TRK("editor.prop.glyph.target.help",
                                                         "Where its spells go when nothing touched it. Setting it turns the glyph to face it."),
                                                 0, kWorldSize - 1),
                                            [](const xf::Glyph& t) {
                                                FieldValue r;
                                                r.f = t.targetX;
                                                r.g = t.targetY;
                                                return r;
                                            },
                                            [](xf::Glyph& t, const FieldValue& r) {
                                                t.targetX = static_cast<float>(r.f);
                                                t.targetY = static_cast<float>(r.g);
                                            }));
                out.push_back(num<xf::Glyph>("glyph.direction", grp::kGlyph, NOX_TRK("editor.prop.glyph.direction.label", "Facing"),
                                             NOX_TRK("editor.prop.glyph.direction.help", "0-255: 0 east, 64 south, 128 west, 192 north."), &xf::Glyph::direction,
                                             0, 255));
            } else if constexpr (std::is_same_v<T, xf::Sentry>) {
                out.push_back(flt<xf::Sentry>("sentry.angle", grp::kSentry, NOX_TRK("editor.prop.sentry.angle.label", "Beam angle"),
                                              NOX_TRK("editor.prop.sentry.angle.help", "Where the beam points at the start."), &xf::Sentry::basePosition, -100, 100));
                out.back().kind = Kind::Angle;
                out.push_back(flt<xf::Sentry>("sentry.speed", grp::kSentry, NOX_TRK("editor.prop.sentry.speed.label", "Turn speed"),
                                              NOX_TRK("editor.prop.sentry.speed.help", "Radians a frame (0.0105 is a turn every 20 seconds)."),
                                              &xf::Sentry::rotateSpeed, -1, 1));
                if (v > 0x3C) {
                    FieldDesc c = flt<xf::Sentry>("sentry.current", grp::kSentry, NOX_TRK("editor.prop.sentry.current.label", "Angle now"),
                                                  NOX_TRK("editor.prop.sentry.current.help", "Running state: the beam's angle when saved."),
                                                  &xf::Sentry::currentPosition, -100, 100);
                    c.kind = Kind::Angle;
                    out.push_back(adv(c));
                }
            } else if constexpr (std::is_same_v<T, xf::SpellReward>) {
                out.push_back(str<xf::SpellReward>("book.spell", grp::kSpellBook, NOX_TRK("editor.prop.book.spell.label", "Spell"), Kind::Spell,
                                                   NOX_TRK("editor.prop.book.spell.help", "The spell it teaches."), &xf::SpellReward::spell, 255));
            } else if constexpr (std::is_same_v<T, xf::AbilityReward>) {
                out.push_back(str<xf::AbilityReward>("book.ability", grp::kAbilityBook, NOX_TRK("editor.prop.book.ability.label", "Ability"), Kind::Ability,
                                                     NOX_TRK("editor.prop.book.ability.help", "The ability it teaches."), &xf::AbilityReward::ability, 255));
            } else if constexpr (std::is_same_v<T, xf::FieldGuide>) {
                out.push_back(str<xf::FieldGuide>("book.creature", grp::kFieldGuide, NOX_TRK("editor.prop.book.creature.label", "Creature"), Kind::Creature,
                                                  NOX_TRK("editor.prop.book.creature.help", "The creature it describes."), &xf::FieldGuide::creature, 63));
            } else if constexpr (std::is_same_v<T, xf::SpellPagePedestal>) {
                out.push_back(on<xf::SpellPagePedestal>(desc("pedestal.spell", grp::kPedestal, NOX_TRK("editor.prop.pedestal.spell.label", "Spell"), Kind::Spell,
                                                             NOX_TRK("editor.prop.pedestal.spell.help", "The spell it awards (by id).")),
                                                        [](const xf::SpellPagePedestal& t) { return sv(std::string(things::spellName(t.spell))); },
                                                        [](xf::SpellPagePedestal& t, const FieldValue& r) { t.spell = things::spellId(r.s).value_or(0); }));
            } else if constexpr (std::is_same_v<T, xf::Weapon> || std::is_same_v<T, xf::Armor> || std::is_same_v<T, xf::Ammo> ||
                                 std::is_same_v<T, xf::Team>) {
                static const Key kSlots[4] = {NOX_TRK("editor.prop.item.mod.effectiveness", "Effectiveness"), NOX_TRK("editor.prop.item.mod.material", "Material"),
                                              NOX_TRK("editor.prop.item.mod.enchantment1", "Enchantment 1"),
                                              NOX_TRK("editor.prop.item.mod.enchantment2", "Enchantment 2")};
                for (int k = 0; k < 4; ++k) {
                    const std::size_t at = static_cast<std::size_t>(k);
                    FieldDesc d = on<T>(desc("item.mod" + std::to_string(k), grp::kItem, kSlots[k], Kind::Modifier,
                                             NOX_TRK("editor.prop.item.mod.help",
                                                     "A modifier from modifier.bin that may go on this item (its ALLOWED_WEAPONS / ALLOWED_ARMOR); empty: none.")),
                                        [at](const T& t) { return sv(t.modifiers[at]); }, [at](T& t, const FieldValue& r) { t.modifiers[at] = r.s; });
                    d.modifierSlot = k;
                    out.push_back(std::move(d));
                }
                constexpr Key kDurability = NOX_TRK("editor.prop.item.durability.label", "Durability");
                constexpr Key kDurabilityHelp = NOX_TRK("editor.prop.item.durability.help", "Its health (at most its most; a network game takes modifier.bin's).");
                constexpr Key kItemFlags = NOX_TRK("editor.prop.item.flags.label", "Item flags");
                const auto outfit = [] {
                    return std::vector<NamedValue>{{1, NOX_TRK("editor.prop.item.flags.outfit", "Outfit item"),
                                                    NOX_TRK("editor.prop.item.flags.outfit_help", "A respawn outfit's (sells for 1 in a quest's shops).")}};
                };
                if constexpr (std::is_same_v<T, xf::Weapon>) {
                    if (v > 0x29) out.push_back(num<T>("item.durability", grp::kItem, kDurability, kDurabilityHelp, &T::health, 0, 65535));
                    if (x.charges) {
                        out.push_back(on<T>(desc("item.charges", grp::kItem, NOX_TRK("editor.prop.item.charges.label", "Charges"), Kind::Int,
                                                 NOX_TRK("editor.prop.item.charges.help", "Charges now."), 0, 255),
                                            [](const T& t) { return iv(t.charges ? t.charges->current : 0); },
                                            [](T& t, const FieldValue& r) {
                                                if (t.charges) t.charges->current = clampTo<std::uint8_t>(r.i, 0, 255);
                                            }));
                        out.push_back(on<T>(desc("item.chargesMax", grp::kItem, NOX_TRK("editor.prop.item.charges_max.label", "Most charges"), Kind::Int,
                                                 NOX_TRK("editor.prop.item.charges_max.help", "The most it holds."), 0, 255),
                                            [](const T& t) { return iv(t.charges ? t.charges->max : 0); },
                                            [](T& t, const FieldValue& r) {
                                                if (t.charges) t.charges->max = clampTo<std::uint8_t>(r.i, 0, 255);
                                            }));
                        if (v > 0x3C) {
                            out.push_back(on<T>(desc("item.chargePercent", grp::kItem, NOX_TRK("editor.prop.item.charge_percent.label", "Recharge percent"),
                                                     Kind::Int, NOX_TRK("editor.prop.item.charge_percent.help", "The charge percent (0-100)."), 0, 100),
                                                [](const T& t) { return iv(t.charges ? t.charges->unknown : 0); },
                                                [](T& t, const FieldValue& r) {
                                                    if (t.charges) t.charges->unknown = clampTo<std::uint32_t>(r.i, 0, 100);
                                                }));
                        }
                    }
                    if (v > 0x3F) out.push_back(flags<T>("item.flags", grp::kItem, kItemFlags, kNoText, &T::unknown2, outfit()));
                } else if constexpr (std::is_same_v<T, xf::Armor>) {
                    if (v > 0x28) out.push_back(num<T>("item.durability", grp::kItem, kDurability, kDurabilityHelp, &T::health, 0, 65535));
                    if (v > 0x3D) out.push_back(flags<T>("item.flags", grp::kItem, kItemFlags, kNoText, &T::unknown2, outfit()));
                } else if constexpr (std::is_same_v<T, xf::Ammo>) {
                    out.push_back(num<T>("item.charges", grp::kItem, NOX_TRK("editor.prop.item.count.label", "Count"),
                                         NOX_TRK("editor.prop.item.count.help", "How many it holds."), &T::current, 0, 255));
                    out.push_back(num<T>("item.chargesMax", grp::kItem, NOX_TRK("editor.prop.item.most.label", "Most"),
                                         NOX_TRK("editor.prop.item.charges_max.help", "The most it holds."), &T::limit, 0, 255));
                }
            } else if constexpr (std::is_same_v<T, xf::Gold>) {
                out.push_back(num<xf::Gold>("gold.amount", grp::kGold, NOX_TRK("editor.prop.gold.amount.label", "Amount"),
                                            NOX_TRK("editor.prop.gold.amount.help",
                                                    "How much gold it holds. 0: the game picks an amount as the map starts (15-30, a little more "
                                                    "for experienced players)."),
                                            &xf::Gold::amount, 0, 0x7FFFFFFF));
            } else if constexpr (std::is_same_v<T, xf::Obelisk>) {
                out.push_back(num<xf::Obelisk>("obelisk.mana", grp::kObelisk, NOX_TRK("editor.prop.obelisk.mana.label", "Mana"),
                                               NOX_TRK("editor.prop.obelisk.mana.help", "The mana it holds (the old editor capped it at 255; the field holds more)."),
                                               &xf::Obelisk::manaPool, 0, 0x7FFFFFFF));
                out.push_back(num<xf::Obelisk>("obelisk.known", grp::kObelisk, NOX_TRK("editor.prop.obelisk.known.label", "Seen on the minimap"),
                                               NOX_TRK("editor.prop.obelisk.known.help", "Shown on the minimap from the start."), &xf::Obelisk::minimapKnown, 0, 1));
                out.back().kind = Kind::Bool;
            } else if constexpr (std::is_same_v<T, xf::ToxicCloud>) {
                out.push_back(num<xf::ToxicCloud>("cloud.lifetime", grp::kCloud, NOX_TRK("editor.prop.cloud.lifetime.label", "Frames left"),
                                                  NOX_TRK("editor.prop.cloud.lifetime.help", "How long the cloud lasts (frames)."), &xf::ToxicCloud::lifetime, 0,
                                                  0x7FFFFFFF));
            } else if constexpr (std::is_same_v<T, xf::RewardMarker>) {
                rewardFields(out, v);
            } else if constexpr (std::is_same_v<T, xf::Monster>) {
                creatureFields<xf::Monster>(out, v, false);
                monsterOnly(out, o, v);
            } else if constexpr (std::is_same_v<T, xf::Npc>) {
                creatureFields<xf::Npc>(out, v, true);
                npcOnly(out);
            } else if constexpr (std::is_same_v<T, xf::MonsterGenerator>) {
                generatorFields(out, v);
            }
        },
        o.data);
    // A treasure item or gem with no gold record: said so where the gold's Amount would be (only
    // Gold, QuestGoldPile and QuestGoldChest hold an amount). The note is shown as the field's
    // text (the map's 8-bit text, as a Text field holds).
    if (!std::holds_alternative<xf::Gold>(o.data)) {
        const bool gem = o.type == "Diamond" || o.type == "Emerald" || o.type == "Ruby";
        if ((classes & things::ObjectClass::Treasure) || gem) {
            const things::ThingDef* t = ctx.things ? ctx.things->findThing(o.type) : nullptr;
            const auto worth = t ? t->attribute("WORTH") : std::nullopt;
            const std::string text =
                toMapText(worth ? nox::l10n::format(NOX_TR("editor.prop.treasure.gold.worth", "None: a shop pays {gold} gold for it"), {{"gold", *worth}})
                                : NOX_TR("editor.prop.treasure.gold.nothing", "None: worth nothing, a plain item to pick up"));
            FieldDesc d = desc("treasure.gold", grp::kGold, NOX_TRK("editor.prop.treasure.gold.label", "Gold"), Kind::Text,
                               NOX_TRK("editor.prop.treasure.gold.help",
                                       "This item holds no gold. Gold (the bag), QuestGoldPile and QuestGoldChest hold an amount: place one of "
                                       "those and set its Amount."));
            d.readOnly = true;
            d.get = [text](const DocObject&) {
                FieldValue r;
                r.s = text;
                return r;
            };
            d.set = [](DocObject&, const FieldValue&) {};
            out.push_back(std::move(d));
        }
    }
    return out;
}

std::vector<SharedField> sharedFields(const MapDocument& doc, const std::vector<ObjId>& ids, const FieldContext& ctx)
{
    std::vector<SharedField> out;
    if (ids.empty()) return out;
    const auto first = doc.objects.find(ids.front());
    if (first == doc.objects.end()) return out;
    const auto fields = objectFields(first->second, ctx);
    std::vector<std::vector<FieldDesc>> others;
    for (std::size_t i = 1; i < ids.size(); ++i) {
        const auto it = doc.objects.find(ids[i]);
        if (it == doc.objects.end()) continue;
        others.push_back(objectFields(it->second, ctx));
    }
    for (const auto& f : fields) {
        SharedField s;
        s.desc = f;
        s.value = f.get(first->second);
        bool everywhere = true;
        std::size_t k = 0;
        for (std::size_t i = 1; i < ids.size(); ++i) {
            const auto it = doc.objects.find(ids[i]);
            if (it == doc.objects.end()) continue;
            const auto& list = others[k++];
            const auto m = std::ranges::find(list, f.id, &FieldDesc::id);
            if (m == list.end()) {
                everywhere = false;
                break;
            }
            const FieldValue v = m->get(it->second);
            if (!(v == s.value)) {
                s.mixed = true;
                if (f.kind == Kind::Flags) s.mixedBits |= static_cast<std::uint64_t>(v.i ^ s.value.i);
            }
        }
        if (everywhere) out.push_back(std::move(s));
    }
    return out;
}

Op setFieldOp(const MapDocument& doc, const std::vector<ObjId>& ids, const std::string& fieldId, const FieldValue& value, const FieldContext& ctx,
              std::uint64_t mask)
{
    std::vector<std::pair<ObjId, DocObject>> changed;
    std::string label;
    for (const ObjId id : ids) {
        const auto it = doc.objects.find(id);
        if (it == doc.objects.end()) continue;
        const auto fields = objectFields(it->second, ctx);
        const auto f = std::ranges::find(fields, fieldId, &FieldDesc::id);
        if (f == fields.end() || f->readOnly || f->special != FieldDesc::Special::None) continue;
        DocObject c = it->second;
        FieldValue v = value;
        if (f->kind == Kind::Flags) {
            const FieldValue now = f->get(c);
            v.i = static_cast<std::int64_t>((static_cast<std::uint64_t>(now.i) & ~mask) | (static_cast<std::uint64_t>(value.i) & mask));
        }
        f->set(c, v);
        if (fieldId == "glyph.target") {
            // Facing the target as the game turns one (the direction of the vector: atan2 scaled to
            // 256 and rounded, the FPU's truncation after +0.5).
            if (auto* g = std::get_if<xf::Glyph>(&c.data)) {
                const double a = std::atan2(g->targetY - c.header.y, g->targetX - c.header.x);
                g->direction = static_cast<std::uint8_t>(static_cast<std::int64_t>((a + kTwoPi) * 256.0 / kTwoPi + 0.5) & 0xFF);
            }
        }
        // (An op's label is English: it travels in the op's bytes to the session's other editors.)
        if (label.empty()) {
            label = ids.size() > 1 ? opLabel(oplabels::kSetFieldOn, {{"field", f->label.en}, {"n", ids.size()}})
                                   : opLabel(oplabels::kSetFieldOf, {{"field", f->label.en}, {"type", c.type}});
        }
        changed.emplace_back(id, std::move(c));
    }
    return editObjectsOp(doc, changed, label);
}

} // namespace nox::mapedit
