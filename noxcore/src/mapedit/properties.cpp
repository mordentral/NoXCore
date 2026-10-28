// The map editor's property tables (properties.hpp).
//
// Each field's meaning is the game's: the xfer reader that stores it and the code that uses it,
// named in the help text by what it does (our server port follows the same readers: world.cpp's
// readObject, objects.cpp, monsters.cpp, rewards.cpp, the client's light drawing).
#include "noxcore/mapedit/properties.hpp"

#include "doc_types.hpp"

#include "noxcore/map/objects.hpp"
#include "noxcore/mapedit/object_edit.hpp"
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

namespace nox::mapedit {

namespace {

namespace xf = map::xfer;
using Kind = FieldKind;

constexpr double kTwoPi = 2 * std::numbers::pi;

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

FieldDesc desc(std::string id, std::string group, std::string label, Kind kind, std::string help, double lo = 0, double hi = 0)
{
    FieldDesc d;
    d.id = std::move(id);
    d.group = std::move(group);
    d.label = std::move(label);
    d.kind = kind;
    d.help = std::move(help);
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
FieldDesc num(std::string id, std::string group, std::string label, std::string help, M T::*m, double lo, double hi)
{
    return on<T>(desc(std::move(id), std::move(group), std::move(label), Kind::Int, std::move(help), lo, hi),
                 [m](const T& t) { return iv(static_cast<std::int64_t>(t.*m)); },
                 [m, lo, hi](T& t, const FieldValue& v) { t.*m = clampTo<M>(v.i, lo, hi); });
}

/// A 16-bit word the game reads signed (sign-extended where it's used), shown and set as
/// -32768..32767 and stored as the same word: a map's bytes round-trip unchanged.
template <class T>
FieldDesc snum16(std::string id, std::string group, std::string label, std::string help, std::uint16_t T::*m)
{
    return on<T>(desc(std::move(id), std::move(group), std::move(label), Kind::Int, std::move(help), -32768, 32767),
                 [m](const T& t) { return iv(static_cast<std::int16_t>(t.*m)); },
                 [m](T& t, const FieldValue& v) { t.*m = static_cast<std::uint16_t>(clampTo<std::int16_t>(v.i, -32768, 32767)); });
}

/// A float member.
template <class T>
FieldDesc flt(std::string id, std::string group, std::string label, std::string help, float T::*m, double lo, double hi)
{
    return on<T>(desc(std::move(id), std::move(group), std::move(label), Kind::Float, std::move(help), lo, hi),
                 [m](const T& t) { return fv(t.*m); },
                 [m, lo, hi](T& t, const FieldValue& v) { t.*m = static_cast<float>(std::clamp(v.f, lo, hi)); });
}

/// A float kept as its bits in a u32 member.
template <class T>
FieldDesc fbits(std::string id, std::string group, std::string label, std::string help, std::uint32_t T::*m, double lo, double hi)
{
    return on<T>(desc(std::move(id), std::move(group), std::move(label), Kind::Float, std::move(help), lo, hi),
                 [m](const T& t) { return fv(bitsFloat(t.*m)); },
                 [m, lo, hi](T& t, const FieldValue& v) { t.*m = floatBits(std::clamp(v.f, lo, hi)); });
}

std::vector<NamedValue> named(const std::vector<FlagBit>& bits)
{
    std::vector<NamedValue> out;
    for (const auto& b : bits) out.push_back({b.bit, b.name, b.help});
    return out;
}

/// A flags member (unknown bits kept: the panel sets bits under a mask).
template <class T, class M>
FieldDesc flags(std::string id, std::string group, std::string label, std::string help, M T::*m, std::vector<NamedValue> names)
{
    FieldDesc d = on<T>(desc(std::move(id), std::move(group), std::move(label), Kind::Flags, std::move(help), 0,
                             static_cast<double>(std::numeric_limits<M>::max())),
                        [m](const T& t) { return iv(static_cast<std::int64_t>(t.*m)); },
                        [m](T& t, const FieldValue& v) { t.*m = static_cast<M>(v.i); });
    d.names = std::move(names);
    return d;
}

/// An enumerated member.
template <class T, class M>
FieldDesc enumf(std::string id, std::string group, std::string label, std::string help, M T::*m, std::vector<NamedValue> names)
{
    FieldDesc d = on<T>(desc(std::move(id), std::move(group), std::move(label), Kind::Enum, std::move(help), 0,
                             static_cast<double>(std::numeric_limits<M>::max())),
                        [m](const T& t) { return iv(static_cast<std::int64_t>(t.*m)); },
                        [m](T& t, const FieldValue& v) { t.*m = static_cast<M>(v.i); });
    d.names = std::move(names);
    return d;
}

/// A string member of some kind (Text, Spell, MapName...), at most `maxBytes`.
template <class T>
FieldDesc str(std::string id, std::string group, std::string label, Kind kind, std::string help, std::string T::*m, std::size_t maxBytes)
{
    return on<T>(desc(std::move(id), std::move(group), std::move(label), kind, std::move(help), 0, static_cast<double>(maxBytes)),
                 [m](const T& t) { return sv(t.*m); },
                 [m, maxBytes](T& t, const FieldValue& v) { t.*m = v.s.substr(0, maxBytes); });
}

/// A script handler member (the function's name; its stored flags word kept).
template <class T, class H>
FieldDesc handler(std::string id, std::string group, std::string label, std::string help, H get)
{
    return on<T>(desc(std::move(id), std::move(group), std::move(label), Kind::Function, std::move(help), 0, 1023),
                 [get](const T& t) { return sv(get(const_cast<T&>(t)).function); },
                 [get](T& t, const FieldValue& v) { get(t).function = v.s.substr(0, 1023); });
}

template <class T>
FieldDesc color(std::string id, std::string group, std::string label, std::string help, std::array<std::uint8_t, 3> T::*m)
{
    return on<T>(desc(std::move(id), std::move(group), std::move(label), Kind::Color, std::move(help)),
                 [m](const T& t) { return rgbv((t.*m)[0], (t.*m)[1], (t.*m)[2]); }, [m](T& t, const FieldValue& v) { t.*m = v.rgb; });
}

FieldDesc& adv(FieldDesc& d)
{
    d.advanced = true;
    return d;
}

std::vector<NamedValue> range(std::initializer_list<const char*> names, std::int64_t first = 0)
{
    std::vector<NamedValue> out;
    std::int64_t v = first;
    for (const char* n : names) out.push_back({v++, n, {}});
    return out;
}

// --- Tables of names -------------------------------------------------------------------------------

/// The object CLASS bits by name (thing.bin's names).
const std::vector<FlagBit>& classBits()
{
    namespace oc = things::ObjectClass;
    static const std::vector<FlagBit> bits = {
        {oc::Missile, "MISSILE", ""},       {oc::Monster, "MONSTER", "Creatures and NPCs."},
        {oc::Player, "PLAYER", ""},         {oc::Obstacle, "OBSTACLE", "Pushable obstacles."},
        {oc::Food, "FOOD", ""},             {oc::Exit, "EXIT", ""},
        {oc::Key, "KEY", ""},               {oc::Door, "DOOR", ""},
        {oc::InfoBook, "INFO_BOOK", ""},    {oc::Trigger, "TRIGGER", ""},
        {oc::Transporter, "TRANSPORTER", ""}, {oc::Hole, "HOLE", ""},
        {oc::Wand, "WAND", ""},             {oc::Fire, "FIRE", ""},
        {oc::Elevator, "ELEVATOR", ""},     {oc::ElevatorShaft, "ELEVATOR_SHAFT", ""},
        {oc::Dangerous, "DANGEROUS", ""},   {oc::MonsterGenerator, "MONSTERGENERATOR", ""},
        {oc::Readable, "READABLE", ""},     {oc::Light, "LIGHT", ""},
        {oc::Simple, "SIMPLE", ""},         {oc::Complex, "COMPLEX", ""},
        {oc::Immobile, "IMMOBILE", ""},     {oc::VisibleEnable, "VISIBLE_ENABLE", ""},
        {oc::Weapon, "WEAPON", ""},         {oc::Armor, "ARMOR", ""},
        {oc::NotStackable, "NOT_STACKABLE", ""}, {oc::Treasure, "TREASURE", ""},
        {oc::Flag, "FLAG", ""},             {oc::ClientPersist, "CLIENT_PERSIST", ""},
        {oc::ClientPredict, "CLIENT_PREDICT", ""}, {oc::Pickup, "PICKUP", "Anything that can be picked up."},
    };
    return bits;
}

std::vector<NamedValue> teamValues()
{
    std::vector<NamedValue> out;
    for (const auto& t : teamNames()) out.push_back({t.team, t.name, {}});
    return out;
}

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
        FieldDesc d = desc("header.type", "Header", "Type", Kind::Thing,
                           "The thing.bin type. Changing it gives the object the new type's fresh data (what the game gives a new one: "
                           "monster.bin's numbers, a plate's colours...), keeping its place, extent and header.");
        d.special = FieldDesc::Special::Type;
        d.get = [](const DocObject& x) { return sv(x.type); };
        d.set = [](DocObject&, const FieldValue&) {}; // (changeTypeOp)
        out.push_back(std::move(d));
    }
    {
        FieldDesc d = desc("header.position", "Header", "Position", Kind::Point, "Where it stands, in world units (23 a wall cell).", 0,
                           kWorldSize - 1);
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
        FieldDesc d = desc("header.extent", "Header", "Extent", Kind::Int,
                           "The map's number for it: links (teleporters, elevators, movers) and object groups name it. Changing it "
                           "rewrites every link and group member that named it. Must be unique.",
                           1, 0x7FFFFFFF);
        d.special = FieldDesc::Special::Extent;
        d.get = [](const DocObject& x) { return iv(x.header.extent); };
        d.set = [](DocObject&, const FieldValue&) {}; // (setExtentOp)
        out.push_back(std::move(d));
    }
    {
        FieldDesc d = desc("header.details", "Header", "Header details", Kind::Bool,
                           "Whether the record stores the header's details (the old editor's \"Extra Bytes\"): script name, team, flags, "
                           "handler, inventory. Without them the object takes its type's flags.");
        d.special = FieldDesc::Special::Details;
        d.get = [](const DocObject& x) { return iv(x.header.details ? 1 : 0); };
        d.set = [](DocObject&, const FieldValue&) {}; // (setDetailsOp)
        out.push_back(std::move(d));
    }
    {
        FieldDesc d = desc("header.scriptId", "Header", "Script id", Kind::Int,
                           "The script's handle for it. 0: the game gives one when the map loads (every map the editor writes has 0 for "
                           "new objects). The game rewrites it.",
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
    out.push_back(details(desc("header.scriptName", "Header", "Script name", Kind::Text,
                               "The name scripts find it by (Object(\"name\")); the game prefixes it with the map's name. At most 255 bytes.", 0, 255),
                          [](const map::ObjectHeader::Details& d) { return sv(d.scriptName); },
                          [](map::ObjectHeader::Details& d, const FieldValue& v) { d.scriptName = v.s.substr(0, 255); }));
    {
        FieldDesc d = details(desc("header.team", "Header", "Team", Kind::Enum,
                                   "Its team: a team's players can't hurt or set off what's on their team; team-only items. 0 none.", 0, 255),
                              [](const map::ObjectHeader::Details& x) { return iv(x.team); },
                              [](map::ObjectHeader::Details& x, const FieldValue& v) { x.team = clampTo<std::uint8_t>(v.i, 0, 255); });
        d.names = teamValues();
        out.push_back(std::move(d));
    }
    {
        FieldDesc d = details(desc("header.flags", "Header", "Create flags", Kind::Flags,
                                   "The object's flags. The game's save writes only the authoring bits (mask 0x11408162); on load the stored "
                                   "value is ORed in whole, so a runtime-only bit stored in a map is set too. Unknown bits are kept.",
                                   0, 0xFFFFFFFF),
                              [](const map::ObjectHeader::Details& x) { return iv(x.flags); },
                              [](map::ObjectHeader::Details& x, const FieldValue& v) { x.flags = static_cast<std::uint32_t>(v.i); });
        d.names = named(objectFlagBits());
        out.push_back(std::move(d));
    }
    {
        FieldDesc d = details(desc("header.xStatus", "Header", "Status (xStatus)", Kind::Flags,
                                   "The object's status bits (the old editor's \"AnimFlags\"). The save writes bits 0x5E; the load ORs the "
                                   "stored value in.",
                                   0, 0xFFFFFFFF),
                              [](const map::ObjectHeader::Details& x) { return iv(x.statusFlags); },
                              [](map::ObjectHeader::Details& x, const FieldValue& v) { x.statusFlags = static_cast<std::uint32_t>(v.i); });
        d.names = named(xStatusBits());
        out.push_back(std::move(d));
    }
    if (o.header.details->onPickup) {
        out.push_back(details(desc("header.onPickup", "Header", "Pickup handler", Kind::Function,
                                   "A script function called when a player picks it up (header version 0x3F on).", 0, 1023),
                              [](const map::ObjectHeader::Details& x) { return sv(x.onPickup ? x.onPickup->function : std::string()); },
                              [](map::ObjectHeader::Details& x, const FieldValue& v) {
                                  if (x.onPickup) x.onPickup->function = v.s.substr(0, 1023);
                              }));
    }
    if (o.header.details->decayFrames) {
        out.push_back(details(desc("header.decay", "Header", "Decay frames", Kind::Int,
                                   "Frames left before a TRANSIENT object decays (header version 0x40); 0 or less: none. (The old editor's "
                                   "0xFFFFFFFB default is just -5.)",
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
    const std::vector<NamedValue> ways = {{0, "0", ""}, {8, "8 (a quarter turn)", ""}, {16, "16 (a half turn)", ""}, {24, "24 (three quarters)", ""}};
    out.push_back(enumf<xf::Door>("door.lock", "Door", "Lock", "The key it needs (Silver, Gold, Ruby, Sapphire); a mechanism lock opens "
                                                              "only by script (UnlockDoor).",
                                  &xf::Door::lock, range({"None", "Silver key", "Gold key", "Ruby key", "Sapphire key", "Mechanism (script only)"})));
    if (v >= 0x29) {
        // Both directions together, as every stock door has them (the shut way the wall joins by).
        FieldDesc d = on<xf::Door>(desc("door.direction", "Door", "Hangs", Kind::Enum,
                                        "Which way the door hangs when shut (0..31, 8 a quarter turn: every stock door uses 0, 8, 16 or 24). Sets "
                                        "the shut direction and the current swing together.",
                                        0, 31),
                                   [](const xf::Door& t) { return iv(t.direction); },
                                   [](xf::Door& t, const FieldValue& x) { t.direction = t.currentDirection = static_cast<std::uint32_t>(x.i & 31); });
        d.names = ways;
        out.push_back(std::move(d));
    }
    FieldDesc cur = num<xf::Door>("door.current", "Door", "Current swing", "The door's swing now (0..31); equal to the shut direction for a shut door.",
                                  &xf::Door::currentDirection, 0, 31);
    out.push_back(adv(cur));
}

void triggerFields(std::vector<FieldDesc>& out, std::uint16_t v, bool plate)
{
    const char* g = plate ? "Pressure plate" : "Trigger";
    out.push_back(num<xf::Trigger>("trigger.sizeX", g, "Width", "The box's side along one diagonal (world units; the game takes at most 60).",
                                   &xf::Trigger::sizeX, 1, 60));
    out.push_back(num<xf::Trigger>("trigger.sizeY", g, "Height", "The box's other side (at most 60).", &xf::Trigger::sizeY, 1, 60));
    out.push_back(color<xf::Trigger>("trigger.edge", g, "Outline colour",
                                     "The plate's outline, drawn first (PressurePlateDraw draws the diamond twice, never filled). All six colour "
                                     "bytes 0: the plate isn't drawn.",
                                     &xf::Trigger::edgeColor));
    out.push_back(color<xf::Trigger>("trigger.back", g, "Shadow colour", "The plate's second outline, one pixel to the right (its shadow).",
                                     &xf::Trigger::backColor));
    out.push_back(handler<xf::Trigger>("trigger.onPressed", g, "Pressed", "Called when it's pressed.",
                                       [](xf::Trigger& t) -> map::ScriptHandler& { return t.onPressed; }));
    out.push_back(handler<xf::Trigger>("trigger.onReleased", g, "Released", "Called when it's let go (once what pressed it has left).",
                                       [](xf::Trigger& t) -> map::ScriptHandler& { return t.onReleased; }));
    out.push_back(handler<xf::Trigger>("trigger.onCollided", g, "Touched (filter)",
                                       "Called on each touch; when it returns 0 the touch doesn't count as a press (a filter).",
                                       [](xf::Trigger& t) -> map::ScriptHandler& { return t.onCollided; }));
    out.push_back(flags<xf::Trigger>("trigger.allowed", g, "Pressed by (classes)",
                                     "What may press it: none ticked = anything with mass; else only objects with one of these classes.",
                                     &xf::Trigger::allowedClasses, named(classBits())));
    out.push_back(flags<xf::Trigger>("trigger.ignored", g, "Ignores (classes)", "What never presses it: objects with any of these classes.",
                                     &xf::Trigger::ignoredClasses, named(classBits())));
    {
        FieldDesc d = enumf<xf::Trigger>("trigger.allowedTeam", g, "Pressed by team", "0: any team; else only that team.", &xf::Trigger::allowedTeam, teamValues());
        out.push_back(std::move(d));
        FieldDesc e = enumf<xf::Trigger>("trigger.ignoredTeam", g, "Ignores team", "0: none; else that team never presses it.", &xf::Trigger::ignoredTeam, teamValues());
        out.push_back(std::move(e));
    }
    {
        const std::vector<NamedValue> bits = {
            {1, "Pressed (runtime)", "Set while something presses it this frame."},
            {2, "One shot", "Once released it stays used up (state 5) until it is disabled and enabled again."},
            {4, "Pressed last frame (runtime)", ""},
            {8, "Initialised (runtime)", "Cleared while it's disabled, so it re-arms when enabled."},
        };
        out.push_back(flags<xf::Trigger>("trigger.flags", g, "Trigger flags", "Only One shot is a design setting; the others are its running state.",
                                         &xf::Trigger::unknown1, bits));
    }
    if (v > 0x3C) {
        FieldDesc s = enumf<xf::Trigger>("trigger.state", g, "State", "Its state: up, pressed, or used up (a one-shot after its release).", &xf::Trigger::state,
                                         {{0, "Up", ""}, {1, "Pressed", ""}, {5, "Used up", "Ignores every touch."}});
        out.push_back(adv(s));
        FieldDesc a = num<xf::Trigger>("trigger.anim", g, "Drawn down", "Its drawing: 0 up, 1 down.", &xf::Trigger::animFrame, 0, 1);
        out.push_back(adv(a));
        FieldDesc u = num<xf::Trigger>("trigger.unknown2", g, "Byte after the state", "Stored and kept; the trigger code never reads it.",
                                       &xf::Trigger::unknown2, 0, 255);
        out.push_back(adv(u));
    }
}

void lightFields(std::vector<FieldDesc>& out)
{
    const char* g = "Light";
    out.push_back(enumf<xf::InvisibleLight>("light.type", g, "Type", "0 none, 1 white, 2 coloured.", &xf::InvisibleLight::type,
                                            range({"None", "White", "Coloured"})));
    {
        // The intensity sets the radius as the game does (lightRadiusFromIntensity), and the 16.16 copy.
        FieldDesc d = on<xf::InvisibleLight>(
            desc("light.intensity", g, "Intensity", Kind::Float,
                 "How bright (0..63); it sets the light's reach (the radius is worked out from it as the game does). Negative: an "
                 "anti-light (darkness).",
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
    FieldDesc r = num<xf::InvisibleLight>("light.radius", g, "Radius", "The reach as stored (world units); the intensity sets it.", &xf::InvisibleLight::radius, 0, 2000);
    out.push_back(adv(r));
    out.push_back(on<xf::InvisibleLight>(desc("light.color", g, "Colour", Kind::Color, "The light's colour."),
                                         [](const xf::InvisibleLight& t) {
                                             return rgbv(clampTo<std::uint8_t>(t.color[0], 0, 255), clampTo<std::uint8_t>(t.color[1], 0, 255),
                                                         clampTo<std::uint8_t>(t.color[2], 0, 255));
                                         },
                                         [](xf::InvisibleLight& t, const FieldValue& x) {
                                             for (int i = 0; i < 3; ++i) t.color[static_cast<std::size_t>(i)] = x.rgb[static_cast<std::size_t>(i)];
                                         }));
    out.push_back(on<xf::InvisibleLight>(desc("light.anti", g, "Anti-light", Kind::Bool, "It darkens instead (the client takes the colour inverted). Not for a cone: the game's cone always lights."),
                                         [](const xf::InvisibleLight& t) { return iv(t.isAnti != 0); },
                                         [](xf::InvisibleLight& t, const FieldValue& x) { t.isAnti = x.i ? 1 : 0; }));
    {
        const std::vector<NamedValue> bits = {
            {1, "Colour loops", "The colour sequence jumps back to its first key (else it runs back down)."},
            {4, "Intensity loops", "The intensity keys loop (else they run back down: pulsing)."},
            {0x10, "Cone loops", "The cone width keys loop."},
            {0x40, "Aims at the target", "The cone points at the target object (an extent); an all-round light becomes a cone. Needs a key (below)."},
            {0x80, "Rotates", "The cone turns (not an all-round light). It turns, aims or changes width only if the light has at least one "
                              "colour, intensity or cone key."},
            {0x100, "Rotation sweeps", "Turns from its direction to the most, then jumps back to its direction (else all the way round)."},
        };
        out.push_back(flags<xf::InvisibleLight>("light.anim", g, "Animation", "What animates and how.", &xf::InvisibleLight::animFlags, bits));
    }
    out.push_back(num<xf::InvisibleLight>("light.colorStep", g, "Colour step", "Frames from one colour key to the next (30 a second).",
                                          &xf::InvisibleLight::colorStep, 0, 65535));
    out.push_back(num<xf::InvisibleLight>("light.intensityStep", g, "Intensity step", "Frames from one intensity key to the next.",
                                          &xf::InvisibleLight::intensityStep, 0, 65535));
    out.push_back(num<xf::InvisibleLight>("light.coneStep", g, "Cone step", "Frames from one cone key to the next.", &xf::InvisibleLight::coneStep, 0, 65535));
    const auto angle16 = [](const char* id, const char* label, const char* help, std::uint16_t xf::InvisibleLight::*m) {
        FieldDesc d = on<xf::InvisibleLight>(desc(id, "Light cone", label, Kind::Angle, help, 0, kTwoPi),
                                             [m](const xf::InvisibleLight& t) { return fv((t.*m) * kTwoPi / 65536.0); },
                                             [m](xf::InvisibleLight& t, const FieldValue& x) {
                                                 double a = std::fmod(x.f, kTwoPi);
                                                 if (a < 0) a += kTwoPi;
                                                 t.*m = static_cast<std::uint16_t>(std::lround(a * 65536.0 / kTwoPi) & 0xFFFF);
                                             });
        return d;
    };
    out.push_back(angle16("light.direction", "Direction", "Where the cone points.", &xf::InvisibleLight::direction));
    out.push_back(angle16("light.cone", "Cone width", "How far it lights either side of its direction (half the cone); 0, or 180 degrees or more, lights nothing.",
                          &xf::InvisibleLight::cone));
    out.push_back(on<xf::InvisibleLight>(desc("light.omni", "Light cone", "All round", Kind::Bool,
                                              "No cone: light all round (stored 0xFFFF). Turns off the cone's animation and rotation."),
                                         [](const xf::InvisibleLight& t) { return iv(t.omni != 0); },
                                         [](xf::InvisibleLight& t, const FieldValue& x) { t.omni = x.i ? 0xFFFFu : 0u; }));
    {
        FieldDesc d = on<xf::InvisibleLight>(desc("light.target", "Light cone", "Target", Kind::Extent,
                                                  "The object the cone aims at (with \"Aims at the target\"): a static object's extent."),
                                             [](const xf::InvisibleLight& t) { return iv(t.target); },
                                             [](xf::InvisibleLight& t, const FieldValue& x) { t.target = static_cast<std::uint32_t>(x.i); });
        out.push_back(std::move(d));
    }
    // (The game's colorLightRotate sign-extends the speed: a negative one turns the cone the other
    // way. The most, like the direction it starts from, it takes unsigned.)
    out.push_back(snum16<xf::InvisibleLight>("light.rotationSpeed", "Light cone", "Rotation speed",
                                             "Degrees a second, -32768 to 32767: positive turns one way, negative (-30) the other; 0 doesn't turn.",
                                             &xf::InvisibleLight::rotationSpeed));
    out.push_back(num<xf::InvisibleLight>("light.rotationMax", "Light cone", "Rotation most",
                                          "Degrees it sweeps to (with \"Rotation sweeps\"), 0 and up.",
                                          &xf::InvisibleLight::rotationMax, 0, 65535));
    FieldDesc u = num<xf::InvisibleLight>("light.unused", g, "Unused byte", "Saved (0x80 by default), never read.", &xf::InvisibleLight::unused, 0, 255);
    out.push_back(adv(u));
}

template <class T>
void creatureFields(std::vector<FieldDesc>& out, std::uint16_t v, bool npc)
{
    const char* g = npc ? "NPC" : "Monster";
    {
        FieldDesc d = on<T>(desc(std::string(npc ? "npc" : "monster") + ".direction", g, "Facing", Kind::Direction,
                                 "Which way it faces at the start: one of the 8 grid steps."),
                            [](const T& t) { return iv(stepOf(t.direction)); },
                            [](T& t, const FieldValue& x) { t.direction = kSteps[static_cast<std::size_t>(((x.i % 8) + 8) % 8)]; });
        out.push_back(std::move(d));
    }
    const std::string p = npc ? "npc." : "monster.";
    out.push_back(str<T>(p + "action", g, "Starts by", Kind::Action, "The action it starts with (MonsterInit pushes it: guard, escort, roam...).", &T::action, 255));
    out.push_back(flags<T>(p + "status", "Behaviour", "Status", "Its status flags (monster.bin's STATUS at the start).", &T::statusFlags,
                           named(monsterStatusBits())));
    out.push_back(fbits<T>(p + "sight", "Behaviour", "Sight range", "How far it sees (world units).", &T::sightRange, 0, 2000));
    out.push_back(fbits<T>(p + "aggression", "Behaviour", "Aggression", "How readily it attacks: 0 friendly ... 0.83 hostile (the AI's own numbers "
                                                                       "go higher).",
                           &T::aggression, 0, 2));
    out.push_back(fbits<T>(p + "retreat", "Behaviour", "Retreats at", "The health fraction (0-1) at which it retreats.", &T::retreatLevel, 0, 1));
    out.push_back(fbits<T>(p + "resume", "Behaviour", "Fights again at", "The health fraction (0-1) at which it stops retreating.", &T::resumeLevel, 0, 1));
    out.push_back(str<T>(p + "escort", "Behaviour", "Escorts", Kind::Text, "The script name of whom it escorts (with the escort action).", &T::unknown341, 255));
    out.push_back(fbits<T>(p + "escortDistance", "Behaviour", "Escort distance", "How close it keeps to whom it escorts.", &T::unknown329, 0, 2000));
    {
        const std::vector<NamedValue> bits = {{0x01, "Route class 1", ""}, {0x02, "Route class 2", ""}, {0x04, "Route class 3", ""},
                                              {0x08, "Route class 4", ""}, {0x10, "Route class 5", ""}, {0x20, "Route class 6", ""},
                                              {0x40, "Route class 7", ""}, {0x80, "Default routes (0x80)", "The links stock maps give every waypoint."}};
        out.push_back(flags<T>(p + "roam", "Behaviour", "Roams by", "Which waypoint links it roams along: the link flags it matches (SetRoamFlag).",
                               &T::unknown333, bits));
    }
    out.push_back(handler<T>(p + "onSighted", "Script handlers", "Enemy sighted", "", [](T& t) -> map::ScriptHandler& { return t.scripts.enemySighted; }));
    out.push_back(handler<T>(p + "onDeath", "Script handlers", "Death", "", [](T& t) -> map::ScriptHandler& { return t.scripts.death; }));
    out.push_back(handler<T>(p + "onLooking", "Script handlers", "Looking for enemy", "", [](T& t) -> map::ScriptHandler& { return t.scripts.lookingForEnemy; }));
    out.push_back(handler<T>(p + "onFocus", "Script handlers", "Change of focus", "", [](T& t) -> map::ScriptHandler& { return t.scripts.changeFocus; }));
    out.push_back(handler<T>(p + "onHit", "Script handlers", "Is hit", "", [](T& t) -> map::ScriptHandler& { return t.scripts.isHit; }));
    out.push_back(handler<T>(p + "onRetreat", "Script handlers", "Retreat", "", [](T& t) -> map::ScriptHandler& { return t.scripts.retreat; }));
    out.push_back(handler<T>(p + "onCollision", "Script handlers", "Collision", "", [](T& t) -> map::ScriptHandler& { return t.scripts.collision; }));
    out.push_back(handler<T>(p + "onHear", "Script handlers", "Hears enemy", "", [](T& t) -> map::ScriptHandler& { return t.scripts.hearEnemy; }));
    out.push_back(handler<T>(p + "onWaypoint", "Script handlers", "End of waypoint", "", [](T& t) -> map::ScriptHandler& { return t.scripts.endOfWaypoint; }));
    out.push_back(handler<T>(p + "onLost", "Script handlers", "Lost enemy", "", [](T& t) -> map::ScriptHandler& { return t.scripts.lostEnemy; }));
    out.push_back(on<T>(desc(p + "lookDelay", "Script handlers", "Look delay", Kind::Int, "Frames between its looks for an enemy (stored with the handlers).", 0, 65535),
                        [](const T& t) { return iv(t.scripts.unknown305); },
                        [](T& t, const FieldValue& x) { t.scripts.unknown305 = clampTo<std::uint16_t>(x.i, 0, 65535); }));
    static const char* kUse[5] = {"Counter", "On self", "Attack", "Hold ground", "Escape"};
    for (int k = 0; k < 5; ++k) {
        for (int m = 0; m < 2; ++m) {
            const std::size_t at = static_cast<std::size_t>(k * 2 + m);
            out.push_back(on<T>(desc(p + "delay" + std::to_string(at), "Spell delays", std::string(kUse[k]) + (m ? " (most)" : " (least)"), Kind::Int,
                                     "Frames between casts of its " + std::string(kUse[k]) + " spells: the least and the most.", 0, 65535),
                                [at](const T& t) { return iv(t.spellDelays[at]); },
                                [at](T& t, const FieldValue& x) { t.spellDelays[at] = clampTo<std::uint16_t>(x.i, 0, 65535); }));
        }
    }
    out.push_back(num<T>(p + "spellPower", "Spells", "Spell power", "The level its spells are cast at (1-5).", &T::unknown510, 1, 5));
    out.push_back(fbits<T>(p + "aim", "Spells", "Aim", "Missile and spell aim accuracy, 0-1.", &T::unknown330, 0, 1));
    for (int k = 0; k < 3; ++k) {
        const std::size_t at = static_cast<std::size_t>(k);
        out.push_back(on<T>(desc(p + "bomb" + std::to_string(k), "Spells", "Bomber spell " + std::to_string(k + 1), Kind::Spell,
                                 "A bomber's spells (it carries a glyph of them).", 0, 255),
                            [at](const T& t) { return sv(t.defaultSpells[at]); },
                            [at](T& t, const FieldValue& x) { t.defaultSpells[at] = x.s; }));
    }
    out.push_back(fbits<T>(p + "startHealth", "Combat", "Starting health", "Its health at the start as a fraction of its most.", &T::unknown338, 0, 1));
    out.push_back(on<T>(desc(p + "immortal", "Combat", "Immortal", Kind::Bool,
                             "The map's health kept: the host gives it no health at all, so it can't be killed (the story's maidens and captains)."),
                        [](const T& t) { return iv(t.unknown361 != 0); }, [](T& t, const FieldValue& x) { t.unknown361 = x.i ? 1 : 0; }));
    if constexpr (std::is_same_v<T, xf::Monster>) {
        if (v >= 0x40) out.push_back(num<T>(p + "poison", "Combat", "Poisoned", "Its poison level at the start (0 none).", &T::poison, 0, 255));
    } else {
        if (v >= 0x3E) out.push_back(num<T>(p + "poison", "Combat", "Poisoned", "Its poison level at the start (0 none).", &T::poison, 0, 255));
    }
    FieldDesc poly = num<T>(p + "polygon", "Advanced", "Polygon", "The polygon it was in when saved (0xDEADFACE: unknown). The game rewrites it.", &T::unknown0, 0, 0xFFFFFFFF);
    poly.readOnly = true;
    out.push_back(adv(poly));
    FieldDesc sub = flags<T>(p + "subclass", "Advanced", "Subclass bits added", "SUBCLASS bits (0x180) added to its type's.", &T::subclassBits, {});
    out.push_back(adv(sub));
    FieldDesc u1 = num<T>(p + "unknown1", "Advanced", "Word after the handlers", "Read and dropped by the game.", &T::unknown1, 0, 0xFFFFFFFF);
    out.push_back(adv(u1));
}

void monsterOnly(std::vector<FieldDesc>& out, const DocObject& o, std::uint16_t v)
{
    const auto& m = std::get<xf::Monster>(o.data);
    out.push_back(num<xf::Monster>("monster.health", "Combat", "Health", "Its health (monster.bin's for a new one).", &xf::Monster::health, 0, 65535));
    static const char* kFrom[4] = {"Resume level from its type", "Retreat level from its type", "Status from its type", "Spells from its type"};
    for (int k = 0; k < 4; ++k) {
        const std::size_t at = static_cast<std::size_t>(k);
        out.push_back(on<xf::Monster>(desc("monster.fromType" + std::to_string(k), "Defaults", kFrom[k], Kind::Bool,
                                           "Ticked: the game takes this from monster.bin when it spawns, not from the map."),
                                      [at](const xf::Monster& t) { return iv(t.unknown337[at] != 0); },
                                      [at](xf::Monster& t, const FieldValue& x) { t.unknown337[at] = x.i ? 1 : 0; }));
    }
    if (m.colors) {
        static const char* kNames[6] = {"Hair", "Hair 2", "Sideburns", "Beard", "Skin", "Skin 2"};
        for (int k = 0; k < 6; ++k) {
            const std::size_t at = static_cast<std::size_t>(k);
            out.push_back(on<xf::Monster>(desc("monster.color" + std::to_string(k), "Look", std::string(kNames[k]) + " colour", Kind::Color, "A maiden's colour."),
                                          [at](const xf::Monster& t) { return t.colors ? rgbv((*t.colors)[at][0], (*t.colors)[at][1], (*t.colors)[at][2]) : FieldValue{}; },
                                          [at](xf::Monster& t, const FieldValue& x) {
                                              if (t.colors) (*t.colors)[at] = x.rgb;
                                          }));
        }
    }
    if (m.voiceSet) {
        out.push_back(on<xf::Monster>(desc("monster.voice", "Look", "Voice", Kind::Voice, "Its sound set (soundset.bin)."),
                                      [](const xf::Monster& t) { return sv(t.voiceSet ? *t.voiceSet : std::string()); },
                                      [](xf::Monster& t, const FieldValue& x) {
                                          if (t.voiceSet) t.voiceSet = x.s;
                                      }));
    }
    if (m.voiceSet2 && v > 0x3E) {
        out.push_back(on<xf::Monster>(desc("monster.voice2", "Look", "Voice", Kind::Voice, "A wounded NPC's sound set (soundset.bin)."),
                                      [](const xf::Monster& t) { return sv(t.voiceSet2 ? *t.voiceSet2 : std::string()); },
                                      [](xf::Monster& t, const FieldValue& x) {
                                          if (t.voiceSet2) t.voiceSet2 = x.s;
                                      }));
    }
    if (m.shop) {
        out.push_back(on<xf::Monster>(desc("monster.shopBuy", "Shop", "Buys at", Kind::Float, "What it pays, as a fraction of an item's worth.", 0, 10),
                                      [](const xf::Monster& t) { return fv(t.shop ? bitsFloat(t.shop->unknown1) : 0); },
                                      [](xf::Monster& t, const FieldValue& x) {
                                          if (t.shop) t.shop->unknown1 = floatBits(std::clamp(x.f, 0.0, 10.0));
                                      }));
        if (v > 0x3C) {
            out.push_back(on<xf::Monster>(desc("monster.shopSell", "Shop", "Sells at", Kind::Float, "What it asks, as a multiple of an item's worth.", 0, 10),
                                          [](const xf::Monster& t) { return fv(t.shop ? bitsFloat(t.shop->unknown2) : 0); },
                                          [](xf::Monster& t, const FieldValue& x) {
                                              if (t.shop) t.shop->unknown2 = floatBits(std::clamp(x.f, 0.0, 10.0));
                                          }));
        }
        out.push_back(on<xf::Monster>(desc("monster.shopGreeting", "Shop", "Greeting", Kind::StringKey, "Its greeting: a string table key.", 0, 255),
                                      [](const xf::Monster& t) { return sv(t.shop ? t.shop->name : std::string()); },
                                      [](xf::Monster& t, const FieldValue& x) {
                                          if (t.shop) t.shop->name = x.s.substr(0, 255);
                                      }));
    }
}

void npcOnly(std::vector<FieldDesc>& out)
{
    static const char* kNames[6] = {"Hair", "Hair 2", "Sideburns", "Beard", "Skin", "Skin 2"};
    for (int k = 0; k < 6; ++k) {
        const std::size_t at = static_cast<std::size_t>(k);
        out.push_back(on<xf::Npc>(desc("npc.color" + std::to_string(k), "Look", std::string(kNames[k]) + " colour", Kind::Color, "Its colour (setNPCColor)."),
                                  [at](const xf::Npc& t) { return rgbv(t.colors[at][0], t.colors[at][1], t.colors[at][2]); },
                                  [at](xf::Npc& t, const FieldValue& x) { t.colors[at] = x.rgb; }));
    }
    out.push_back(str<xf::Npc>("npc.voice", "Look", "Voice", Kind::Voice, "Its sound set (soundset.bin).", &xf::Npc::voiceSet, 255));
    out.push_back(num<xf::Npc>("npc.healthMax", "Combat", "Most health", "Its most health.", &xf::Npc::healthMax, 0, 65535));
    out.push_back(num<xf::Npc>("npc.health", "Combat", "Health", "Its health now.", &xf::Npc::healthCurrent, 0, 65535));
    out.push_back(num<xf::Npc>("npc.strength", "Combat", "Strength", "Its strength (as a player's: damage and what it can carry).", &xf::Npc::unknown331, 0, 255));
    out.push_back(fbits<xf::Npc>("npc.speed", "Combat", "Speed", "Its speed stat: base speed = this / 2 + 1.7.", &xf::Npc::unknown332, 0, 10));
    out.push_back(fbits<xf::Npc>("npc.experience", "Combat", "Experience", "What killing it is worth.", &xf::Npc::experience, 0, 1e7));
}

void generatorFields(std::vector<FieldDesc>& out, std::uint16_t v)
{
    const char* g = "Generator";
    out.push_back(handler<xf::MonsterGenerator>("gen.onHurt", g, "Damaged", "", [](xf::MonsterGenerator& t) -> map::ScriptHandler& { return t.scripts[0]; }));
    out.push_back(handler<xf::MonsterGenerator>("gen.onDie", g, "Destroyed", "", [](xf::MonsterGenerator& t) -> map::ScriptHandler& { return t.scripts[1]; }));
    out.push_back(handler<xf::MonsterGenerator>("gen.onCollide", g, "Collided", "", [](xf::MonsterGenerator& t) -> map::ScriptHandler& { return t.scripts[2]; }));
    out.push_back(handler<xf::MonsterGenerator>("gen.onSpawn", g, "Spawned a monster", "", [](xf::MonsterGenerator& t) -> map::ScriptHandler& { return t.scripts[3]; }));
    if (v > 0x3E) {
        out.push_back(flags<xf::MonsterGenerator>(
            "gen.mode", g, "Spawns when",
            "None ticked: it never spawns. The old editor's \"algorithm\" 0-3 (3 acts as 1).", &xf::MonsterGenerator::unknown6,
            {{2, "A player is near", "Any live player within 300 units (then the next bit is ignored)."},
             {4, "A player can see it", "A random player within a screen's distance with a clear line: the monster comes out toward them."}}));
    }
    out.push_back(num<xf::MonsterGenerator>("gen.alive", g, "Alive now", "How many of its monsters are alive (the game keeps count).", &xf::MonsterGenerator::unknown2, 0, 255));
    out.back().readOnly = true;
    out.back().advanced = true;
    out.push_back(num<xf::MonsterGenerator>("gen.most", g, "Most alive", "The most of its monsters alive at once (the limit below sets it on load).",
                                            &xf::MonsterGenerator::unknown3, 0, 255));
    FieldDesc last = num<xf::MonsterGenerator>("gen.last", g, "Last spawn frame", "The frame it last spawned (running state).", &xf::MonsterGenerator::unknown4, 0, 0xFFFFFFFF);
    out.push_back(adv(last));
    // Per slot (quest difficulty variant): the rate and the limit.
    static const char* kSlot[3] = {"variant 1", "variant 2", "variant 3"};
    for (int k = 0; k < 3; ++k) {
        const std::size_t at = static_cast<std::size_t>(k);
        FieldDesc rate = on<xf::MonsterGenerator>(
            desc("gen.rate" + std::to_string(k), "Slots", std::string("Spawn rate, ") + kSlot[k], Kind::Enum,
                 "How often it spawns (gamedata's SpawnRate values; a quest's hardcore stages shorten it). The game uses slot 1 outside "
                 "quests; a quest picks one of the three at random each time the map loads.",
                 0, 255),
            [at](const xf::MonsterGenerator& t) { return iv(at < t.unknown1.size() ? t.unknown1[at] : 0); },
            [at](xf::MonsterGenerator& t, const FieldValue& x) {
                if (t.unknown1.size() <= at) t.unknown1.resize(at + 1, 0);
                t.unknown1[at] = clampTo<std::uint8_t>(x.i, 0, 255);
            });
        rate.names = range({"High", "Normal", "Low", "Very low", "Very very low"});
        out.push_back(std::move(rate));
        if (v > 0x3D) {
            FieldDesc most = on<xf::MonsterGenerator>(
                desc("gen.limit" + std::to_string(k), "Slots", std::string("Limit, ") + kSlot[k], Kind::Enum,
                     "The most alive at once for this slot (gamedata's GeneratorMaxActiveCreatures: High, Normal, Low, Singular); another "
                     "value keeps \"Most alive\".",
                     0, 255),
                [at](const xf::MonsterGenerator& t) { return iv(at < t.unknown5.size() ? t.unknown5[at] : 255); },
                [at](xf::MonsterGenerator& t, const FieldValue& x) {
                    if (t.unknown5.size() <= at) t.unknown5.resize(at + 1, 255);
                    t.unknown5[at] = clampTo<std::uint8_t>(x.i, 0, 255);
                });
            most.names = {{0, "High", ""}, {1, "Normal", ""}, {2, "Low", ""}, {3, "Singular", ""}, {255, "Keep \"Most alive\"", ""}};
            out.push_back(std::move(most));
        }
    }
}

void rewardFields(std::vector<FieldDesc>& out, std::uint16_t v)
{
    const char* g = "Quest reward";
    out.push_back(flags<xf::RewardMarker>("reward.kinds", g, "Can give", "The kinds of reward it may make (weighted 16/2/2/24/16/23/16/1 in that order).",
                                          &xf::RewardMarker::rewardType,
                                          {{1, "Spell book", ""}, {2, "Ability book", ""}, {4, "Field guide", ""}, {8, "Weapon", ""},
                                           {0x10, "Armour", ""}, {0x20, "Gold or gem", ""}, {0x40, "Potion", ""},
                                           {0x80, "Gold or gem (2)", "Also the quest ankh's candidates."}}));
    out.push_back(flags<xf::RewardMarker>("reward.lists", g, "Use the map's lists", "Books come from the lists below (else from the stage's tables).",
                                          &xf::RewardMarker::unknown1, {{1, "Spells", ""}, {2, "Abilities", ""}, {4, "Field guides", ""}}));
    if (v > 0x3D) {
        out.push_back(enumf<xf::RewardMarker>("reward.chance", g, "Chance", "How likely it makes anything at all (the game's roll out of 100).",
                                              &xf::RewardMarker::activateChance,
                                              {{0, "Always", ""}, {1, "75%", ""}, {2, "50%", ""}, {3, "25%", ""}, {4, "5%", ""}}));
    }
    if (v > 0x3E) {
        out.push_back(flags<xf::RewardMarker>("reward.flags", g, "Flags", "", &xf::RewardMarker::rareOrSpecial,
                                              {{1, "Always placed", "Never left out when the level picks its rewards."},
                                               {0x80, "Chosen (runtime)", "Set while the level picks."}}));
    }
}

} // namespace

// --- Names -------------------------------------------------------------------------------------------------

const std::vector<FlagBit>& objectFlagBits()
{
    // The authoring bits first (the save's mask 0x11408162), then the runtime-only ones.
    static const std::vector<FlagBit> bits = {
        {0x2, "NO_UPDATE", "Never updated (frozen)."},
        {0x20, "DESTROYED", "Already destroyed: the load doesn't place it."},
        {0x40, "NO_COLLIDE", "Nothing collides with it."},
        {0x100, "EQUIPPED", "Worn or wielded by whoever carries it."},
        {0x8000, "DEAD", "A dead creature (a body)."},
        {0x400000, "TRANSIENT", "Decays after its decay frames."},
        {0x1000000, "ENABLED", "On (doors, lights, generators, triggers...; scripts turn it on and off)."},
        {0x10000000, "NO_AUTO_DROP", "Not dropped when its carrier dies."},
        // Runtime-only (the save never writes them; a map that stores one sets it).
        {0x1, "BELOW", ""},
        {0x4, "ACTIVE", "In the world: the load doesn't place an object stored with it."},
        {0x8, "ALLOW_OVERLAP", ""},
        {0x10, "SHORT", ""},
        {0x80, "MISSILE_HIT", ""},
        {0x200, "PARTITIONED", ""},
        {0x400, "NO_COLLIDE_OWNER", ""},
        {0x800, "OWNER_VISIBLE", ""},
        {0x1000, "EDIT_VISIBLE", ""},
        {0x2000, "NO_PUSH_CHARACTERS", ""},
        {0x4000, "AIRBORNE", ""},
        {0x10000, "SHADOW", ""},
        {0x20000, "FALLING", ""},
        {0x40000, "IN_HOLE", ""},
        {0x80000, "RESPAWN", ""},
        {0x100000, "ON_OBJECT", ""},
        {0x200000, "SIGHT_DESTROY", ""},
        {0x800000, "BOUNCY", ""},
        {0x2000000, "PENDING", ""},
        {0x4000000, "TRANSLUCENT", ""},
        {0x8000000, "STILL", ""},
        {0x20000000, "FLICKER", ""},
        {0x40000000, "SELECTED", ""},
        {0x80000000u, "MARKED", ""},
    };
    return bits;
}

const std::vector<FlagBit>& xStatusBits()
{
    static const std::vector<FlagBit> bits = {
        {0x2, "Shut", "A chest's, breakable's or trap door's state: shut."},
        {0x4, "Opening", "... opening."},
        {0x8, "Open", "... open."},
        {0x10, "Has a dialog", "An NPC with a dialog."},
        {0x40, "Bit 0x40", "Saved; its meaning isn't known."},
    };
    return bits;
}

const std::vector<TeamName>& teamNames()
{
    static const std::vector<TeamName> teams = {
        {0, "None", {128, 128, 128}},  {1, "1 (red)", {230, 40, 40}},      {2, "2 (blue)", {50, 90, 240}},   {3, "3 (green)", {40, 200, 60}},
        {4, "4 (cyan)", {40, 220, 220}}, {5, "5 (yellow)", {240, 230, 50}}, {6, "6 (violet)", {170, 70, 230}}, {7, "7 (black)", {30, 30, 30}},
        {8, "8 (white)", {240, 240, 240}}, {9, "9 (orange)", {250, 150, 40}},
    };
    return teams;
}

const std::vector<FlagBit>& monsterStatusBits()
{
    namespace ms = things::MonsterStatus;
    static const std::vector<FlagBit> bits = {
        {ms::DestroyWhenDead, "DESTROY_WHEN_DEAD", "Its body vanishes."},
        {ms::Check, "CHECK", ""},
        {ms::CanBlock, "CAN_BLOCK", ""},
        {ms::CanDodge, "CAN_DODGE", ""},
        {1u << 4, "(bit 4)", "Not used."},
        {ms::CanCastSpells, "CAN_CAST_SPELLS", ""},
        {ms::HoldYourGround, "HOLD_YOUR_GROUND", ""},
        {ms::Summoned, "SUMMONED", ""},
        {ms::Alert, "ALERT", ""},
        {ms::Injured, "INJURED", "Hurt this frame (runtime)."},
        {ms::CanSeeFriends, "CAN_SEE_FRIENDS", ""},
        {ms::CanHealSelf, "CAN_HEAL_SELF", ""},
        {ms::CanHealOthers, "CAN_HEAL_OTHERS", ""},
        {ms::CanRun, "CAN_RUN", ""},
        {ms::Running, "RUNNING", ""},
        {ms::AlwaysRun, "ALWAYS_RUN", ""},
        {ms::NeverRun, "NEVER_RUN", ""},
        {ms::Bot, "BOT", ""},
        {ms::Morphed, "MORPHED", "A mimic as a chest."},
        {ms::OnFire, "ON_FIRE", ""},
        {ms::StayDead, "STAY_DEAD", ""},
        {ms::Frustrated, "FRUSTRATED", ""},
    };
    return bits;
}

const std::vector<FlagBit>& spellUsageBits()
{
    static const std::vector<FlagBit> bits = {
        {0x08000000u, "Counter", "Cast against an incoming spell."},
        {0x10000000u, "On self", "A buff on itself."},
        {0x20000000u, "Attack", ""},
        {0x40000000u, "Hold ground", "Summons and defences while it holds."},
        {0x80000000u, "Escape", ""},
    };
    return bits;
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
                out.push_back(str<xf::Readable>("readable.text", "Readable", "Text", Kind::StringKey,
                                                "What it says: a string table key (\"file:name\") the game looks up (a missing key shows the "
                                                "key itself).",
                                                &xf::Readable::text, 255));
            } else if constexpr (std::is_same_v<T, xf::Exit>) {
                out.push_back(str<xf::Exit>("exit.map", "Exit", "Leads to", Kind::MapName,
                                            "The map it takes the players to (the game's buffer holds 79 bytes and the NUL).", &xf::Exit::mapName, 79));
                out.push_back(on<xf::Exit>(desc("exit.point", "Exit", "Return point", Kind::Point,
                                                "Not used by the exit's own travel: in solo it's where the player comes back to on this map; a "
                                                "quest's warp gate puts the players there.",
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
                out.push_back(on<xf::Hole>(desc("hole.fall", "Pit", "Falls to", Kind::Point, "Where what falls in comes out (world units).", 0, kWorldSize - 1),
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
                out.push_back(handler<xf::Hole>("hole.script", "Pit", "Stepped on", "Called the first time something steps on it while it's shut.",
                                                [](xf::Hole& t) -> map::ScriptHandler& { return t.script; }));
                out.push_back(num<xf::Hole>("hole.timeout", "Pit", "Opens after", "Frames from that first step until it opens (30 a second); 0: never by time.",
                                            &xf::Hole::scriptTimeout, 0, 65535));
                FieldDesc a = num<xf::Hole>("hole.stepped", "Pit", "Stepped on (state)", "Running state: it has been stepped on.", &xf::Hole::scriptActivated, 0, 1);
                out.push_back(adv(a));
                FieldDesc t = num<xf::Hole>("hole.opensAt", "Pit", "Opens at frame", "Running state: the frame it opens (write 0).", &xf::Hole::scriptTime, 0, 0xFFFFFFFF);
                out.push_back(adv(t));
            } else if constexpr (std::is_same_v<T, xf::Transporter>) {
                FieldDesc d = num<xf::Transporter>("transporter.target", "Teleporter", "Leads to", "The teleporter it sends to (an extent; 0 none).",
                                                   &xf::Transporter::targetExtent, 0, 0x7FFFFFFF);
                d.kind = Kind::Extent;
                out.push_back(std::move(d));
            } else if constexpr (std::is_same_v<T, xf::Elevator>) {
                FieldDesc d = num<xf::Elevator>("elevator.link", "Elevator", "Shaft", "Its shaft (an extent; 0 none).", &xf::Elevator::linkedExtent, 0, 0x7FFFFFFF);
                d.kind = Kind::Extent;
                out.push_back(std::move(d));
                if (v > 0x28) {
                    FieldDesc h = num<xf::Elevator>("elevator.height", "Elevator", "Height", "How high it is now (running state).", &xf::Elevator::height, 0, 0xFFFFFFFF);
                    out.push_back(adv(h));
                }
                if (v > 0x3C) {
                    out.push_back(enumf<xf::Elevator>("elevator.status", "Elevator", "State", "Where it is at the start.", &xf::Elevator::status,
                                                      range({"Down", "Going down", "Up", "Going up"})));
                }
            } else if constexpr (std::is_same_v<T, xf::ElevatorShaft>) {
                FieldDesc d = num<xf::ElevatorShaft>("shaft.link", "Elevator shaft", "Elevator", "Its elevator (an extent; 0 none).",
                                                     &xf::ElevatorShaft::linkedExtent, 0, 0x7FFFFFFF);
                d.kind = Kind::Extent;
                out.push_back(std::move(d));
            } else if constexpr (std::is_same_v<T, xf::Mover>) {
                out.push_back(num<xf::Mover>("mover.speed", "Mover", "Speed", "Its speed: a quarter of a world unit a frame for each.", &xf::Mover::movingSpeed, 0, 1000));
                FieldDesc w = num<xf::Mover>("mover.waypoint", "Mover", "Starts at", "The waypoint it heads for first.", &xf::Mover::waypoint, 0, 0x7FFFFFFF);
                w.kind = Kind::Waypoint;
                out.push_back(std::move(w));
                FieldDesc c = num<xf::Mover>("mover.carries", "Mover", "Carries", "The object it moves (an extent).", &xf::Mover::movedExtent, 0, 0x7FFFFFFF);
                c.kind = Kind::Extent;
                out.push_back(std::move(c));
                out.push_back(enumf<xf::Mover>("mover.state", "Mover", "State",
                                               "Ready: when enabled it heads for its first waypoint. At a waypoint with no links it's done; one link, "
                                               "it follows it; more, a random one but the way it came.",
                                               &xf::Mover::moveType, range({"Ready", "Moving", "Paused", "Done"})));
                FieldDesc t = num<xf::Mover>("mover.target", "Mover", "Heading for", "Running state: the waypoint it's heading for.", &xf::Mover::waypointStart, 0, 0x7FFFFFFF);
                t.kind = Kind::Waypoint;
                out.push_back(adv(t));
                FieldDesc p = num<xf::Mover>("mover.previous", "Mover", "Came from", "Running state: the waypoint it came from.", &xf::Mover::waypointEnd, 0, 0x7FFFFFFF);
                p.kind = Kind::Waypoint;
                out.push_back(adv(p));
                FieldDesc sb = flt<xf::Mover>("mover.speedBase", "Mover", "Base speed", "The object's speeds (running state).", &xf::Mover::speedBase, 0, 1000);
                out.push_back(adv(sb));
                FieldDesc sc = flt<xf::Mover>("mover.speedCurrent", "Mover", "Speed now", "", &xf::Mover::speedCurrent, 0, 1000);
                out.push_back(adv(sc));
            } else if constexpr (std::is_same_v<T, xf::Glyph>) {
                out.push_back(on<xf::Glyph>(desc("glyph.target", "Glyph", "Target", Kind::Point,
                                                 "Where its spells go when nothing touched it. Setting it turns the glyph to face it.", 0, kWorldSize - 1),
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
                out.push_back(num<xf::Glyph>("glyph.direction", "Glyph", "Facing", "0-255: 0 east, 64 south, 128 west, 192 north.", &xf::Glyph::direction, 0, 255));
            } else if constexpr (std::is_same_v<T, xf::Sentry>) {
                out.push_back(flt<xf::Sentry>("sentry.angle", "Sentry", "Beam angle", "Where the beam points at the start.", &xf::Sentry::basePosition, -100, 100));
                out.back().kind = Kind::Angle;
                out.push_back(flt<xf::Sentry>("sentry.speed", "Sentry", "Turn speed", "Radians a frame (0.0105 is a turn every 20 seconds).", &xf::Sentry::rotateSpeed, -1, 1));
                if (v > 0x3C) {
                    FieldDesc c = flt<xf::Sentry>("sentry.current", "Sentry", "Angle now", "Running state: the beam's angle when saved.", &xf::Sentry::currentPosition, -100, 100);
                    c.kind = Kind::Angle;
                    out.push_back(adv(c));
                }
            } else if constexpr (std::is_same_v<T, xf::SpellReward>) {
                out.push_back(str<xf::SpellReward>("book.spell", "Spell book", "Spell", Kind::Spell, "The spell it teaches.", &xf::SpellReward::spell, 255));
            } else if constexpr (std::is_same_v<T, xf::AbilityReward>) {
                out.push_back(str<xf::AbilityReward>("book.ability", "Ability book", "Ability", Kind::Ability, "The ability it teaches.", &xf::AbilityReward::ability, 255));
            } else if constexpr (std::is_same_v<T, xf::FieldGuide>) {
                out.push_back(str<xf::FieldGuide>("book.creature", "Field guide", "Creature", Kind::Creature, "The creature it describes.", &xf::FieldGuide::creature, 63));
            } else if constexpr (std::is_same_v<T, xf::SpellPagePedestal>) {
                out.push_back(on<xf::SpellPagePedestal>(desc("pedestal.spell", "Spell pedestal", "Spell", Kind::Spell, "The spell it awards (by id)."),
                                                        [](const xf::SpellPagePedestal& t) { return sv(std::string(things::spellName(t.spell))); },
                                                        [](xf::SpellPagePedestal& t, const FieldValue& r) { t.spell = things::spellId(r.s).value_or(0); }));
            } else if constexpr (std::is_same_v<T, xf::Weapon> || std::is_same_v<T, xf::Armor> || std::is_same_v<T, xf::Ammo> ||
                                 std::is_same_v<T, xf::Team>) {
                static const char* kSlots[4] = {"Effectiveness", "Material", "Enchantment 1", "Enchantment 2"};
                for (int k = 0; k < 4; ++k) {
                    const std::size_t at = static_cast<std::size_t>(k);
                    FieldDesc d = on<T>(desc("item.mod" + std::to_string(k), "Item", kSlots[k], Kind::Modifier,
                                             "A modifier from modifier.bin that may go on this item (its ALLOWED_WEAPONS / ALLOWED_ARMOR); empty: none."),
                                        [at](const T& t) { return sv(t.modifiers[at]); }, [at](T& t, const FieldValue& r) { t.modifiers[at] = r.s; });
                    d.modifierSlot = k;
                    out.push_back(std::move(d));
                }
                if constexpr (std::is_same_v<T, xf::Weapon>) {
                    if (v > 0x29) out.push_back(num<T>("item.durability", "Item", "Durability", "Its health (at most its most; a network game takes modifier.bin's).", &T::health, 0, 65535));
                    if (x.charges) {
                        out.push_back(on<T>(desc("item.charges", "Item", "Charges", Kind::Int, "Charges now.", 0, 255),
                                            [](const T& t) { return iv(t.charges ? t.charges->current : 0); },
                                            [](T& t, const FieldValue& r) {
                                                if (t.charges) t.charges->current = clampTo<std::uint8_t>(r.i, 0, 255);
                                            }));
                        out.push_back(on<T>(desc("item.chargesMax", "Item", "Most charges", Kind::Int, "The most it holds.", 0, 255),
                                            [](const T& t) { return iv(t.charges ? t.charges->max : 0); },
                                            [](T& t, const FieldValue& r) {
                                                if (t.charges) t.charges->max = clampTo<std::uint8_t>(r.i, 0, 255);
                                            }));
                        if (v > 0x3C) {
                            out.push_back(on<T>(desc("item.chargePercent", "Item", "Recharge percent", Kind::Int, "The charge percent (0-100).", 0, 100),
                                                [](const T& t) { return iv(t.charges ? t.charges->unknown : 0); },
                                                [](T& t, const FieldValue& r) {
                                                    if (t.charges) t.charges->unknown = clampTo<std::uint32_t>(r.i, 0, 100);
                                                }));
                        }
                    }
                    if (v > 0x3F) out.push_back(flags<T>("item.flags", "Item", "Item flags", "", &T::unknown2, {{1, "Outfit item", "A respawn outfit's (sells for 1 in a quest's shops)."}}));
                } else if constexpr (std::is_same_v<T, xf::Armor>) {
                    if (v > 0x28) out.push_back(num<T>("item.durability", "Item", "Durability", "Its health (at most its most; a network game takes modifier.bin's).", &T::health, 0, 65535));
                    if (v > 0x3D) out.push_back(flags<T>("item.flags", "Item", "Item flags", "", &T::unknown2, {{1, "Outfit item", "A respawn outfit's (sells for 1 in a quest's shops)."}}));
                } else if constexpr (std::is_same_v<T, xf::Ammo>) {
                    out.push_back(num<T>("item.charges", "Item", "Count", "How many it holds.", &T::current, 0, 255));
                    out.push_back(num<T>("item.chargesMax", "Item", "Most", "The most it holds.", &T::limit, 0, 255));
                }
            } else if constexpr (std::is_same_v<T, xf::Gold>) {
                out.push_back(num<xf::Gold>("gold.amount", "Gold", "Amount",
                                            "How much gold it holds. 0: the game picks an amount as the map starts (15-30, a little more "
                                            "for experienced players).",
                                            &xf::Gold::amount, 0, 0x7FFFFFFF));
            } else if constexpr (std::is_same_v<T, xf::Obelisk>) {
                out.push_back(num<xf::Obelisk>("obelisk.mana", "Obelisk", "Mana", "The mana it holds (the old editor capped it at 255; the field holds more).", &xf::Obelisk::manaPool, 0, 0x7FFFFFFF));
                out.push_back(num<xf::Obelisk>("obelisk.known", "Obelisk", "Seen on the minimap", "Shown on the minimap from the start.", &xf::Obelisk::minimapKnown, 0, 1));
                out.back().kind = Kind::Bool;
            } else if constexpr (std::is_same_v<T, xf::ToxicCloud>) {
                out.push_back(num<xf::ToxicCloud>("cloud.lifetime", "Toxic cloud", "Frames left", "How long the cloud lasts (frames).", &xf::ToxicCloud::lifetime, 0, 0x7FFFFFFF));
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
    // Gold, QuestGoldPile and QuestGoldChest hold an amount).
    if (!std::holds_alternative<xf::Gold>(o.data)) {
        const bool gem = o.type == "Diamond" || o.type == "Emerald" || o.type == "Ruby";
        if ((classes & things::ObjectClass::Treasure) || gem) {
            const things::ThingDef* t = ctx.things ? ctx.things->findThing(o.type) : nullptr;
            const auto worth = t ? t->attribute("WORTH") : std::nullopt;
            std::string text = "None: worth nothing, a plain item to pick up";
            if (worth) {
                text = "None: a shop pays " + std::string(*worth) + " gold for it";
            }
            FieldDesc d = desc("treasure.gold", "Gold", "Gold", Kind::Text,
                               "This item holds no gold. Gold (the bag), QuestGoldPile and QuestGoldChest hold an amount: place one of "
                               "those and set its Amount.");
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
        if (label.empty()) label = "Set " + f->label + (ids.size() > 1 ? " on " + std::to_string(ids.size()) + " objects" : " of " + c.type);
        changed.emplace_back(id, std::move(c));
    }
    return editObjectsOp(doc, changed, label);
}

} // namespace nox::mapedit
