// The server's world (world.hpp): how a thing is made from its thing.bin type (describe), how a
// map file becomes a World (World::load: objects with their xfers, walls, floor, polygons,
// waypoints, groups), the lookups by extent, script id and name, the wall ray (traceRay), the
// secret walls' motion, the floor under a point, and the frame's deletions (sweepDeleted).
#include "noxcore/server/world.hpp"

#include "noxcore/audio/sound_names.hpp"
#include "noxcore/server/items.hpp"
#include "noxcore/server/monsters.hpp"
#include "noxcore/server/npc.hpp"
#include "noxcore/server/physics.hpp"
#include "noxcore/server/sound.hpp"

#include "noxcore/server/wall_shapes.hpp"
#include "noxcore/map/objects.hpp"
#include "noxcore/map/polygon_hit.hpp"
#include "noxcore/map/sections.hpp"
#include "noxcore/map/xfer.hpp"
#include "noxcore/things/field_guide.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/things/spell_ids.hpp"
#include "noxcore/util/bytes.hpp"
#include "noxcore/util/random.hpp"
#include "noxcore/util/text.hpp"
#include "noxcore/util/x87.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <tuple>
#include <variant>

namespace nox::server {

namespace {

// A map section decoded, or none when the map file lacks it.
template <class Section>
std::optional<Section> optionalSection(const map::MapFile& map)
{
    if (!map.find(Section::kName)) {
        return std::nullopt;
    }
    return map::decodeSection<Section>(map);
}

// A wall grid cell is 23 units square; the wall grid is 256 x 256 cells.
constexpr float kCell = 23;
constexpr float kInvCell = kInvWallCell;

/// Where a door's edge is, from its hinge, at each of its 32 directions.
constexpr std::pair<int, int> kDoorSwing[32] = {
    {-23, -23}, {-18, -27}, {-12, -30}, {-6, -31}, {0, -32},  {6, -31},  {12, -30},  {18, -27},
    {23, -23},  {27, -18},  {30, -12},  {31, -6},  {32, 0},   {31, 6},   {30, 12},   {27, 18},
    {23, 23},   {18, 27},   {12, 30},   {6, 31},   {0, 32},   {-6, 31},  {-12, 30},  {-18, 27},
    {-23, 23},  {-27, 18},  {-30, 12},  {-31, 6},  {-32, 0},  {-31, -6}, {-30, -12}, {-27, -18},
};

using ShapeSegment = wall_shapes::Segment;
using wall_shapes::kShapes;

/// Where the ray meets the line through `p` along (1, 1). (The game's x87: every difference, the
/// products and t stay on the stack; only the meeting point is stored.)
std::pair<float, float> meetBackslash(const float r[4], float px, float py)
{
    using util::X87;
    const X87 ax = X87(r[2]) - r[0];
    const X87 ay = X87(r[3]) - r[1];
    const X87 t = ((X87(py) - r[1]) * ax - (X87(px) - r[0]) * ay) / (ay - ax);
    return {(t + px).f(), (t + py).f()};
}

/// ... and along (-1, 1).
std::pair<float, float> meetSlash(const float r[4], float px, float py)
{
    using util::X87;
    const X87 ax = X87(r[2]) - r[0];
    const X87 ay = X87(r[3]) - r[1];
    const X87 t = ((X87(py) - r[1]) * ax - (X87(px) - r[0]) * ay) / (-ay - ax);
    return {(X87(px) - t).f(), (t + py).f()};
}

/// The whole cells below v (modf's integer part of v / 23: the quotient taken on the stack, at 53
/// bits and chopped, never stored to a float; modf's own rounding is a chop too).
double wholeCells(util::X87 v)
{
    return std::trunc((v / util::X87(kCell)).d());
}

// Characters and missiles move every substep (clock.hpp); everything else once a frame.
Priority priorityOf(std::uint32_t classes)
{
    namespace oc = things::ObjectClass;
    return (classes & (oc::Player | oc::Monster | oc::Missile)) ? Priority::High : Priority::Low;
}

// A number from an attribute's text; `fallback` when it's missing or doesn't start with one.
NOX_NOINLINE float number(std::optional<std::string_view> text, float fallback)
{
    if (!text) {
        return fallback;
    }
    const std::string s(*text);
    char* end = nullptr;
    const util::RoundingScope nearest(false); // (text read to nearest, as it always was)
    const float v = std::strtof(s.c_str(), &end);
    return end != s.c_str() ? v : fallback;
}

/// Its INIT function's part that the server keeps: ChestInit / BreakInit (breakInit)
/// shut one with no state yet (xStatus 2, none of 0xE).
void applyInit(const World& w, ServerObject& o, const things::ThingDef& t, bool fresh = true)
{
    const auto init = t.attribute("INIT");
    if (!init) {
        return;
    }
    // (Not after a record's state: the exe's INIT runs as it's made, its file's gold read after.)
    if (*init == "GoldInit" && fresh && o.goldAmount == 0 && w.rng) {
        // unitInitGold: a pile with no gold yet, by the players' mean experience t
        // (from each one's unit): randInt(t x 0.01, t x 0.02) - t x -0.02 + randInt(15, 30), each
        // product __ftol'd (truncated). With no players t is 0 / 0, a NaN: __ftol gives 0 each time
        // (the 64-bit indefinite's low half), randInt(0, 0) drawing all the same, so 15-30 (an
        // original bug).
        const auto randInt = [&](int lo, int hi) {
            return hi - lo + 1 == 0 ? hi : nox::util::uniform_int_distribution<int>(lo, hi)(*w.rng);
        };
        // The players by index (Plr_GetFirst / Plr_GetNext's walk), each one's experience added in
        // that order. (The game counts every player in the table, a unit or not; one is without a
        // unit only on a hosting client between a leaver's removal and its own client's hearing of
        // it, which ours hasn't: a player here is its unit, one that's left gone.)
        std::vector<const ServerObject*> players;
        for (const ServerObject* p : w.playerUnits()) {
            if (!p->gone()) {
                players.push_back(p);
            }
        }
        std::ranges::sort(players, [](const ServerObject* a, const ServerObject* b) { return a->player->index < b->player->index; });
        // (The game's x87: each sum stored, the mean stored, each product on the stack by a double.)
        using util::X87;
        float sum = 0;
        for (const ServerObject* p : players) {
            sum = (X87(sum) + p->experience).f();
        }
        const float mean = (X87(sum) / X87::fromDouble(static_cast<double>(players.size()))).f(); // (0 / 0: NaN)
        const auto trunc = [&](double k) { return util::ftol(X87(mean) * X87::fromDouble(k)); };
        const int least = randInt(trunc(0.01), trunc(0.02));
        const std::int64_t amount = static_cast<std::int64_t>(least) - trunc(-0.02) + randInt(15, 30);
        o.goldAmount = static_cast<std::uint32_t>(amount);
    }
    if ((*init == "ChestInit" || *init == "BreakInit") && !(o.xStatus & 0xEu)) {
        o.xStatus |= 2;
    }
    // SkullInit / DirectionInit (DirectionInit): its facing from the INIT's grid way (dx dy,
    // each -1 to 1: xferDirectionToAngle's table).
    int dx = 0;
    int dy = 0;
    const std::string s(*init);
    if (s.starts_with("SkullInit") || s.starts_with("DirectionInit")) {
        // (An INIT with no numbers after its name has no space: nothing to read, rather than
        // reading from before the string.)
        const auto space = s.find(' ');
        if (space != std::string::npos && std::sscanf(s.c_str() + space, "%d %d", &dx, &dy) == 2 &&
            -1 <= dx && dx <= 1 && -1 <= dy && dy <= 1) {
            static constexpr std::uint16_t kAngles[9] = {0xA0, 0xC0, 0xE0, 0x80, 0, 0, 0x60, 0x40, 0x20};
            o.direction = kAngles[(dy * 3 + dx) + 4];
        }
    }
}

} // namespace

std::uint32_t typeFlags(const things::ThingDef& t)
{
    // FLAGS (things::thingFlags), then the type reader's own (read_things_alternative): every
    // type ENABLED, one without a COLLIDE function ("NoCollide" is none) NO_COLLIDE.
    std::uint32_t flags = things::thingFlags(t) | 0x1000000u;
    const auto collide = t.attribute("COLLIDE");
    if (!collide || collide->starts_with("NoCollide")) {
        flags |= 0x40u;
    }
    return flags;
}

bool clientOnlyType(const things::ThingDef& t)
{
    // A client-only type: not a missile, IMMOBILE, not VISIBLE_ENABLE, NO_COLLIDE, no SHADOW.
    namespace oc = things::ObjectClass;
    const std::uint32_t c = things::thingClasses(t);
    const std::uint32_t f = typeFlags(t);
    return !(c & oc::Missile) && (c & oc::Immobile) && !(c & oc::VisibleEnable) && (f & 0x40u) && !(f & 0x10000u);
}

namespace {

/// The type's fields and its CREATE function's, read into `o` (describe's first part); the type
/// found, or null.
NOX_NOINLINE const things::ThingDef* describeTypeFresh(ServerObject& o, const things::ThingDb& things, std::string_view type, const World& w)
{
    // (The type reader's text and figures, read to nearest as they always were here: the
    // server's chop rounding is for the game's figuring, x87.hpp.)
    const util::RoundingScope nearest(false);
    o.typeName = std::string(type);
    const things::ThingDef* t = things.findThing(type);
    if (t) {
        o.type = static_cast<std::uint16_t>(t - things.things.data() + 1);
        o.classes = things::thingClasses(*t);
        o.subclasses = things::thingSubclasses(*t);
        // The motion attributes, as the type reader leaves them (read_things_alternative; each line sets its
        // field, so the last of a repeated one stands).
        // MASS (1 when not given); IMMOBILE 1e10; the boulders 100 with drag 0.01; Rock7 0.25;
        // then every type's mass x 10 as the reader finishes it.
        o.mass = number(t->lastAttribute("MASS"), 1);
        if (o.classes & 0x400000u) {
            o.mass = 1e10f;
        }
        if (o.typeName == "Boulder" || o.typeName == "RollingBoulder" || o.typeName == "BoulderIndestructible") {
            o.drag = 0.01f;
            o.mass = 100.0f;
        }
        if (o.typeName == "Rock7") {
            o.mass = 0.25f;
        }
        o.mass *= 10.0f;
        o.speedBase = o.speedCur = number(t->lastAttribute("SPEED"), 0) / 32; // (SPEED)
        o.speedBonus = 0;
        // (The names "NoCollide" and "NoUpdate" are no function at all.)
        o.collides = t->attribute("COLLIDE").has_value() && !t->attribute("COLLIDE")->starts_with("NoCollide");
        // An UPDATE is a function only for the names in the exe's update table with one:
        // any other name (BoulderUpdate, SignUpdate, ImpEggUpdate, DefaultUpdate,
        // AnimateUpdate...), NoUpdate and WeaponArmorUpdate leave the thing without (so off the
        // updatable list at rest, and in the motion only when something moves it).
        if (const auto u = t->attribute("UPDATE")) {
            o.updateFn = std::string(u->substr(0, u->find(' ')));
            static constexpr std::string_view kUpdates[] = {
                "PlayerUpdate", "ProjectileUpdate", "HomingProjectileUpdate", "SpellProjectileUpdate", "AntiSpellProjectileUpdate",
                "DoorUpdate", "SparkUpdate", "ProjectileTrailUpdate", "PushUpdate", "TriggerUpdate", "ToggleUpdate", "MonsterUpdate",
                "LoopAndDamageUpdate", "ElevatorUpdate", "ElevatorShaftUpdate", "PhantomPlayerUpdate", "ObeliskUpdate",
                "LifetimeUpdate", "MagicMissileUpdate", "PixieUpdate", "SpikeBlockUpdate", "TowerUpdate", "SkullUpdate",
                "PentagramUpdate", "InvisiblePentagramUpdate", "SwitchUpdate", "BlowUpdate", "MoverUpdate",
                "BlackPowderBarrelUpdate", "OneSecondDieUpdate", "WaterBarrelUpdate", "SelfDestructUpdate", "BlackPowderBurnUpdate",
                "DeathBallUpdate", "DeathBallFragmentUpdate", "MoonglowUpdate", "SentryGlobeUpdate", "TelekinesisUpdate", "FistUpdate",
                "MeteorShowerUpdate", "MeteorUpdate", "ToxicCloudUpdate", "SmallToxicCloudUpdate", "ArachnaphobiaUpdate", "ExpireUpdate",
                "BreakUpdate", "OpenUpdate", "BreakAndRemoveUpdate", "ChakramInMotionUpdate", "FlagUpdate", "TrapDoorUpdate",
                "BallUpdate", "CrownUpdate", "UndeadKillerUpdate", "HarpoonUpdate", "MonsterGeneratorUpdate"};
            o.updates = std::ranges::find(kUpdates, std::string_view(o.updateFn)) != std::end(kUpdates);
        } else {
            o.updates = false;
        }
        if (t->lastAttribute("FLAGS")) { // (the type's, copied at its making)
            o.flags = 0;
        }
        o.flags |= typeFlags(*t);
        o.material = 0x4000; // (a type without one: NONE, read_things_alternative's default)
        if (const auto m = t->lastAttribute("MATERIAL")) { // (the names table, a bit each)
            static constexpr std::string_view kMaterials[] = {"FLESH", "CLOTH", "ANIMAL_HIDE", "WOOD", "METAL", "STONE", "EARTH", "LIQUID",
                                                              "GLASS", "PAPER", "SNOW", "MUD", "MAGIC", "DIAMOND", "NONE"};
            std::string_view rest = *m;
            o.material = 0;
            while (!rest.empty()) {
                const auto plus = rest.find('+');
                const std::string_view word = rest.substr(0, plus);
                for (std::size_t i = 0; i < std::size(kMaterials); ++i) {
                    if (word == kMaterials[i]) {
                        o.material = static_cast<std::uint16_t>(o.material | (1u << i));
                    }
                }
                rest = plus == std::string_view::npos ? std::string_view{} : rest.substr(plus + 1);
            }
        }
        o.weight = static_cast<std::uint8_t>(number(t->lastAttribute("WEIGHT"), 0));
        o.carryCapacity = static_cast<std::uint16_t>(number(t->lastAttribute("CARRYCAPACITY"), 0));
        if (const auto p = t->lastAttribute("PICKUP")) {
            o.pickup = std::string(*p);
        }
        if (const auto d = t->lastAttribute("DROP")) {
            o.dropFn = std::string(*d);
        }
        if (const auto u = t->lastAttribute("USE")) {
            const std::string use(*u);
            const auto space = use.find(' ');
            o.useFn = use.substr(0, space);
            o.useArg = space == std::string::npos ? 0 : std::atoi(use.c_str() + space + 1);
        }
        if (t->attribute("INIT").value_or("") == "GlyphInit") { // (its InitData: no spells yet)
            o.trap.emplace();
        }
        if (t->attribute("CREATE").value_or("").starts_with("RewardMarkerCreate")) {
            o.rewardMarker.emplace(); // (createRewardMarker: every kind, always)
        }
        if (o.updateFn == "FlagUpdate") {
            o.flag.emplace();
        } else if (o.updateFn == "BallUpdate") {
            o.ball.emplace();
        } else if (o.updateFn == "CrownUpdate") {
            o.crown.emplace();
        }
        if (const auto h = t->lastAttribute("HEALTH")) {
            o.maxHealth = o.health = static_cast<std::uint16_t>(number(h, 0));
            o.healthData = true;
        }
        o.experience = static_cast<float>(number(t->lastAttribute("EXPERIENCE"), 0)); // (EXPERIENCE)
        if (const auto extent = t->lastAttribute("EXTENT")) {
            const std::string e(*extent);
            float a = 0;
            float b = 0;
            if (e.starts_with("CIRCLE") && std::sscanf(e.c_str() + 6, "%f", &a) == 1) {
                o.shape = ShapeKind::Circle;
                o.radius = a;
            } else if (e.starts_with("BOX") && std::sscanf(e.c_str() + 3, "%f %f", &a, &b) == 2) {
                o.shape = ShapeKind::Box;
                o.boxWidth = a;
                o.boxHeight = b;
            } else if (e.starts_with("CENTER")) {
                o.shape = ShapeKind::Center;
            }
        }
        if (const auto c = t->attribute("COLLIDE")) {
            const std::string s(*c);
            const auto space = s.find(' ');
            o.collideFn = s.substr(0, space);
            if (space != std::string::npos) {
                o.collideArg = std::atoi(s.c_str() + space + 1);
                std::sscanf(s.c_str() + space + 1, "%*d %d", &o.collideArg2);
                if (o.collideFn == "AudioEventCollide") {
                    // Its sound by name (SmallRockMove), kept as the sound's number.
                    o.collideArg = static_cast<int>(audio::soundId(std::string_view(s).substr(space + 1)).value_or(0));
                }
                // A damage type's name after it (the damage types' table, in order).
                static constexpr std::string_view kTypes[] = {"DAMAGE_BLADE", "DAMAGE_FLAME", "DAMAGE_CRUSH", "DAMAGE_IMPALE", "DAMAGE_DRAIN",
                                                              "DAMAGE_POISON", "DAMAGE_DISPEL_UNDEAD", "DAMAGE_EXPLOSION", "DAMAGE_BITE",
                                                              "DAMAGE_ELECTRIC", "DAMAGE_CLAW", "DAMAGE_IMPACT", "DAMAGE_LAVA",
                                                              "DAMAGE_DEATH_MAGIC", "DAMAGE_PLASMA", "DAMAGE_MANA_BOMB", "DAMAGE_ZAP_RAY",
                                                              "DAMAGE_AIRBORNE_ELECTRIC"};
                for (std::size_t i = 0; i < std::size(kTypes); ++i) {
                    if (s.find(kTypes[i]) != std::string::npos) {
                        o.collideType = static_cast<std::uint8_t>(i);
                    }
                }
            }
        }
        if (const auto u = t->attribute("USE"); u && u->starts_with("WandUse")) {
            // WandUse <charges> <missile> <shots a second> <SINGLE_SHOT | MULTI_SHOT> <sound> ...
            char missile[64] = {};
            char shot[32] = {};
            char sound[64] = {};
            int charges = 0;
            float rate = 0;
            const std::string s(*u);
            if (std::sscanf(s.c_str(), "WandUse %d %63s %f %31s %63s", &charges, missile, &rate, shot, sound) >= 3) {
                o.charges = o.maxCharges = static_cast<std::uint8_t>(charges);
                o.wandMissile = missile;
                // (__ftol(FPS / rate). A rate of 0, the Sulphorous Shower's, is +inf,
                // whose 64-bit integer's low dword is 0: no wait, a shot each Action, its spray.)
                o.wandDelay = static_cast<std::uint32_t>(util::ftol(util::X87::fromDouble(30.0) / rate));
                o.wandMultiShot = std::string_view(shot) == "MULTI_SHOT";
                o.wandSound = static_cast<std::uint16_t>(audio::soundId(sound).value_or(0));
            }
        } else if (u && u->starts_with("WandCastUse")) {
            // WandCastUse <charges> <uses a second> <SPELL_NAME> (FPS / it between).
            char spell[64] = {};
            int charges = 0;
            float rate = 0;
            const std::string s(*u);
            if (std::sscanf(s.c_str(), "WandCastUse %d %f %63s", &charges, &rate, spell) == 3) {
                o.charges = o.maxCharges = static_cast<std::uint8_t>(charges);
                o.wandDelay = static_cast<std::uint32_t>(util::ftol(util::X87::fromDouble(30.0) / rate));
                o.wandSpell = things::spellId(spell).value_or(0);
            }
        }
        // (WeaponCreate, ArmorCreate) A weapon's or armour's health (one with
        // HEALTH) its modifier.bin definition's DURABILITY, a quest game's QuestDurabilityMultiplier
        // times that (chopped); its quivers and chakrams hold DefaultAmmoAmountQuest (not
        // DefaultAmmoAmount), and its staves carry QuestStaffChargeMultiplier times the charges
        // (twice that for subclass 0x40000). (Without modifier.bin: the type's HEALTH times the
        // multiplier.)
        namespace oc = things::ObjectClass;
        const bool quest = w.quest();
        const auto create = t->attribute("CREATE").value_or("");
        const things::EquipDef* equip = nullptr;
        if (w.modifierDb && t->lastAttribute("HEALTH")) {
            if (create.starts_with("WeaponCreate")) {
                equip = w.modifierDb->findWeapon(o.typeName);
            } else if (create.starts_with("ArmorCreate")) {
                equip = w.modifierDb->findArmor(o.typeName);
            }
        }
        if (equip) {
            o.maxHealth = o.health = static_cast<std::uint16_t>(equip->durability);
            if (quest) {
                // (ArmorCreate: Math_FloatToInt of FILD x the multiplier, chopped.)
                const float m = w.number("QuestDurabilityMultiplier");
                o.health = static_cast<std::uint16_t>(util::floatToInt(util::X87::fromDouble(o.health) * m));
                o.maxHealth = static_cast<std::uint16_t>(util::floatToInt(util::X87::fromDouble(o.maxHealth) * m));
            }
        } else if (!w.modifierDb && quest && (o.classes & (oc::Weapon | oc::Armor)) && o.maxHealth != 0) {
            const float m = w.number("QuestDurabilityMultiplier");
            o.maxHealth = o.health = static_cast<std::uint16_t>(util::floatToInt(util::X87::fromDouble(o.maxHealth) * m));
        }
        if ((o.classes & oc::Weapon) && (o.subclasses & 0x82u)) {
            o.charges = o.maxCharges = static_cast<std::uint8_t>(w.number(quest ? "DefaultAmmoAmountQuest" : "DefaultAmmoAmount"));
        }
        if (quest && (o.classes & oc::Wand) && (o.subclasses & 0x47F0000u)) {
            float m = w.number("QuestStaffChargeMultiplier");
            if (o.subclasses & 0x40000u) {
                m += m;
            }
            o.charges = static_cast<std::uint8_t>(static_cast<float>(o.charges) * m);
            o.maxCharges = static_cast<std::uint8_t>(static_cast<float>(o.maxCharges) * m);
        }
        if (const auto c = t->attribute("CREATE"); c && c->starts_with("ObeliskCreate")) {
            o.value = 50; // (createFnObelisk: full, its mana 0-50)
        } else if (c && c->starts_with("AnimCreate")) {
            o.xStatus |= 2; // (createFnAnim: shut, whole)
        }
        if (const auto d = t->attribute("DIE")) { // ("%s %s", the type and a sound)
            const std::string s(*d);
            char fn[64] = {};
            char arg[128] = {};
            char sound[64] = {};
            std::sscanf(s.c_str(), "%63s %127s %63s", fn, arg, sound);
            o.dieFn = fn;
            o.dieType = arg;
            o.dieSound = static_cast<std::uint16_t>(audio::soundId(sound).value_or(0));
        }
        if (const auto d = t->attribute("DAMAGE")) {
            o.damageFn = std::string(d->substr(0, d->find(' ')));
        }
        if (const auto d = t->attribute("DAMAGESOUND")) {
            o.damageSoundFn = std::string(d->substr(0, d->find(' ')));
        }
        if (o.updateFn == "LifetimeUpdate") { // ("%d", the frames it lasts)
            o.value = std::atoi(std::string(t->attribute("UPDATE")->substr(14)).c_str());
        } else if (o.updateFn == "SkullUpdate") { // (its missile's name)
            const std::string u(*t->attribute("UPDATE"));
            o.shooter.emplace();
            if (const auto space = u.find(' '); space != std::string::npos) {
                o.shooter->missile = u.substr(space + 1, u.find(' ', space + 1) - space - 1);
            }
        }
        // Its CREATE, after the type's fields (MonsterCreate: monsters.hpp).
        if (t->attribute("CREATE").value_or("") == "MonsterCreate") {
            createMonster(o, w.monsterDb ? w.monsterDb->find(o.typeName) : nullptr, w.gameFlags, t);
        }
    }
    return t;
}

} // namespace

template <class Make>
const ServerObject& World::typeProto(const things::ThingDb& things, std::size_t index, Make&& make) const
{
    TypeCache& c = typeCache_;
    if (c.things != &things || c.thingData != things.things.data() || c.thingCount != things.things.size() ||
        c.thingReindexes != things.reindexCount || c.gameFlags != gameFlags || c.gameData != gameData || c.modifiers != modifierDb ||
        c.monsters != monsterDb) {
        c = TypeCache{&things, things.things.data(), things.things.size(), things.reindexCount, gameFlags, gameData, modifierDb, monsterDb, {}};
        c.protos.resize(things.things.size());
    }
    auto& proto = c.protos[index];
    if (!proto) {
        auto fresh = std::make_unique<ServerObject>();
        make(*fresh);
        proto = std::move(fresh);
    }
    return *proto;
}

namespace {

/// describeTypeFresh as a fresh object of the type takes it, by the world's cache of each type's
/// making (the same work on a fresh object leaves it the same every time): a copy of what it left
/// the first time, the object's own extent, file extent and script id kept (the making never
/// reads them). Only for an object as made but for those three.
const things::ThingDef* describeType(ServerObject& o, const things::ThingDb& things, std::string_view type, const World& w)
{
    const things::ThingDef* t = w.typeCacheOn ? things.findThing(type) : nullptr;
    if (!t) {
        return describeTypeFresh(o, things, type, w);
    }
    const ServerObject& proto = w.typeProto(things, static_cast<std::size_t>(t - things.things.data()),
                                            [&](ServerObject& fresh) { describeTypeFresh(fresh, things, type, w); });
    const std::uint32_t extent = o.extent;
    const std::uint32_t fileExtent = o.fileExtent;
    const std::int32_t scriptId = o.scriptId;
    o = proto;
    o.extent = extent;
    o.fileExtent = fileExtent;
    o.scriptId = scriptId;
    return t;
}

/// An object of `type` as `w`'s game makes it (the type's fields, its CREATE function's, then its
/// INIT's).
void describe(ServerObject& o, const things::ThingDb& things, std::string_view type, const World& w)
{
    if (const things::ThingDef* t = describeType(o, things, type, w)) {
        // Its INIT in the server's own rounding, past describeType's nearest: unitInitGold's
        // figure is the game's x87 work (chopped), not the type reader's.
        applyInit(w, o, *t);
        if (o.classes & things::ObjectClass::Missile) { // a missile: drag 1, twice as fast
            o.drag = 1;
            o.speedCur += o.speedCur;
        }
    }
    o.priority = priorityOf(o.classes);
}

} // namespace

float World::number(std::string_view name, std::size_t index) const
{
    return things::gameNumber(gameData, name, gameFlags, index);
}

/// What an item's xfer reads into it (WeaponXfer's charges and health, ArmorXfer
/// XFerArmor's health, the item flags, AmmoXfer's ammunition, the books' entries:
/// SpellRewardXfer, AbilityRewardXfer, FieldGuideXfer; ReadableXfer
/// XFerReadable's text), a map's records and a .plr's alike.
static void readItemXfer(World& world, ServerObject& o, const map::ObjectXfer& x, const things::ThingDb& things,
                         const things::ModifierDb* modifiers)
{
    const things::ThingDef* t = things.findThing(o.typeName);
    const auto codec = t ? t->lastAttribute("XFER") : std::nullopt;
    // Only an item type with an xfer codec, and only records of version 0x3C on, carry item data here.
    if (!codec || x.xferVersion < 0x3C) {
        return;
    }
    const std::string name(*codec);
    if (name != "WeaponXfer" && name != "ArmorXfer" && name != "AmmoXfer" && name != "SpellRewardXfer" && name != "AbilityRewardXfer" &&
        name != "FieldGuideXfer" && name != "ReadableXfer") {
        return;
    }
    map::xfer::Data data;
    try {
        data = map::xfer::decode(name, x.xferVersion, x.extra, {o.classes, o.subclasses}, x.extraOffset());
    } catch (const std::exception&) {
        return;
    }
    const bool network = world.network();
    const bool quest = world.quest();
    // A weapon's or armour's health from the file (at most its most) out of a network game or
    // while a quest's .plr is applied; else its definition's durability.
    const bool anyTransfer = quest && std::ranges::any_of(world.objects(), [](const auto& kv) { return kv.second.player && kv.second.player->transfer; });
    const auto health = [&](std::uint16_t fileHealth, const things::EquipDef* def) {
        const std::uint16_t h = std::min(fileHealth, o.maxHealth);
        if (!network || (quest && anyTransfer)) {
            o.health = h;
        } else if (def) {
            o.maxHealth = o.health = static_cast<std::uint16_t>(def->durability);
        }
    };
    // Each field read only when the record's version is new enough to hold it (the exe's version tests).
    if (const auto* w = std::get_if<map::xfer::Weapon>(&data)) {
        if (w->charges) {
            const auto& c = *w->charges;
            const std::uint8_t typeMost = o.maxCharges;
            const int typePercent = o.chargePercent;
            bool reset = false;
            if (!quest) {
                o.charges = c.current;
            } else if (typeMost < c.current || static_cast<std::int32_t>(c.unknown) < 0 || typePercent < static_cast<std::int32_t>(c.unknown) ||
                       typeMost != c.max) {
                // (A quest's file with charges it can't have: none.)
                o.charges = 0;
                o.maxCharges = typeMost;
                o.chargePercent = 0;
                reset = true;
            } else {
                o.charges = c.current;
            }
            if (!reset) {
                o.maxCharges = c.max;
                o.chargePercent = static_cast<int>(c.unknown);
            }
        }
        if (0x29 < x.xferVersion) {
            health(w->health, modifiers ? modifiers->findWeapon(o.typeName) : nullptr);
        }
        if (0x3F < x.xferVersion) {
            o.itemFlags = w->unknown2;
        }
    } else if (const auto* a = std::get_if<map::xfer::Armor>(&data)) {
        if (0x28 < x.xferVersion) {
            health(a->health, modifiers ? modifiers->findArmor(o.typeName) : nullptr);
        }
        if (0x3D < x.xferVersion) {
            o.itemFlags = a->unknown2;
        }
    } else if (const auto* m = std::get_if<map::xfer::Ammo>(&data)) {
        o.charges = m->current;
        o.maxCharges = m->limit;
        if (quest) {
            o.infiniteAmmo = false;
        }
    } else if (const auto* s = std::get_if<map::xfer::SpellReward>(&data)) {
        o.bookEntry = static_cast<std::uint8_t>(things::spellId(s->spell).value_or(0)); // (spellNameToN)
    } else if (const auto* ab = std::get_if<map::xfer::AbilityReward>(&data)) {
        // (abilityNameToN: 0 when unknown)
        static constexpr std::string_view kAbilities[] = {"ABILITY_INVALID", "ABILITY_BERSERKER_CHARGE", "ABILITY_WARCRY",
                                                          "ABILITY_HARPOON", "ABILITY_TREAD_LIGHTLY", "ABILITY_EYE_OF_THE_WOLF"};
        o.bookEntry = 0;
        for (std::size_t i = 1; i < std::size(kAbilities); ++i) {
            if (ab->ability == kAbilities[i]) {
                o.bookEntry = static_cast<std::uint8_t>(i);
            }
        }
    } else if (const auto* g = std::get_if<map::xfer::FieldGuide>(&data)) {
        o.guideCreature = g->creature;
    } else if (const auto* r = std::get_if<map::xfer::Readable>(&data)) {
        o.text = r->text;
    }
}

/// XFer_ActionData's read of a saved record's AI state into the monster: its
/// frames moved by the time since the save (AssignIfGreater: at least 1), the
/// waypoints by id, the heard sound by name; the script ids it keeps where its
/// objects go in ServerObject::Monster::savedRefs (the fields 0) for the load's fix-ups.
static void readActionData(World& w, ServerObject& o, const map::xfer::ActionData::Body& b, std::uint16_t version, std::uint32_t frame)
{
    auto& m = *o.monster;
    const std::int32_t delta = static_cast<std::int32_t>(frame - b.saveFrame);
    // A saved frame moved by the time since the save (at least 1).
    const auto rebase = [&](std::uint32_t v) {
        const std::int32_t r = static_cast<std::int32_t>(v) + delta;
        return r < 1 ? 1u : static_cast<std::uint32_t>(r);
    };
    // Two floats from 8 little-endian bytes (a saved position).
    const auto pair = [](const std::array<std::uint8_t, 8>& a) {
        std::uint32_t lo = 0;
        std::uint32_t hi = 0;
        for (int i = 0; i < 4; ++i) {
            lo |= static_cast<std::uint32_t>(a[static_cast<std::size_t>(i)]) << (8 * i);
            hi |= static_cast<std::uint32_t>(a[static_cast<std::size_t>(i + 4)]) << (8 * i);
        }
        return std::pair{std::bit_cast<float>(lo), std::bit_cast<float>(hi)};
    };
    // A waypoint's id as its index + 1 in the world's list (0: none).
    const auto waypointIndex = [&](std::uint32_t id) -> std::uint32_t {
        for (std::size_t i = 0; i < w.waypoints().size(); ++i) {
            if (static_cast<std::uint32_t>(w.waypoints()[i].id) == id) {
                return static_cast<std::uint32_t>(i + 1);
            }
        }
        return 0;
    };
    ServerObject::Monster::SavedRefs refs;
    m.pathCount = static_cast<int>(std::min<std::size_t>(b.unknownList1.size(), m.path.size()));
    for (int i = 0; i < m.pathCount; ++i) {
        m.path[static_cast<std::size_t>(i)] = pair(b.unknownList1[static_cast<std::size_t>(i)]);
    }
    m.pathIndex = static_cast<int>(b.unknown2);
    std::tie(m.pathToX, m.pathToY) = pair(b.unknown3);
    m.pathAt = rebase(b.frame1);
    m.pathStatus = b.unknown4;
    m.waypointCount = static_cast<int>(std::min<std::size_t>(b.waypoints.size(), 16));
    for (int i = 0; i < m.waypointCount; ++i) {
        m.waypoints[static_cast<std::size_t>(i)] = waypointIndex(b.waypoints[static_cast<std::size_t>(i)]);
    }
    m.waypointIndex = static_cast<int>(b.unknown5);
    std::tie(m.waypointsToX, m.waypointsToY) = pair(b.unknown6);
    m.homeDirection = static_cast<int>(b.unknown7);
    std::tie(m.homeX, m.homeY) = pair(b.unknown8);
    m.heard = audio::soundId(b.sound).value_or(0); // (utilFindSound)
    std::tie(m.heardX, m.heardY) = pair(b.unknown9);
    m.heardAt = rebase(b.frame2);
    m.animFrame = b.unknown10[0];
    m.animDelay = b.unknown10[1];
    m.animDone = b.unknown10[2];
    m.field1f0 = rebase(b.frame3);
    std::tie(m.stuckX, m.stuckY) = pair(b.unknown12);
    m.sightedSoundAt = rebase(b.frame4);
    m.blockedAt = rebase(b.frame5);
    // The stack (each): the action by name, its arguments by its type (an object's script
    // id and a waypoint's id kept for the fix-ups, a frame moved), whether it started.
    m.top = static_cast<int>(b.stack.size()) - 1;
    for (std::size_t i = 0; i < b.stack.size() && i < m.stack.size(); ++i) {
        const auto& e = b.stack[i];
        auto& it = m.stack[i];
        it.action = MonsterAction::byName(e.action);
        if (it.action == MonsterAction::Invalid && e.action != "ACTION_INVALID") {
            // (A dependency: actionByName knows those too.)
            for (int d = 0x28; d < 0x48; ++d) {
                if (const char* n = MonsterAction::name(d); n && e.action == n) {
                    it.action = d;
                }
            }
        }
        for (std::size_t k = 0; k < e.args.size() && k < 2; ++k) {
            const auto& a = e.args[k];
            const auto lo = static_cast<std::uint32_t>(a.value);
            // The argument's type: 0 a raw 64-bit value, 1 an object's script id, 2 a waypoint id (both
            // kept for the fix-ups), 5 a frame (moved), 7 a byte into the low bits; else the low word.
            switch (a.type) {
            case 0:
                it.args[2 * k] = lo;
                it.args[2 * k + 1] = static_cast<std::uint32_t>(a.value >> 32);
                break;
            case 1:
            case 2:
                refs.stackArgs[i][k] = lo;
                it.args[2 * k] = 0;
                break;
            case 5:
                it.args[2 * k] = rebase(lo);
                break;
            case 7:
                it.args[2 * k] = (it.args[2 * k] & ~0xFFu) | (lo & 0xFFu);
                break;
            default:
                it.args[2 * k] = lo;
                break;
            }
        }
        it.started = e.unknown != 0;
    }
    m.seenCount = static_cast<std::uint8_t>(std::min<std::size_t>(b.unknownList2.size(), m.seen.size()));
    m.seen.fill(0);
    refs.seen = b.unknownList2;
    refs.enemy = b.unknown14;
    m.enemy = 0;
    m.lookedAt = rebase(b.frame6);
    // The dialog (dialog.hpp).
    m.dialogStart = static_cast<std::int32_t>(b.unknown15);
    m.dialogEnd = static_cast<std::int32_t>(b.unknown16);
    m.dialogType = b.unknown17;
    m.dialogAnswer = b.unknown18;
    m.portrait = b.unknownName.substr(0, b.unknownName.find('\0'));
    if (version > 3 && b.v4) {
        const auto& v = *b.v4;
        refs.heardFrom = v.scriptId1;
        m.heardFrom = 0;
        m.deadFrames = v.unknown2;
        m.movedAwayAt = rebase(v.frames1[0]);
        m.nextMelee = rebase(v.frames1[1]);
        m.hurtHandledAt = rebase(v.frames1[2]);
        m.hurtSoundAt = rebase(v.frames1[3]);
        m.idleSoundAt = rebase(v.frames2[0]);
        m.tauntSoundAt = rebase(v.frames2[1]);
        m.nearestPlayer = std::bit_cast<float>(v.unknown3);
        m.actionFrame = rebase(v.frame3);
        m.stamina = v.unknown4;
        refs.lastEnemy = v.scriptId2;
        m.lastEnemy = 0;
        m.nextLook = rebase(v.frames4[0]);
        m.reachCheckedAt = rebase(v.frames4[1]);
        refs.focus = v.scriptId3;
        m.focus = 0;
        refs.transits = v.scriptIds;
        m.transits.clear();
    }
    m.savedRefs = std::move(refs);
}

/// XFerNPC's read (from version 0x3C: the older ones aren't decoded) of an NPC:
/// its facing, script hooks, colours (setNPCColor each), roam flags, status,
/// levels, sight, health, aggression, escort, spells and their delays, strength,
/// speeds, a bomber's spells, the action it starts in, its saved AI state (XFer_ActionData), whether its
/// health is the map's (its unknown361 byte), its polygon, most health, subclass bits, health,
/// experience, voice (readNPCVoiceSet); then (the host's, game flag 1: ours
/// always) no health at all when the map's kept, and a dead one NO_COLLIDE. (Its enchantments and
/// poison: at the load's end, object_save.hpp readLoadedEnchantments. What it carries, and wearing
/// it again: World::load, npc.hpp npcWearLoaded.)
static void readNpcXfer(World& w, ServerObject& o, const map::ObjectXfer& xfer, std::uint32_t frame)
{
    map::xfer::Data data;
    try {
        data = map::xfer::decode("NPCXfer", xfer.xferVersion, xfer.extra, {o.classes, o.subclasses}, xfer.extraOffset());
    } catch (const std::exception&) {
        return;
    }
    const auto* x = std::get_if<map::xfer::Npc>(&data);
    if (!x) {
        return;
    }
    auto& m = *o.monster;
    const auto block = [](const auto& h) { return ServerObject::ScriptBlock{h.unknown, h.function}; };
    m.onEnemySighted = block(x->scripts.enemySighted);
    m.onDeath = block(x->scripts.death);
    m.lookDelay = static_cast<std::int16_t>(x->scripts.unknown305);
    m.onLookingForEnemy = block(x->scripts.lookingForEnemy);
    m.onChangeFocus = block(x->scripts.changeFocus);
    m.onIsHit = block(x->scripts.isHit);
    m.onRetreat = block(x->scripts.retreat);
    m.onCollision = block(x->scripts.collision);
    m.onHearEnemy = block(x->scripts.hearEnemy);
    m.onEndOfWaypoint = block(x->scripts.endOfWaypoint);
    m.onLostEnemy = block(x->scripts.lostEnemy);
    // Its facing (xferDirectionToAngle: Direction1 and 2).
    if (-1 <= x->direction[0] && x->direction[0] <= 1 && -1 <= x->direction[1] && x->direction[1] <= 1) {
        static constexpr std::uint16_t kAngles[9] = {0xA0, 0xC0, 0xE0, 0x80, 0, 0, 0x60, 0x40, 0x20};
        o.direction = kAngles[(x->direction[1] * 3 + x->direction[0]) + 4];
    }
    // (The u32 after the hooks is read into a local and dropped.) The six colours.
    for (std::size_t i = 0; i < x->colors.size(); ++i) {
        setNpcColor(o, static_cast<std::uint8_t>(i), x->colors[i]);
    }
    m.field333 = x->unknown333;
    m.status = x->statusFlags;
    m.field338 = std::bit_cast<float>(x->unknown338);
    m.retreatLevel = std::bit_cast<float>(x->retreatLevel);
    m.resumeLevel = std::bit_cast<float>(x->resumeLevel);
    m.sightRange = std::bit_cast<float>(x->sightRange);
    o.health = x->health; // (HealthData->Cur)
    m.aggression = m.aggression2 = std::bit_cast<float>(x->aggression);
    m.escortName = x->unknown341;
    // Its spells by name (the table emptied first), their delays.
    m.spells.fill(0);
    for (const auto& sp : x->spells) {
        if (const auto id = things::spellId(sp.name); id && *id < m.spells.size()) {
            m.spells[*id] = sp.flags;
        }
    }
    std::copy(x->spellDelays.begin(), x->spellDelays.end(), m.spellDelays.begin());
    m.field329 = std::bit_cast<float>(x->unknown329);
    m.spellPower = static_cast<int>(x->unknown510);
    m.field331 = x->unknown331;
    m.field332 = std::bit_cast<float>(x->unknown332);
    m.field330 = std::bit_cast<float>(x->unknown330);
    for (std::size_t i = 0; i < 3; ++i) {
        const auto id = things::spellId(x->defaultSpells[i]);
        m.bombSpells[i] = id ? *id : 0;
    }
    m.startAction = MonsterAction::byName(x->action); // (actionNByNameMB)
    if (xfer.xferVersion > 0x28 && x->actionData.body) {
        readActionData(w, o, *x->actionData.body, x->actionData.version, frame);
    }
    m.healthFromMap = x->unknown361;
    m.polygon = x->unknown0;
    o.maxHealth = static_cast<std::uint16_t>(x->healthMax); // (HealthData->Max, from the u32)
    o.subclasses |= x->subclassBits;                      // (the file's bits added)
    o.health = x->healthCurrent;
    o.experience = std::bit_cast<float>(x->experience);
    setNpcVoice(w, o, x->voiceSet);
    // On the host (Game_HasFlags(1)), the map's health kept means none at all.
    if (m.healthFromMap != 0) {
        o.maxHealth = 0;
        o.health = 0;
    }
    if (o.flags & ObjFlag::Dead) {
        o.flags |= ObjFlag::NoCollide; // (a body)
    }
}

/// A record's xfer read by `codec` (map::xfer::decode, by the object's class bits), handed to
/// `use` when it decodes to a `T`. A record that won't decode, or throws while it's used, leaves
/// the rest as it was.
template <class T, class Use>
static void withXfer(const map::ObjectXfer& xfer, const ServerObject& o, std::string_view codec, Use&& use)
{
    try {
        const auto data = map::xfer::decode(codec, xfer.xferVersion, xfer.extra, {o.classes, o.subclasses}, xfer.extraOffset());
        if (const auto* x = std::get_if<T>(&data)) {
            use(*x);
        }
    } catch (const std::exception&) {
    }
}

// (u32At: the little-endian u32 at `at` in a record's extra bytes.)
using util::u32At;

/// One object from its map record (the type's, then the record's header and its type's xfer),
/// as World::load reads each (and a monster generator its templates).
static ServerObject readObject(const map::ObjectXfer& xfer, std::string_view typeName, World& w, const things::ThingDb& things,
                               const things::ModifierDb* modifiers, std::uint32_t gameFlags,
                               const std::map<std::uint16_t, std::string>& typeNames, std::uint32_t frame)
{
    const auto& h = xfer.header;
    ServerObject o;
    // A fresh object of its type (newObjectWithTypeInd / newObjectByTypeID:
    // a script id from the counter but under game flag 0x200000), then its xfer reading the
    // record: the file's script id over it, or (0 in the file, nor 0x200000 nor 0x400000) a
    // second fresh one (mapReadWriteObjData).
    if (!(gameFlags & 0x200000u)) {
        o.scriptId = w.newScriptId();
    }
    o.extent = h.extent;
    o.fileExtent = h.extent;
    o.scriptId = static_cast<std::int32_t>(h.scriptId);
    if (h.scriptId == 0 && !(gameFlags & 0x600000u)) {
        o.scriptId = w.newScriptId();
    }
    describe(o, things, typeName, w);
    o.record = std::make_shared<const map::ObjectXfer>(xfer); // (what the save writes back: object_save.hpp)
    o.x = o.frameStartX = o.newX = o.lastX = h.x; // (NewPos and PrevPos with it)
    o.y = o.frameStartY = o.newY = o.lastY = h.y;
    if (h.details) {
        // (mapReadWriteObjData) The file's over the type's, but for the
        // type's NO_COLLIDE; its xStatus bits 0x5E the file's.
        o.flags = (o.flags & 0xEEBF7E9Du) | (o.flags & 0x40u) | h.details->flags;
        o.scriptName = h.details->scriptName;
        o.team = h.details->team;
        o.xStatus = (o.xStatus & ~0x5Eu) | h.details->statusFlags;
        // Its pickup handler (xferReadScriptHandler: the name, a function the script lacks none; its flags).
        if (h.details->onPickup) {
            o.onPickup = {h.details->onPickup->unknown, h.details->onPickup->function};
        }
        // What it owns, by script id: noted for the fix-ups.
        if (!(gameFlags & 0x600000u)) {
            for (const std::uint32_t id : h.details->ownedScriptIds) {
                w.pendingOwners.push_back({o.scriptId, id});
            }
        }
        // A decaying one's frames left (version 0x40, TRANSIENT): its decay set once it's placed
        // (unitSetDecayTime from server_mapRWObjectData).
        if (h.details->decayFrames && *h.details->decayFrames > 0 && (o.flags & 0x400000u)) {
            o.decayAt = frame + static_cast<std::uint32_t>(*h.details->decayFrames);
        }
        if (const things::ThingDef* t = things.findThing(o.typeName)) {
            applyInit(w, o, *t, false); // (its INIT after the file's state)
        }
    }
    if ((o.classes & things::ObjectClass::Door) && xfer.extra.size() >= 8) {
        // DoorXfer: u32 direction, u32 lock, then (version 0x29 on) u32
        // the shut direction.
        o.doorDirection = static_cast<std::uint8_t>(u32At(xfer.extra, 0) & 31);
        o.lock = static_cast<std::uint8_t>(u32At(xfer.extra, 4));
        o.doorClosed = xfer.xferVersion >= 0x29 && xfer.extra.size() >= 12 ? static_cast<std::uint8_t>(u32At(xfer.extra, 8) & 31) : o.doorDirection;
        o.doorPrevious = o.doorDirection;
        o.doorAngle = static_cast<std::int16_t>(o.doorDirection * 8);
    }
    if ((o.classes & 0xC000u) && xfer.extra.size() >= 4) {  // an elevator (0x4000) or its shaft (0x8000)
        // ElevatorXfer: u32 its shaft, (version 0x29 on) u32 its height,
        // (0x3D on) u8 its state. ElevatorShaftXfer: u32 its elevator.
        const auto& e = xfer.extra;
        o.elevatorLink = u32At(e, 0);
        std::size_t at = 4;
        if ((o.classes & 0x4000u) && xfer.xferVersion > 0x28 && at + 4 <= e.size()) {
            o.elevatorLevel = static_cast<std::int32_t>(u32At(e, at));
            at += 4;
        }
        if ((o.classes & 0x4000u) && xfer.xferVersion > 0x3C && at < e.size()) {
            o.elevatorState = e[at];
        }
    }
    if (o.updateFn == "ObeliskUpdate" && xfer.xferVersion > 0x3C && xfer.extra.size() >= 4) {
        // ObeliskXfer: (version 0x3D on) u32 its mana, u8 seen on the minimap.
        o.value = static_cast<int>(u32At(xfer.extra, 0));
    }
    if ((o.classes & things::ObjectClass::Hole) && o.collideFn == "TrapDoorCollide") {
        // HoleXfer: its handler, where what falls in comes out, when it
        // opens and the frames to it, whether it's been stepped on. (Records older
        // than 0x3C: none of it.)
        o.pit.emplace();
        if (xfer.xferVersion >= 0x3C) {
            withXfer<map::xfer::Hole>(xfer, o, "HoleXfer", [&](const map::xfer::Hole& hole) {
                auto& d = *o.pit;
                d.stepped = {hole.script.unknown, hole.script.function};
                d.fallX = static_cast<float>(hole.fallX);
                d.fallY = static_cast<float>(hole.fallY);
                d.opensAt = hole.scriptTime;
                d.delay = hole.scriptTimeout;
                d.sprung = hole.scriptActivated != 0;
            });
        }
    }
    if (o.collideFn == "ExitCollide" && xfer.xferVersion >= 0x3C) {
        // ExitXfer: the map it leads to, and (0x1F on) a point: a solo map's return point, a quest warp
        // gate's way out.
        withXfer<map::xfer::Exit>(xfer, o, "ExitXfer", [&](const map::xfer::Exit& x) {
            o.exitMap = x.mapName;
            o.exitX = x.x; // (the return point / a warp gate's way out)
            o.exitY = x.y;
        });
    }
    if (o.collideFn == "SignCollide" || o.updateFn == "ToxicCloudUpdate" || o.updateFn == "SmallToxicCloudUpdate") {
        // ReadableXfer: the sign's text; ToxicCloudXfer: the
        // frames the cloud has left.
        if (o.collideFn == "SignCollide") {
            withXfer<map::xfer::Readable>(xfer, o, "ReadableXfer", [&](const map::xfer::Readable& r) { o.text = r.text; });
        } else {
            withXfer<map::xfer::ToxicCloud>(xfer, o, "ToxicCloudXfer", [&](const map::xfer::ToxicCloud& c) { o.value = static_cast<int>(c.lifetime); });
        }
    }
    if (o.collideFn == "AwardSpellCollide") {
        // SpellPagePedestalXfer: a pedestal's spell (books.hpp).
        withXfer<map::xfer::SpellPagePedestal>(xfer, o, "SpellPagePedestalXfer",
                                               [&](const map::xfer::SpellPagePedestal& p) { o.collideArg = static_cast<int>(p.spell); });
    }
    if (o.updateFn == "MoverUpdate") {
        // MoverXfer: its speed, first waypoint, what it carries; (0x29 on) its
        // state and its waypoints now; (0x2A on) its speeds.
        o.mover.emplace();
        withXfer<map::xfer::Mover>(xfer, o, "MoverXfer", [&](const map::xfer::Mover& m) {
            o.mover->speed = static_cast<int>(m.movingSpeed);
            o.mover->start = static_cast<std::int32_t>(m.waypoint);
            o.mover->carried = m.movedExtent;
            o.mover->state = m.moveType;
            o.mover->target = static_cast<std::int32_t>(m.waypointStart);
            o.mover->previous = static_cast<std::int32_t>(m.waypointEnd);
            o.speedBase = m.speedBase;
            o.speedCur = m.speedCurrent;
        });
    }
    readItemXfer(w, o, xfer, things, modifiers);
    if (o.classes & 0x13001000u) {  // a weapon, armour, flag or wand
        // WeaponXfer / ArmorXfer / AmmoXfer / TeamXfer: its modifiers by name
        // (modifGetIdByName; unknown or empty: none), a flag's home where it
        // stands.
        const things::ThingDef* t = things.findThing(o.typeName);
        const auto codec = t ? t->lastAttribute("XFER") : std::nullopt;
        if (codec && xfer.xferVersion >= 0x3C) {
            try {
                const auto data = map::xfer::decode(*codec, xfer.xferVersion, xfer.extra, {o.classes, o.subclasses}, xfer.extraOffset());
                const map::xfer::Modifiers* names = nullptr;
                if (const auto* x = std::get_if<map::xfer::Weapon>(&data)) {
                    names = &x->modifiers;
                } else if (const auto* a = std::get_if<map::xfer::Armor>(&data)) {
                    names = &a->modifiers;
                } else if (const auto* m = std::get_if<map::xfer::Ammo>(&data)) {
                    names = &m->modifiers;
                } else if (const auto* tm = std::get_if<map::xfer::Team>(&data)) {
                    names = &tm->modifiers;
                    if (o.flag) {
                        o.flag->homeX = o.x;
                        o.flag->homeY = o.y;
                    }
                }
                if (names) {
                    for (std::size_t i = 0; i < 4; ++i) {
                        const things::ModifierDef* m = modifiers && !(*names)[i].empty() ? modifiers->findModifier((*names)[i]) : nullptr;
                        o.modifiers[i] = m ? m->id : 0xFF;
                    }
                }
            } catch (const std::exception&) {
            }
        }
    }
    if (o.monster && (o.subclasses & things::ObjectSubclass::Shopkeeper)) {
        o.shopkeeper.emplace(); // (MonsterCreate: a shopkeeper's 1.0 and 0x3EAAAA3B)
    }
    const things::ThingDef* typeDef = o.monster ? things.findThing(o.typeName) : nullptr;
    if (o.monster && xfer.xferVersion >= 0x3C && typeDef && typeDef->lastAttribute("XFER").value_or("") == "NPCXfer") {
        readNpcXfer(w, o, xfer, frame);
    } else if (o.monster && xfer.xferVersion >= 0x3C) {
        // MonsterXfer: the map's script hooks, status, levels, aggression, sight,
        // the action it starts in.
        withXfer<map::xfer::Monster>(xfer, o, "MonsterXfer", [&](const map::xfer::Monster& x) {
            auto& m = *o.monster;
            const auto block = [](const auto& h) { return ServerObject::ScriptBlock{h.unknown, h.function}; };
            m.onEnemySighted = block(x.scripts.enemySighted);
            m.onDeath = block(x.scripts.death);
            m.onLookingForEnemy = block(x.scripts.lookingForEnemy);
            m.lookDelay = static_cast<std::int16_t>(x.scripts.unknown305);
            m.escortName = x.unknown341;
            m.onChangeFocus = block(x.scripts.changeFocus);
            m.onIsHit = block(x.scripts.isHit);
            m.onRetreat = block(x.scripts.retreat);
            m.onCollision = block(x.scripts.collision);
            m.onHearEnemy = block(x.scripts.hearEnemy);
            m.onEndOfWaypoint = block(x.scripts.endOfWaypoint);
            m.onLostEnemy = block(x.scripts.lostEnemy);
            m.field333 = x.unknown333;
            m.status = x.statusFlags;
            m.field338 = std::bit_cast<float>(x.unknown338);
            m.retreatLevel = std::bit_cast<float>(x.retreatLevel);
            m.resumeLevel = std::bit_cast<float>(x.resumeLevel);
            m.sightRange = std::bit_cast<float>(x.sightRange);
            m.aggression = m.aggression2 = std::bit_cast<float>(x.aggression);
            m.field329 = std::bit_cast<float>(x.unknown329);
            m.field330 = std::bit_cast<float>(x.unknown330);
            m.startAction = MonsterAction::byName(x.action);
            // Its facing (xferDirectionToAngle: Direction1 and 2 from the
            // record's way, the table by dy x 3 + dx).
            if (-1 <= x.direction[0] && x.direction[0] <= 1 && -1 <= x.direction[1] && x.direction[1] <= 1) {
                static constexpr std::uint16_t kAngles[9] = {0xA0, 0xC0, 0xE0, 0x80, 0, 0, 0x60, 0x40, 0x20};
                o.direction = kAngles[(x.direction[1] * 3 + x.direction[0]) + 4];
            }
            // Its spells by name (the table emptied first), their delays, its power.
            m.spells.fill(0);
            for (const auto& sp : x.spells) {
                if (const auto id = things::spellId(sp.name); id && *id < m.spells.size()) {
                    m.spells[*id] = sp.flags;
                }
            }
            std::copy(x.spellDelays.begin(), x.spellDelays.end(), m.spellDelays.begin());
            m.spellPower = static_cast<int>(x.unknown510);
            for (std::size_t i = 0; i < 3; ++i) {
                const auto id = things::spellId(x.defaultSpells[i]);
                m.bombSpells[i] = id ? *id : 0;
            }
            m.healthFromMap = x.unknown361;
            o.health = x.health;
            m.resumeFromDef = x.unknown337[0];
            m.retreatFromDef = x.unknown337[1];
            m.statusFromDef = x.unknown337[2];
            m.typeSpells = x.unknown337[3];
            // A female NPC's colours (from version 0x36, each through setNPCColor)
            // and voice (0x37 on), a wounded one's voice (0x3F on): npc.hpp.
            if (x.colors) {
                for (std::size_t i = 0; i < x.colors->size(); ++i) {
                    setNpcColor(o, static_cast<std::uint8_t>(i), (*x.colors)[i]);
                }
            }
            if (x.voiceSet) {
                setNpcVoice(w, o, *x.voiceSet);
            }
            if (x.voiceSet2) {
                setNpcVoice(w, o, *x.voiceSet2);
            }
            if (x.shop) {
                // A shopkeeper's (XFerMonster -> XFer_ReadShopItem): what it buys at, (version 0x3D on)
                // sells for, (0x30 on) its greeting, its stock: each record's type, count, a
                // book's spell or ability (spellNameToN /
                // abilityNameToN) or a guide's creature, its modifiers by name.
                auto& k = o.shopkeeper ? *o.shopkeeper : o.shopkeeper.emplace();
                k.buyMultiplier = std::bit_cast<float>(x.shop->unknown1);
                if (xfer.xferVersion > 0x3C) {
                    k.sellMultiplier = std::bit_cast<float>(x.shop->unknown2);
                }
                k.greeting = x.shop->name;
                static constexpr std::string_view kAbilities[] = {"ABILITY_INVALID",       "ABILITY_BERSERKER_CHARGE", "ABILITY_WARCRY",
                                                                  "ABILITY_HARPOON",       "ABILITY_TREAD_LIGHTLY",    "ABILITY_EYE_OF_THE_WOLF"};
                for (const auto& item : x.shop->items) {
                    ServerObject::Shopkeeper::Stock s;
                    s.type = item.type;
                    s.count = item.count;
                    const things::ThingDef* t = things.findThing(item.type);
                    const auto codec = t ? t->lastAttribute("XFER") : std::nullopt;
                    if (!item.parameter.empty()) {
                        if (codec && *codec == "FieldGuideXfer") {
                            s.guide = item.parameter;
                        } else if (codec && *codec == "AbilityRewardXfer") {
                            for (std::size_t i = 1; i < std::size(kAbilities); ++i) {
                                s.parameter = item.parameter == kAbilities[i] ? static_cast<std::int32_t>(i) : s.parameter;
                            }
                        } else if (const auto id = things::spellId(item.parameter)) {
                            s.parameter = static_cast<std::int32_t>(*id);
                        }
                    }
                    for (std::size_t i = 0; i < 4; ++i) {
                        const things::ModifierDef* md = modifiers && !item.modifiers[i].empty() ? modifiers->findModifier(item.modifiers[i]) : nullptr;
                        s.modifiers[i] = md ? md->id : 0xFF;
                    }
                    k.stock.push_back(std::move(s));
                }
            }
            m.polygon = x.unknown0;             // (from version 0x2C)
            o.subclasses |= x.subclassBits;      // (from version 0x2D: the file's bits added)
            if (xfer.xferVersion > 0x28 && x.actionData.body) {
                readActionData(w, o, *x.actionData.body, x.actionData.version, frame);
            }
            // On the host (Game_HasFlags(1): ours always) the map's health kept (unknown361)
            // means none at all: the story's maidens and captains (as the stock saves have them).
            if (m.healthFromMap != 0) {
                o.maxHealth = 0;
                o.health = 0;
            }
            if ((o.flags & ObjFlag::Dead) && o.typeName != "Zombie" && o.typeName != "VileZombie") {
                o.flags |= ObjFlag::NoCollide; // (a body)
            }
            spawnFixup(o, gameFlags); // (monsterOnSpawnSpellcaster)
        });
    }
    if (o.updateFn == "SentryGlobeUpdate") {
        // SentryXfer: its start and turn, (version 0x3D on) its way now.
        o.sentry.emplace();
        withXfer<map::xfer::Sentry>(xfer, o, "SentryXfer", [&](const map::xfer::Sentry& s) {
            o.sentry->start = s.basePosition;
            o.sentry->speed = s.rotateSpeed;
            o.sentry->angle = s.currentPosition;
        });
    }
    if (o.classes & things::ObjectClass::Transporter) {
        // TransporterXfer: u32 its partner (0: none).
        o.transport.emplace();
        if (xfer.extra.size() >= 4) {
            o.transport->link = u32At(xfer.extra, 0);
        }
    }
    if (o.classes & things::ObjectClass::Trigger) {
        // TriggerXfer: its box (at most 60 a side), what may press it, its
        // handlers (by name, resolved as they're called), its flags and state. (Records
        // older than 0x3C: no handlers.)
        o.trigger.emplace();
        if (xfer.xferVersion >= 0x3C) {
            withXfer<map::xfer::Trigger>(xfer, o, "TriggerXfer", [&](const map::xfer::Trigger& t) {
                o.boxWidth = std::min(static_cast<float>(t.sizeX), 60.0f);
                o.boxHeight = std::min(static_cast<float>(t.sizeY), 60.0f);
                auto& d = *o.trigger;
                d.flags = t.unknown1;
                d.collided = {t.onCollided.unknown, t.onCollided.function};
                d.pressed = {t.onPressed.unknown, t.onPressed.function};
                d.released = {t.onReleased.unknown, t.onReleased.function};
                d.allowedClasses = t.allowedClasses;
                d.ignoredClasses = t.ignoredClasses;
                d.allowedTeam = t.allowedTeam;
                d.ignoredTeam = t.ignoredTeam;
                d.state = t.state;
                o.animFrame = t.animFrame;
                if (xfer.xferVersion > 0x3C) {
                    o.animOwed = 0xFFFFFFFFu; // (servMarkObjAnimFrame)
                }
            });
        }
    }
    if (o.trap) {
        // GlyphXfer: (before version 0x29 four bytes), u8 its facing, f32 x 2
        // the point its spells go at, u8 their count, then the spells: before version
        // 0x1F five u32 ids, after it each a u8-counted name (0: unknown).
        const auto& e = xfer.extra;
        std::size_t at = xfer.xferVersion < 0x29 ? 4 : 0;
        const auto f32 = [&](std::size_t p) { return std::bit_cast<float>(u32At(e, p)); };
        if (at + 10 <= e.size()) {
            o.direction = e[at];
            o.trap->x = f32(at + 1);
            o.trap->y = f32(at + 5);
            const std::uint8_t count = e[at + 9];
            at += 10;
            // (Past five, the exe's spells run over the count and the rest of its
            // InitData, an original bug. Here the first five.)
            o.trap->count = std::min<std::uint8_t>(count, 5);
            const int listed = xfer.xferVersion < 0x1F ? 5 : count;
            for (int i = 0; i < listed; ++i) {
                std::uint32_t spell = 0;
                if (xfer.xferVersion < 0x1F) {
                    if (at + 4 > e.size()) {
                        break;
                    }
                    spell = u32At(e, at);
                    at += 4;
                } else {
                    if (at >= e.size() || at + 1 + e[at] > e.size()) {
                        break;
                    }
                    const std::string_view word(reinterpret_cast<const char*>(e.data() + at + 1), e[at]);
                    spell = things::spellId(word).value_or(0);
                    at += 1 + e[at];
                }
                if (i < 5) {
                    o.trap->spells[static_cast<std::size_t>(i)] = spell;
                }
            }
        }
    }
    if (o.pickup == "GoldPickup") {
        // GoldXfer: the pile's gold.
        withXfer<map::xfer::Gold>(xfer, o, "GoldXfer", [&](const map::xfer::Gold& g) {
            o.goldAmount = g.amount;
        });
    }
    if (o.rewardMarker) {
        // RewardMarkerXfer: its kinds, which lists it draws from, the lists by name
        // (spells, abilities, guide creatures), the five words after them (unused), (0x3E on)
        // its odds, (0x3F on) its flags.
        withXfer<map::xfer::RewardMarker>(xfer, o, "RewardMarkerXfer", [&](const map::xfer::RewardMarker& m) {
            auto& r = *o.rewardMarker;
            r.kinds = m.rewardType;
            r.listed = m.unknown1;
            for (const auto& name : m.spells) { // (spellNameToN)
                if (const auto id = things::spellId(name); id && *id < r.spells.size()) {
                    r.spells[*id] = true;
                }
            }
            // (abilityNameToN: the names at ABILITY_INVALID first)
            static constexpr std::string_view kAbilities[] = {"ABILITY_INVALID",       "ABILITY_BERSERKER_CHARGE", "ABILITY_WARCRY",
                                                              "ABILITY_HARPOON",       "ABILITY_TREAD_LIGHTLY",    "ABILITY_EYE_OF_THE_WOLF"};
            for (const auto& name : m.abilities) {
                for (std::size_t i = 1; i < std::size(kAbilities); ++i) {
                    if (name == kAbilities[i]) {
                        r.abilities[i] = true;
                    }
                }
            }
            for (const auto& name : m.creatures) { // (Game_GetCreatureGuideIndex)
                for (std::size_t i = 0; i < things::kGuideNames.size(); ++i) {
                    if (name == things::kGuideNames[i]) {
                        r.guides[i + 1] = true;
                    }
                }
            }
            r.chance = m.activateChance;
            r.flags = m.rareOrSpecial;
        });
    }
    if (o.classes & things::ObjectClass::MonsterGenerator) {
        // MonsterGeneratorCreate: made where a player sees it, no handlers.
        // MonsterGeneratorXfer: its spawn rates, how many out, its most, its last
        // spawn, its hurt / death / touch / spawn handlers, its monsters (whole records, with
        // what they carry) by difficulty; (0x3E on) its most by difficulty; (0x3F on) where.
        o.generator.emplace();
        if (xfer.xferVersion >= 0x3C) {
            withXfer<map::xfer::MonsterGenerator>(xfer, o, "MonsterGeneratorXfer", [&](const map::xfer::MonsterGenerator& g) {
                auto& d = *o.generator;
                for (std::size_t i = 0; i < 3 && i < g.unknown1.size(); ++i) {
                    d.rate[i] = g.unknown1[i];
                }
                d.active = g.unknown2;
                d.most = g.unknown3;
                d.lastSpawn = g.unknown4;
                const auto block = [](const auto& s) { return ServerObject::ScriptBlock{s.unknown, s.function}; };
                d.onHurt = block(g.scripts[0]);
                d.onDie = block(g.scripts[1]);
                d.onCollide = block(g.scripts[2]);
                d.onSpawn = block(g.scripts[3]);
                for (std::size_t k = 0; k < 3 && k < g.groups.size(); ++k) {
                    for (const auto& t : g.groups[k]) {
                        ServerObject::Generator::Template tp;
                        tp.object = std::make_shared<ServerObject>(
                            readObject(map::ObjectXfer::parse(t.record.data), t.type, w, things, modifiers, gameFlags, typeNames, frame));
                        for (const auto& rec : t.inventory) {
                            const auto n = typeNames.find(rec.tocId);
                            tp.items.push_back(std::make_shared<ServerObject>(readObject(map::ObjectXfer::parse(rec.data),
                                                                                         n != typeNames.end() ? std::string_view(n->second) : std::string_view(),
                                                                                         w, things, modifiers, gameFlags, typeNames, frame)));
                        }
                        d.groups[k].push_back(std::move(tp));
                    }
                }
                for (std::size_t i = 0; i < 3 && i < g.unknown5.size(); ++i) {
                    d.mostBy[i] = g.unknown5[i];
                }
                if (xfer.xferVersion > 0x3E) {
                    d.flags = g.unknown6;
                }
            });
        }
    }
    return o;
}

ServerObject objectFromRecord(World& world, const map::ObjectXfer& xfer, std::string_view typeName, const things::ThingDb& things,
                              const things::ModifierDb* modifiers)
{
    return readObject(xfer, typeName, world, things, modifiers, world.gameFlags, {}, world.frameNow);
}

ServerObject describeType(World& world, const things::ThingDb& things, std::string_view type)
{
    ServerObject o;
    describe(o, things, type, world);
    return o;
}

std::vector<std::uint32_t> World::placeRecords(World& w, const map::ObjectTOC& toc, const map::ObjectData& data, const things::ThingDb& things,
                                             const things::ModifierDb* modifiers, std::uint32_t gameFlags, std::uint32_t frame, bool live)
{
    std::vector<std::uint32_t> roots;
    std::map<std::uint16_t, std::string> typeNames;
    for (const auto& e : toc.entries) {
        typeNames[e.id] = e.name;
    }
    const auto& records = data.records;
    std::vector<map::ObjectXfer> xfers;
    xfers.reserve(records.size());
    for (const auto& r : records) {
        xfers.push_back(map::ObjectXfer::parse(r.data));
    }
    // (Our extents are keys: one the map gives twice, and the things made at the load, take
    // numbers past all of the file's; the file's kept as each one's fileExtent.)
    for (const auto& x : xfers) {
        w.nextExtent_ = std::max(w.nextExtent_, x.header.extent + 1);
    }
    // Each record read in the file's order (server_mapRWObjectData): a fresh object, its xfer (what it holds
    // read in it, xfer, each put at the inventory's head), then a placed one put
    // in the world (servMapLoadPlaceObj: but a client-only thing, freed with
    // what it holds, the clients loading it from their own map; and createAt
    // places nothing ACTIVE or DESTROYED).
    const auto add = [&](const auto& self, const map::ObjectTree& node, std::uint32_t holder, bool keep) -> std::uint32_t {
        const auto name = typeNames.find(records[node.record].tocId);
        ServerObject o = readObject(xfers[node.record], name != typeNames.end() ? std::string_view(name->second) : std::string_view(), w, things,
                                    modifiers, gameFlags, typeNames, frame);
        o.holder = holder;
        bool placed = keep;
        if (holder == 0) {
            const things::ThingDef* t = things.findThing(o.typeName);
            if (t && !(gameFlags & 0x200000u) && clientOnlyType(*t)) {
                w.clientOnly.push_back({o.type, o.typeName, o.record});
                placed = false;
            }
            if (o.flags & 0x24u) {  // ACTIVE or DESTROYED already
                placed = false;
            }
        }
        std::uint32_t extent = 0;
        if (placed) {
            if (o.extent == 0 || w.objects_.contains(o.extent) || (kDynamicExtents <= o.extent && std::ranges::find(w.reservedExtents, o.extent) != w.reservedExtents.end())) {
                // (0: one made in play, saved; one of the numbers made in play the kept things have)
                o.extent = w.newExtent();
                w.noteRenumbered(o); // (its map number still names it: byMapNumber)
            }
            extent = o.extent;
            if (holder == 0) {
                // (createAt) Its flags but the type's 0x35E9FEDB, ACTIVE; made
                // (the frame it was made) and its timer this frame; home where it stands; in a
                // network game not a quest, gold and items (0x3001110, no missile) NO_COLLIDE.
                o.flags = (o.flags & 0x35E9FEDBu) | ObjFlag::Active;
                o.born = o.timer = frame;
                if (o.classes & things::ObjectClass::Elevator) {
                    o.elevatorStopped = frame; // (that timer is the exe's elevator's stop frame: at rest, it waits a second from here)
                }
                o.homeX = o.x;
                o.homeY = o.y;
                if (w.network() && !w.quest() && (o.typeName == "Gold" || (o.classes & 0x3001110u)) && !(o.classes & things::ObjectClass::Missile)) {
                    o.flags |= ObjFlag::NoCollide;
                }
                if (!live) {
                    w.loadOrder.push_back(extent);
                }
            }
            w.objects_[extent] = std::move(o);
            if (holder == 0) {
                ServerObject& made = w.objects_.at(extent);
                w.noteNew(made); // (createAt's new list)
                if (live && (made.classes & (things::ObjectClass::Monster | things::ObjectClass::Player))) {
                    postCreateNotify(w, made); // (unitPostCreateNotify: the players there its friends)
                }
            }
        }
        std::vector<std::uint32_t> items;
        for (const auto& item : node.inventory) {
            if (const std::uint32_t e = self(self, item, extent, placed)) {
                items.insert(items.begin(), e);
            }
        }
        if (!placed) {
            return 0;
        }
        ServerObject& made = w.objects_.at(extent);
        made.inventory = std::move(items);
        // NPCXfer's end: what it has worn, worn again (the host's: npc.hpp).
        if (made.monster) {
            if (const things::ThingDef* t = things.findThing(made.typeName); t && t->lastAttribute("XFER").value_or("") == "NPCXfer") {
                npcWearLoaded(w, modifiers, made);
            }
        }
        // MonsterXfer's end: a bomber with spells and no glyph carries one of them.
        if (made.monster && (made.subclasses & 0x2000u) &&
            std::ranges::any_of(made.monster->bombSpells, [](std::uint32_t s) { return s != 0; }) &&
            std::ranges::none_of(made.inventory, [&](std::uint32_t i) {
                const auto it = w.objects_.find(i);
                return it != w.objects_.end() && it->second.typeName == "Glyph";
            })) {
            bomberGlyph(w, things, made, false);
        }
        return extent;
    };
    for (const auto& root : map::buildObjectTree(xfers)) {
        if (const std::uint32_t e = add(add, root, 0, true)) {
            roots.push_back(e);
        }
    }
    return roots;
}

void World::attachDoorWall(const ServerObject& o)
{
    if (!(o.classes & things::ObjectClass::Door) || o.holder != 0) {
        return;
    }
    const auto [dx, dy] = kDoorSwing[o.doorClosed];
    // (On the stack, then __ftol: no float store between.)
    const int cx = util::ftol((util::X87::fromDouble(dx / 2) + o.x) * kInvCell);
    const int cy = util::ftol((util::X87::fromDouble(dy / 2) + o.y) * kInvCell);
    if (cx >= 0 && cx < 256 && cy >= 0 && cy < 256) {  // (the wall grid's 256 x 256)
        createWall(cx, cy).door = o.extent;
    }
}

std::vector<std::uint32_t> World::placeLiveRecords(const map::ObjectTOC& toc, const map::ObjectData& data, const things::ThingDb& things,
                                                   std::uint32_t frame)
{
    // (noxcpp's map editor sessions only: the load's own reading of each record and its placing,
    // then a door's wall as the load gives it; never in a game the exe could host.)
    std::vector<std::uint32_t> roots = placeRecords(*this, toc, data, things, modifierDb, gameFlags, frame, true);
    for (const std::uint32_t e : roots) {
        if (const ServerObject* o = byExtent(e)) {
            attachDoorWall(*o);
        }
    }
    return roots;
}

World World::load(const map::MapFile& map, const things::ThingDb& things, std::uint32_t gameFlags, const things::GameData* gameData,
                  const things::ModifierDb* modifiers, const things::MonsterDb* monsterDb, std::int32_t nextScriptId, std::uint32_t frame,
                  std::vector<std::uint32_t> reserved)
{
    World w;
    w.reservedExtents = std::move(reserved);
    w.gameFlags = gameFlags;
    w.gameData = gameData;
    w.monsterDb = monsterDb;
    w.modifierDb = modifiers;
    w.thingDb = &things;
    w.nextScriptId_ = nextScriptId;
    w.frameNow = frame;
    // The waypoints (WayPoints comes before ObjectData in the file: a saved monster's finds its
    // waypoints by id).
    if (const auto waypoints = optionalSection<map::Waypoints>(map)) {
        for (const auto& wp : waypoints->waypoints) {
            // (The map's flag bit 0 is the waypoint's on state, which WaypointOn / Off set.)
            // (Its links kept in its own record.)
            ServerWaypoint s{static_cast<std::int32_t>(wp.id), wp.name, wp.x, wp.y, (wp.flags & 1) != 0};
            for (const auto& link : wp.links) {
                s.links.push_back(static_cast<std::int32_t>(link.target));
                s.linkTypes.push_back(link.flags);
            }
            w.waypoints_.push_back(std::move(s));
        }
        // Each one's link flags (its own links' and those of links to it).
        for (std::size_t i = 0; i < waypoints->waypoints.size(); ++i) {
            for (const auto& link : waypoints->waypoints[i].links) {
                w.waypoints_[i].linkFlags |= link.flags;
                if (ServerWaypoint* to = w.waypoint(static_cast<std::int32_t>(link.target))) {
                    to->linkFlags |= link.flags;
                }
            }
        }
        w.registerWaypoints(); // (each into the map grid's lists as it's read)
    }
    // Objects, with their inventories (records following their owner).
    std::vector<std::uint32_t> placed; // (the top-level ones, in record order)
    if (const auto toc = optionalSection<map::ObjectTOC>(map)) {
        placed = placeRecords(w, *toc, map::decodeSection<map::ObjectData>(map), things, modifiers, gameFlags, frame, false);
    }
    // The floor (server_mapRWFloorMap: pair (x, y) is cell (x, y), its odd tile the
    // first, its even the second).
    w.floor_.assign(128 * 128, {});
    if (const auto floor = optionalSection<map::FloorMap>(map)) {
        const auto half = [&](const map::FloorTile& t) {
            FloorHalf h;
            h.floor = t.image;
            for (const auto& e : t.edges) {
                const things::EdgeDef* def = e.edge < things.edges.size() ? &things.edges[e.edge] : nullptr;
                h.edges.push_back({e.floor, def ? def->rows : std::uint8_t{0}, def ? def->cols : std::uint8_t{0}, e.direction});
            }
            return h;
        };
        w.anyFloor_ = !floor->tiles.empty();
        for (const auto& p : floor->tiles) {
            auto& cell = w.floor_[static_cast<std::size_t>(p.x & 0x7F) * 128 + (p.y & 0x7F)];
            if (p.odd) {
                cell[0] = half(*p.odd);
            }
            if (p.even) {
                cell[1] = half(*p.even);
            }
        }
    }
    // The floors a teleport can't reach (the names mapTileAllowTeleport looks for).
    w.noTeleportFloors_.assign(things.floors.size(), false);
    for (std::size_t i = 0; i < things.floors.size(); ++i) {
        const std::string& n = things.floors[i].name;
        w.noTeleportFloors_[i] = n == "WaterNoTeleport" || n == "WaterDeepNoTeleport" || n == "WaterShallowNoTeleport" ||
                                 n == "WaterSwampDeepNoTeleport" || n == "WaterSwampShallowNoTeleport";
    }
    // Walls, and which of them are secret, breakable or windows.
    if (const auto walls = optionalSection<map::WallMap>(map)) {
        // (Each made at the wall list's head; the direction's bit 7 is the wall's flag 0x80,
        // then its byte 0xC: server_mapRWWallMap.)
        for (const auto& wall : walls->walls) {
            WallCell& c = w.createWall(wall.x, wall.y);
            c.material = wall.material;
            c.direction = wall.direction & 0x7F;
            c.flag80 = c.flag80 || (wall.direction & 0x80) != 0;
            c.variant = wall.variant;
            c.extra = wall.unknown;
        }
    }
    // (The secret, breakable and window lists find their walls by Map_GetWallAtGrid: even cells.)
    // Each found has its id by a count of those found (then)
    // and goes at the head of its list (a cell named twice: once here, its record the last).
    if (const auto secret = optionalSection<map::SecretWalls>(map)) {
        std::uint16_t id = 0;
        for (const auto& s : secret->walls) {
            if (WallCell* c = w.gridWall(s.pos.x, s.pos.y)) {
                if (!c->secret) {
                    w.secretWalls_.insert(w.secretWalls_.begin(), {s.pos.x, s.pos.y});
                }
                c->wallId = id++;
                c->secret = true;
                c->secretFlags = s.flags;
                c->secretWait = s.openWait;
                c->secretState = s.state;
                c->secretOpen = s.openDelay; // (how far open)
                c->secretTimer = static_cast<std::int32_t>(s.lastOpen);
                c->secretSeen = s.unknown;
                if (s.state == 0) { // (an older map's: open if it starts so, else shut)
                    const bool open = (s.flags & 8) != 0;
                    c->secretState = open ? 3 : 1;
                    c->secretOpen = open ? 23 : 0;
                    c->secretSeen = open ? 0xFFFFFFFFu : 0;
                }
            }
        }
    }
    if (const auto breakable = optionalSection<map::DestructableWalls>(map)) {
        std::uint16_t id = 0;
        for (const auto& p : breakable->walls) {
            if (WallCell* c = w.gridWall(p.x, p.y)) {
                c->destructible = true;
                c->wallId = id++; // (MSG_DESTROY_WALL's index)
                w.breakableWalls_.insert(w.breakableWalls_.begin(), {p.x, p.y});
            }
        }
    }
    if (const auto windows = optionalSection<map::WindowWalls>(map)) {
        for (const auto& p : windows->walls) {
            if (WallCell* c = w.gridWall(p.x, p.y)) {
                c->window = true;
            }
        }
    }
    // Each door's leaf takes the cell half its shut swing reaches (XFerDoor ->
    // doorAttachWall, which takes the wall there or makes a blank one), as each record is read:
    // in the file's order, so of two doors on one cell the later record's is its leaf.
    for (const std::uint32_t e : placed) {
        w.attachDoorWall(w.objects_.at(e));
    }
    for (const auto& def : things.walls) {
        w.wallDefFlags_.push_back(def.flags);
        w.wallDefHealth_.push_back(def.health);
        w.wallDefVariations_.push_back(def.randomVariations);
        w.wallDefNames_.push_back(def.name);
    }
    // A breakable wall starts at its material's health (mapWallGetHpByTile).
    for (auto& [cell, c] : w.walls_) {
        if (c.destructible) {
            c.health = w.wallHealth(c.material);
        }
    }
    if (const auto polys = optionalSection<map::Polygons>(map)) {
        // The polygons (server_mapRWPolygons): their corners by id, truncated; their box; group; scripts.
        std::map<std::uint32_t, std::pair<int, int>> corners;
        for (const auto& p : polys->points) {
            corners[p.id] = {static_cast<int>(p.x), static_cast<int>(p.y)};
        }
        for (const auto& poly : polys->polygons) {
            MapPolygon mp;
            for (const std::uint32_t id : poly.points) {
                if (const auto it = corners.find(id); it != corners.end()) {
                    mp.points.push_back(it->second);
                }
            }
            if (!mp.points.empty()) {
                mp.minX = mp.maxX = mp.points[0].first;
                mp.minY = mp.maxY = mp.points[0].second;
                for (const auto& [x, y] : mp.points) {
                    mp.minX = std::min(mp.minX, x);
                    mp.maxX = std::max(mp.maxX, x);
                    mp.minY = std::min(mp.minY, y);
                    mp.maxY = std::max(mp.maxY, y);
                }
            }
            mp.group = poly.minimapGroup;
            mp.onEnter = ServerObject::ScriptBlock{poly.playerEnter.unknown, poly.playerEnter.function};
            mp.onLeave = ServerObject::ScriptBlock{poly.monsterEnter.unknown, poly.monsterEnter.function};
            mp.flags = poly.flags;
            w.polygons_.push_back(std::move(mp));
        }
    }
    if (const auto groups = optionalSection<map::GroupData>(map)) {
        for (const auto& g : groups->groups) {
            // (server_mapLoadAddGroup: the name in a 0x4C buffer, its last byte 0. Its
            // members each linked at the head of its list: kept in that order, the
            // file's last first.)
            ServerGroup sg;
            sg.id = g.id;
            sg.name = g.name.substr(0, std::min(g.name.find('\0'), std::size_t{75}));
            sg.kind = static_cast<ServerGroup::Kind>(g.kind);
            sg.members.assign(g.members.rbegin(), g.members.rend());
            for (auto p = g.walls.rbegin(); p != g.walls.rend(); ++p) {
                sg.walls.emplace_back(p->x, p->y);
            }
            w.groups_.push_back(std::move(sg));
        }
    }
    // Each elevator and its shaft linked, and each teleport pad to its partner, as the parse's end
    // does before the new things' pass.
    linkElevators(w, placed);
    linkTransporters(w, placed);
    // The load's new things' pass (serverParseEntireMap's unitsNewAddToList):
    // each made in the grid and the lists, newest first.
    queueNewObjects(w);
    return w;
}

void World::linkElevators(World& w, const std::vector<std::uint32_t>& placed)
{
    // (serverParseEntireMap, after the objects, before unitsNewAddToList) The new things, newest
    // first (createAt puts each at the new list's head): each elevator takes the first shaft whose
    // map number (its Extent as the record gave it) is the one its record named, and the shaft
    // takes the elevator. A shaft's own record's number isn't read; one no elevator names, and an
    // elevator that finds none, have no partner. Ours are extents kept here, so that a number the
    // load gave twice (a save's thing brought from another map, with that map's number, listed
    // ahead of the map's own) doesn't send either to the other thing.
    std::vector<ServerObject*> shafts;
    std::vector<ServerObject*> elevators;
    for (auto it = placed.rbegin(); it != placed.rend(); ++it) {
        if (ServerObject* o = w.byExtent(*it)) {
            if (o->classes & things::ObjectClass::ElevatorShaft) {
                shafts.push_back(o);
            } else if (o->classes & things::ObjectClass::Elevator) {
                elevators.push_back(o);
            }
        }
    }
    for (ServerObject* s : shafts) {
        s->elevatorLink = 0;
    }
    for (ServerObject* e : elevators) {
        const std::uint32_t named = e->elevatorLink; // (the record's: the shaft's map number)
        e->elevatorLink = 0;
        for (ServerObject* s : shafts) {
            if (s->fileExtent == named) {
                e->elevatorLink = s->extent;
                s->elevatorLink = e->extent;
                break;
            }
        }
    }
}

void World::linkTransporters(World& w, const std::vector<std::uint32_t>& placed)
{
    // (serverParseEntireMap's same pass, its other half) Each new teleport pad, on its own: its
    // partner none, then (when its record named one) the first new pad, newest first, itself
    // included, whose map number is the one named. One way only: the partner's own link is its
    // own record's. Ours is the partner's extent, so a number the load gave twice can't send it
    // to another thing.
    std::vector<ServerObject*> pads;
    for (auto it = placed.rbegin(); it != placed.rend(); ++it) {
        if (ServerObject* o = w.byExtent(*it); o && (o->classes & things::ObjectClass::Transporter) && o->transport) {
            pads.push_back(o);
        }
    }
    std::vector<std::uint32_t> named(pads.size());
    for (std::size_t i = 0; i < pads.size(); ++i) {
        named[i] = pads[i]->transport->link; // (the record's: the partner's map number)
    }
    for (std::size_t i = 0; i < pads.size(); ++i) {
        pads[i]->transport->link = 0;
        if (named[i] == 0) {
            continue;
        }
        for (const ServerObject* p : pads) {
            if (p->fileExtent == named[i]) {
                pads[i]->transport->link = p->extent;
                break;
            }
        }
    }
}

ServerObject* World::byExtent(std::uint32_t extent)
{
    if (extent < ExtentTable::kLimit) {
        ServerObject* o = objects_.index().at(extent);
        return o && !o->gone() ? o : nullptr;
    }
    const auto it = objects_.find(extent);
    return it != objects_.end() && !it->second.gone() ? &it->second : nullptr;
}

void World::noteRenumbered(const ServerObject& o)
{
    if (o.fileExtent != 0 && o.fileExtent != o.extent) {
        renumbered_[o.fileExtent].push_back(o.extent);
    }
}

void World::setMapNumber(ServerObject& o, std::uint32_t number)
{
    o.fileExtent = number;
    noteRenumbered(o);
}

ServerObject* World::byMapNumber(std::uint32_t number)
{
    // (netGetUnitByExtent) The object list from its head: the first not destroyed (0x20) whose
    // Extent it is, so of two things on the map with one number the newest (each pass puts its
    // things at the head): a thing put down later with a map object's number (brought from another
    // map, a .plr's, dropped by its holder; a save lists such things first) takes the map object's
    // place, OB-412. Ours, by decision (VD-47): the one on the list longest, the map's own thing:
    // the oldest (born); of one load's, the later record (a save lists the newer things first).
    // The candidates: the thing under that extent (of a load, the first record of the number),
    // then those numbered afresh as they came.
    if (number == 0) {
        return nullptr;
    }
    const auto listed = [&](ServerObject* o) {
        if (!o || o->fileExtent != number || o->pendingDelete || o->holder != 0 || (o->classes & things::ObjectClass::Missile) || o->fresh) {
            return false;
        }
        // (one put back in the world this frame, still on the new list: Items::place)
        return o->born != frameNow || std::ranges::find(fresh_, o->extent) == fresh_.end();
    };
    ServerObject* best = byExtent(number);
    if (!listed(best)) {
        best = nullptr;
    }
    if (const auto it = renumbered_.find(number); it != renumbered_.end()) {
        for (const std::uint32_t e : it->second) {
            ServerObject* o = byExtent(e);
            if (o != best && listed(o) && (!best || o->born <= best->born)) {
                best = o;
            }
        }
    }
    return best;
}

ServerObject* World::byPacketCode(std::uint32_t code)
{
    // (packetDynamicUnitCode: bit 15 a map object's, netGetUnitByExtent's NetCode; then
    // Obj_GetByNetCode)
    if ((code & 0x8000u) == 0x8000u) {
        return byMapNumber(code & 0x7FFFu);
    }
    return byNetCode(code);
}

ServerObject* World::byNetCode(std::uint32_t code)
{
    // (Obj_GetByNetCode: none destroyed. A map object's NetCode is its own, under 0x8000: a code
    // with bit 15 is nobody's.)
    if (code == 0 || (code & 0x8000u) != 0) {
        return nullptr;
    }
    return byExtent(code);
}

ServerObject* World::byScriptId(std::int32_t id)
{
    for (auto& [extent, o] : objects_) {
        if (!o.gone() && !o.pendingDelete && o.scriptId == id) {
            return &o;
        }
    }
    return nullptr;
}

ServerObject* World::byScriptName(std::string_view name)
{
    if (name.empty()) {
        return nullptr;
    }
    // A name without ':' is matched against what follows an object's own "Map:" (with
    // one, the whole name).
    const bool bare = name.find(':') == std::string_view::npos;
    const auto named = [&](const ServerObject& o) {
        std::string_view own = o.scriptName;
        if (const auto colon = own.find(':'); bare && colon != std::string_view::npos) {
            own = own.substr(colon + 1);
        }
        return !o.scriptName.empty() && own == name;
    };
    for (auto& [extent, o] : objects_) {
        // (No 0x20 test: a deleted thing on the map is found; one deleted from an inventory was
        // taken out of it at once by delayedDeleteObject's Obj_RemoveFromInventory
        // and is in no list.)
        const bool deleted = o.gone() || o.pendingDelete;
        if (named(o) && (!deleted || (o.holder == 0 && (o.flags & ObjFlag::Active)))) {
            return &o;
        }
    }
    for (Dying& d : dying_) {
        if (d.named && named(d.node.mapped())) {
            return &d.node.mapped();
        }
    }
    return nullptr;
}

void World::settleHurtBy(ServerObject& o)
{
    // (updateUnits) Its Obj130 DESTROYED (0x20: ours' pendingDelete; or freed): its
    // ObjOwner if that one isn't, else none. A missile's hit leaves the missile there
    // (DefaultDamage: obj3, else the source), so the next frame's update sees the shooter.
    if (o.hurtBy == 0) {
        return;
    }
    const ServerObject* by = nullptr;
    if (const auto it = objects_.find(o.hurtBy); it != objects_.end()) {
        if (!it->second.pendingDelete && !it->second.gone()) {
            return;
        }
        by = &it->second;
    } else {
        by = dying(o.hurtBy);
    }
    std::uint32_t to = 0;
    if (by && by->owner != 0) {
        const auto oi = objects_.find(by->owner);
        if (oi != objects_.end() && !oi->second.pendingDelete && !oi->second.gone()) {
            to = by->owner;
        }
    }
    o.hurtBy = to;
}

ServerObject* World::dying(std::uint32_t extent)
{
    for (Dying& d : dying_) {
        if (d.node.key() == extent) {
            return &d.node.mapped();
        }
    }
    return nullptr;
}

WallCell* World::wall(int x, int y)
{
    if (WallTable::inside(x, y)) {
        return walls_.index().at(x, y);
    }
    const auto it = walls_.find({x, y});
    return it != walls_.end() ? &it->second : nullptr;
}

WallCell& World::createWall(int x, int y)
{
    const auto [it, made] = walls_.try_emplace({x, y});
    if (made) {
        it->second.serial = nextWallSerial_++;
    }
    return it->second;
}

std::optional<std::pair<int, int>> World::doorCell(std::uint32_t door) const
{
    for (const auto& [cell, w] : walls_) {
        if (w.door == door) {
            return cell;
        }
    }
    return std::nullopt;
}

WallCell* World::gridWall(int x, int y)
{
    // Map_GetWallAtGrid: only cells whose x + y is even hold walls for it.
    if (((x + y) & 1) != 0) {
        return nullptr;
    }
    WallCell* c = wall(x, y);
    return c && !c->broken && c->door == 0 ? c : nullptr;
}

int World::wallShape(int x, int y, std::uint8_t mode) const
{
    if (x < 0 || x >= 256 || y < 0 || y >= 256) {
        return -1;
    }
    const WallCell* cell = walls_.index().at(x, y);
    if (!cell || cell->broken) { // (wall passes over broken ones)
        return -1;
    }
    const WallCell& c = *cell;
    if (c.door == 0) {
        // A window only when they count (mode 0x40); a secret wall until it's half open.
        if (((mode & 0x40) || !c.window) && (!c.secret || c.secretOpen < 12)) {
            return c.direction;
        }
        return -1;
    }
    // A door's leaf (mode 0x10): only while it's shut, and a gate's (subclass 4) only for mode
    // 0x80; its shape is the diagonal its swing lies along.
    if (!(mode & 0x10)) {
        return -1;
    }
    const ServerObject* leaf = nullptr;
    if (c.door < ExtentTable::kLimit) {
        leaf = objects_.index().at(c.door);
    } else if (const auto d = objects_.find(c.door); d != objects_.end()) {
        leaf = &d->second;
    }
    if (!leaf || leaf->gone()) {
        return -1;
    }
    const ServerObject& door = *leaf;
    if (((mode & 0x80) || !(door.subclasses & 4)) && door.doorDirection == door.doorClosed) {
        const auto [dx, dy] = kDoorSwing[door.doorDirection & 31];
        if ((dx > 0 && dy > 0) || (dx < 0 && dy < 0)) {
            return 1;
        }
        if ((dx < 0 && dy > 0) || (dx > 0 && dy < 0)) {
            return 0;
        }
    }
    return -1;
}

int World::rayCell(int x, int y, const float ray[4], float hit[4], std::uint8_t flags) const
{
    // Off the 256x256 cell grid, or no wall in the cell (wallShape's -1, whatever the flags): none.
    if (x < 0 || x >= 256 || y < 0 || y >= 256 || !walls_.index().at(x, y)) {
        return 0;
    }
    // With NearWindows, windows within 60 of the ray's start (3600 = 60 squared) don't block.
    // (All on the stack, at 53 bits.)
    if (flags & Trace::NearWindows) {
        using util::X87;
        const X87 cx = X87::fromDouble(x) * kCell + 11.5f - ray[0];
        const X87 cy = X87::fromDouble(y) * kCell + 11.5f - ray[1];
        if (cx * cx + cy * cy < 3600.0f) {
            flags |= Trace::ThroughWindows;
        }
    }
    // wallShape's mode: windows count (0x40) unless ThroughWindows is set; shut doors (0x10) with Doors.
    std::uint8_t mode = static_cast<std::uint8_t>((~flags & 4) << 4);
    if (flags & Trace::Doors) {
        mode |= 0x10;
    }
    const int shape = wallShape(x, y, mode);
    if (shape < 0 || shape > 10) {
        return 0;
    }
    // The wall itself: without Doors, only an even cell's, and not a door's (Map_GetWallAtGrid).
    const WallCell& c = *walls_.index().at(x, y);
    if (!(flags & Trace::Doors) && (((x + y) & 1) != 0 || c.door != 0)) {
        return 0;
    }
    // The material's WALL flags (bit 1 passes rays; with SightOnly a wall needs bit 0 to block).
    const std::uint32_t defFlags = c.material < wallDefFlags_.size() ? wallDefFlags_[c.material] : 0;
    // With PassSecret, a secret wall whose record has flag 2 does not block.
    if ((flags & Trace::PassSecret) && c.secret && (c.secretFlags & 2)) {
        return 0;
    }
    if ((defFlags & 2) || ((flags & Trace::SightOnly) && !(defFlags & 1))) {
        return 0;
    }
    const float minX = std::min(ray[0], ray[2]);
    const float maxX = std::max(ray[0], ray[2]);
    const float minY = std::min(ray[1], ray[3]);
    const float maxY = std::max(ray[1], ray[3]);
    const float ox = static_cast<float>(x * 23);  // the cell's top left corner in world units
    const float oy = static_cast<float>(y * 23);
    const auto within = [&](const ShapeSegment& s, std::pair<float, float> p) {
        return s.present && util::X87(ox) + s.from <= p.first && p.first <= util::X87(ox) + s.to && minX <= p.first &&
               p.first <= maxX &&
               minY <= p.second && p.second <= maxY;
    };
    const auto& [back, slash] = kShapes[shape];
    // Where the ray's line meets the cell's \ diagonal (from its top left corner) and its / one
    // (from its top right); a meeting counts when it's on the shape's segment and within the ray.
    int n = 0;
    std::pair<float, float> a{};
    std::pair<float, float> b{};
    if (shape != 0) {
        a = meetBackslash(ray, ox, oy);
    }
    if (shape != 1) {
        b = meetSlash(ray, ox + 23, oy);
    }
    if (within(back, a)) {
        hit[0] = a.first;
        hit[1] = a.second;
        n = 1;
    }
    if (within(slash, b)) {
        hit[n * 2] = b.first;
        hit[n * 2 + 1] = b.second;
        ++n;
    }
    // Both crossed: the one nearer the ray's start first (the squares on the stack).
    if (n == 2) {
        const auto d2 = [&](std::pair<float, float> p) {
            const util::X87 ex = util::X87(ray[0]) - p.first;
            const util::X87 ey = util::X87(ray[1]) - p.second;
            return ex * ex + ey * ey;
        };
        if (d2(b) < d2(a)) {
            std::swap(hit[0], hit[2]);
            std::swap(hit[1], hit[3]);
        }
    }
    return n;
}

bool World::traceRay(float x0, float y0, float x1, float y1, std::uint8_t flags, RayHit* out) const
{
    const float ray[4] = {x0, y0, x1, y1};
    float hit[4] = {};
    const auto blocked = [&](int cx, int cy) {
        if (out) {
            *out = {hit[0], hit[1], cx, cy};
        }
        return false;
    };
    // The cells the ends are in (Math_FloatToInt of the chopped product: FISTP, which truncates,
    // the game running the FPU in chop mode: mainloop sets _controlfp(_RC_CHOP, _MCW_RC) each pass).
    using util::X87;
    int cx = wallCellOf(x0);
    int cy = wallCellOf(y0);
    const int ex = wallCellOf(x1);
    const int ey = wallCellOf(y1);
    if (cx < 0 || cx >= 256 || cy < 0 || cy >= 256) {
        return false; // (blocked, with nowhere hit)
    }
    if (cx == ex) { // a column: down it a cell at a time
        const int step = y0 <= y1 ? 1 : -1;
        for (; cy != ey + step; cy += step) {
            if (rayCell(cx, cy, ray, hit, flags)) {
                return blocked(cx, cy);
            }
        }
        return true;
    }
    if (cy == ey) { // a row
        const int step = x0 <= x1 ? 1 : -1;
        for (; cx != ex + step; cx += step) {
            if (rayCell(cx, cy, ray, hit, flags)) {
                return blocked(cx, cy);
            }
        }
        return true;
    }
    // Otherwise band by band along one axis (columns when it's steeper than 45 degrees, else
    // rows), every cell the line spans in the band with one more each side.
    // (The game's x87: dy's difference divides on the stack after its store; cross's product and
    // quotient stay on the stack. The walk's position, the band's line ends before their
    // clamping, and every quotient by 23 stay on the stack too; see wholeCells.)
    const float dx = (X87(x1) - x0).f();
    const X87 dyRaw = X87(y1) - y0;
    const float dy = dyRaw.f();
    const float slope = (dyRaw / dx).f();
    const float cross = (X87(y0) - X87(dy) * x0 / dx).f();
    const auto range = [](X87 from, float to, float dir) {
        const int first = static_cast<int>(wholeCells(from) - dir);
        const int last = static_cast<int>(dir + wholeCells(to));
        return std::tuple{first, last, last <= first ? -1 : 1};
    };
    // A band's edges: 23 x its whole cells (stored to a float) and that plus 23.
    const auto edges = [](double whole, float dir) {
        const float base = (X87(kCell) * X87::fromDouble(whole)).f();
        const float far = (X87(base) + kCell).f();
        return dir <= 0 ? std::pair{far, base} : std::pair{base, far};
    };
    if (std::fabs(dx) <= std::fabs(dy)) {
        const float lo = std::min(x0, x1);
        const float hi = x1 <= x0 ? x0 : x1;
        const float sx = x1 <= x0 ? -1.0f : 1.0f;
        const float sy = y1 <= y0 ? -1.0f : 1.0f;
        double lastWhole = std::numeric_limits<double>::quiet_NaN();
        for (X87 x = x0; x <= hi && lo <= x;) {
            const double whole = wholeCells(x);
            if (whole == lastWhole) {
                // (noxcpp's guard: heading below 0 from column 0, modf's chop gives column 0 again
                // and the exe's walk never ends (an original bug); the cells left are off
                // the map, with no walls: clear.)
                return true;
            }
            lastWhole = whole;
            const auto [enter, leave] = edges(whole, sx);
            const int column = static_cast<int>(whole);
            // The rows the line spans across this column, kept within the ray's own ends.
            X87 ya = X87(enter) * slope + cross;
            if ((sy > 0 && ya < y0) || (sy < 0 && X87(y0) < ya)) {
                ya = y0;
            }
            float yb = (X87(leave) * slope + cross).f();
            if ((sy > 0 && y1 < yb) || (sy < 0 && yb < y1)) {
                yb = y1;
            }
            const auto [first, last, step] = range(ya, yb, sy);
            for (int row = first; row != last + step; row += step) {
                if (rayCell(column, row, ray, hit, flags)) {
                    return blocked(column, row);
                }
            }
            // On to the next column (a tenth past this one's far edge).
            x = X87(sx) * 0.1f + leave;
        }
        return true;
    }
    // Shallower than 45 degrees: the same, row by row.
    const float lo = std::min(y0, y1);
    const float hi = y1 <= y0 ? y0 : y1;
    const float sy = y1 <= y0 ? -1.0f : 1.0f;
    const float sx = x1 <= x0 ? -1.0f : 1.0f;
    double lastWhole = std::numeric_limits<double>::quiet_NaN();
    for (X87 y = y0; y <= hi && lo <= y;) {
        const double whole = wholeCells(y);
        if (whole == lastWhole) {
            return true; // (noxcpp's guard, as the columns' above: row 0 heading below 0)
        }
        lastWhole = whole;
        const auto [enter, leave] = edges(whole, sy);
        const int row = static_cast<int>(whole);
        X87 xa = (X87(enter) - cross) / slope;
        if ((sx > 0 && xa < x0) || (sx < 0 && X87(x0) < xa)) {
            xa = x0;
        }
        float xb = ((X87(leave) - cross) / slope).f();
        if ((sx > 0 && x1 < xb) || (sx < 0 && xb < x1)) {
            xb = x1;
        }
        const auto [first, last, step] = range(xa, xb, sx);
        for (int column = first; column != last + step; column += step) {
            if (rayCell(column, row, ray, hit, flags)) {
                return blocked(column, row);
            }
        }
        y = X87(sy) * 0.1f + leave;
    }
    return true;
}

void World::stepSecretWalls()
{
    constexpr std::int32_t kFps = 30; // (gameFPS)
    for (const auto& [x, y] : secretWalls_) { // (the secret list, its head first)
        WallCell* cell = wall(x, y);
        if (!cell) {
            continue;
        }
        WallCell& c = *cell;
        // Starting to move by itself, its material's OPEN / CLOSE sound (else SecretWallOpen /
        // SecretWallClose: wallFindOpenSound / wallFindCloseSound) at the cell's middle in
        // whole units, (x * 23 + 11, y * 23 + 11) (audCreate).
        const auto sound = [&](bool opening) {
            const things::WallDef* def = thingDb && c.material < thingDb->walls.size() ? &thingDb->walls[c.material] : nullptr;
            std::string_view name = opening ? "SecretWallOpen" : "SecretWallClose";
            if (def && !(opening ? def->openSound : def->closeSound).empty()) {
                name = opening ? def->openSound : def->closeSound;
            }
            playSoundAt(*this, name, static_cast<float>(x * 23 + 11), static_cast<float>(y * 23 + 11));
        };
        bool moving = false;
        switch (c.secretState) {
        case 1: // shut: one that opens by itself counts down
            if ((c.secretFlags & 4) && (c.secretFlags & 8) && --c.secretTimer == 0) {
                c.secretState = 4;
                sound(true);
            }
            break;
        case 2:  // closing: a unit a frame; once shut, its wait begins
            if (--c.secretOpen == 0) {
                c.secretState = 1;
                c.secretTimer = static_cast<std::int32_t>(c.secretWait) * kFps;
            }
            moving = true;
            break;
        case 3: // open: one that shuts by itself counts down
            if ((c.secretFlags & 4) && !(c.secretFlags & 8) && --c.secretTimer == 0) {
                c.secretState = 2;
                sound(false);
            }
            break;
        case 4:  // opening: a unit a frame to 23 (fully open)
            if (++c.secretOpen == 23) {
                c.secretState = 3;
                c.secretTimer = static_cast<std::int32_t>(c.secretWait) * kFps;
            }
            moving = true;
            break;
        }
        // Moving (it was shutting or opening as the frame began), what's within 42.5 of its middle
        // joins the motion (getUnitsInRect ->
        // unitHasCollideOrUpdateFn): pushed out of it as it shuts. (The exe's flag
        // for this is left over from the record before for a state outside 1-4: none occur.)
        if (moving) {
            const float cx = static_cast<float>(x * 23) + 11.5f;
            const float cy = static_cast<float>(y * 23) + 11.5f;
            constexpr float kReach = 42.5f;
            // (By the grid: they join in its order.)
            unitsInRect(cx - kReach, cy - kReach, cx + kReach, cy + kReach, [&](ServerObject& o) {
                if (!o.gone()) {
                    queueMotion(*this, o);
                }
            });
        }
    }
}

ServerWaypoint* World::waypoint(std::int32_t id)
{
    const auto it = std::ranges::find(waypoints_, id, &ServerWaypoint::id);
    return it != waypoints_.end() ? &*it : nullptr;
}

ServerWaypoint* World::waypointNamed(std::string_view name)
{
    const auto it = std::ranges::find(waypoints_, name, &ServerWaypoint::name);
    return it != waypoints_.end() ? &*it : nullptr;
}

bool World::namesMatch(std::string_view own, std::string_view name)
{
    // each cut at its first ':'; both scoped, the scopes alike but for case
    // (in the "C" locale); then what follows, exactly.
    const auto split = [](std::string_view s) -> std::pair<std::optional<std::string_view>, std::string_view> {
        const auto c = s.find(':');
        return c == std::string_view::npos ? std::pair{std::optional<std::string_view>{}, s} : std::pair{std::optional{s.substr(0, c)}, s.substr(c + 1)};
    };
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 0x20) : c; };
    const auto [scope, rest] = split(name.substr(0, name.find('\0'))); // (C strings)
    const auto [oScope, oRest] = split(own.substr(0, own.find('\0')));
    if (scope && oScope && !std::ranges::equal(*scope, *oScope, {}, lower, lower)) {
        return false;
    }
    return rest == oRest;
}

ServerWaypoint* World::waypointMatching(std::string_view name)
{
    // the waypoint list from its head, in the map's order (the map's read,
    // server_mapRWWaypoints, links each at the head of a holding list
    // (waypointNewNotMap), which waypoint then moves, its head
    // first, each to the head of the list: the map's first at the head).
    for (ServerWaypoint& w : waypoints_) {
        if (namesMatch(w.name, name)) {
            return &w;
        }
    }
    return nullptr;
}

std::optional<ServerGroup::Kind> World::groupKind(const ServerGroup& g)
{
    // a group of groups (3) has the kind of the group its first member (the list's
    // head) names, looked further down while that's one too. (The exe reads a missing group's or
    // an empty list's: an original bug; ours none. A loop of groups: the exe's never ends; ours none.)
    const ServerGroup* at = &g;
    for (std::size_t steps = 0; at && steps <= groups_.size(); ++steps) {
        if (at->kind != ServerGroup::Kind::Unknown3) {
            return at->kind;
        }
        if (at->members.empty()) {
            return std::nullopt;
        }
        at = group(at->members.front());
    }
    return std::nullopt;
}

ServerGroup* World::groupMatching(std::string_view name, ServerGroup::Kind kind)
{
    // the group list from its head, the newest first (server_mapLoadAddGroup
    // links each at the head), the first of that kind whose name matches
    // (namesMatch).
    for (auto it = groups_.rbegin(); it != groups_.rend(); ++it) {
        if (groupKind(*it) == kind && namesMatch(it->name, name)) {
            return &*it;
        }
    }
    return nullptr;
}

ServerGroup* World::group(std::int32_t id)
{
    // server_scriptGetGroup: the list from its head, the newest first.
    const auto it = std::ranges::find(groups_.rbegin(), groups_.rend(), id, &ServerGroup::id);
    return it != groups_.rend() ? &*it : nullptr;
}

ServerGroup* World::groupNamed(std::string_view name)
{
    const auto it = std::ranges::find(groups_, name, &ServerGroup::name);
    return it != groups_.end() ? &*it : nullptr;
}

namespace {

/// The shape of an edge at its direction: 0-11.
int edgeShape(int rows, int cols, int direction)
{
    if (rows == 3 && cols == 3) {
        return direction;
    }
    if (direction == 0) {
        return 0;
    }
    if (direction <= rows - 2) {
        return 1;
    }
    if (direction == rows - 1) {
        return 2;
    }
    const int side = (rows - 4) + cols * 2;
    if (direction < side) {
        return (((direction ^ rows) & 1) != 0) + 3;
    }
    if (direction == side) {
        return 5;
    }
    if (direction <= (cols + rows) * 2 - 6) {
        return 6;
    }
    if (direction == (cols + rows) * 2 - 5) {
        return 7;
    }
    return direction + ((6 - cols) - rows) * 2;
}

/// Whether the point (lx, ly) of a tile's 46 square is under an edge of `shape`.
bool underEdge(int lx, int ly, int shape)
{
    bool onOrAbove = false; // lx >= ly
    bool onOrBelow = false; // lx <= ly
    bool anti = false;      // lx >= 46 - ly
    bool antiLess = false;  // lx <= 46 - ly
    // Which side of the \ diagonal (lx == ly) the point is on...
    if (lx == ly) {
        onOrAbove = onOrBelow = true;
    } else if (lx < ly) {
        onOrBelow = true;
    } else {
        onOrAbove = true;
    }
    // ...and which side of the / diagonal (lx == 46 - ly); 0x2E is 46.
    if (lx == 0x2E - ly) {
        anti = antiLess = true;
    } else if (lx < 0x2E - ly) {
        antiLess = true;
    } else {
        anti = true;
    }
    // Each shape's edge is the half (or the union of halves) of those diagonals it covers, as the exe's switch tests them.
    bool b = onOrBelow;
    switch (shape) {
    case 0:
        if (!b) {
            return false;
        }
        [[fallthrough]];
    case 3:
        b = !anti;
        break;
    case 1:
        b = !b;
        break;
    case 2:
        if (!b) {
            return false;
        }
        [[fallthrough]];
    case 4:
        b = !antiLess;
        break;
    case 5:
        if (!anti) {
            return false;
        }
        [[fallthrough]];
    case 6:
        b = !onOrAbove;
        break;
    case 7:
        if (!onOrAbove) {
            return false;
        }
        b = !antiLess;
        break;
    case 8:
        if (b) {
            return true;
        }
        b = !anti;
        break;
    case 9:
        return b || antiLess;
    case 10:
        return onOrAbove || antiLess;
    case 11:
        if (onOrAbove) {
            return true;
        }
        b = !anti;
        break;
    default:
        return false;
    }
    return !b;
}

} // namespace

int World::floorAt(float x, float y) const
{
    if (floor_.empty()) {
        return -1;
    }
    // (1/46: 11.5; Math_FloatToInt chops. The sum and product on the stack, stored as its float
    // argument.)
    using util::X87;
    constexpr float kInvTile = std::bit_cast<float>(0x3CB21643u); // (1 / 46)
    const int cx = util::floatToInt((X87(x) + 11.5f) * kInvTile);
    const int cy = util::floatToInt((X87(y) + 11.5f) * kInvTile);
    int lx = util::floatToInt(X87(x) + 11.5f) % 0x2E;
    int ly = util::floatToInt(X87(y) + 11.5f) % 0x2E;
    if (cx <= 1 || 0x7E < cx || cy <= 1 || 0x7E < cy) {
        return -1;
    }
    const FloorHalf* h = nullptr;
    // The quarter of the cell the point is in picks the tile; (lx, ly) moves into that tile's own
    // 46 square.
    if (ly < lx) {
        if (ly < 0x2E - lx) { // the top quarter: the cell above's even tile
            h = &floor_[static_cast<std::size_t>(cx) * 128 + (cy - 1)][1];
            ly += 0x17;
        } else { // the right: its odd
            h = &floor_[static_cast<std::size_t>(cx) * 128 + cy][0];
            lx -= 0x17;
        }
    } else if (ly < 0x2E - lx) { // the left: the cell to the left's odd
        h = &floor_[static_cast<std::size_t>(cx - 1) * 128 + cy][0];
        lx += 0x17;
    } else { // the bottom: its even
        h = &floor_[static_cast<std::size_t>(cx) * 128 + cy][1];
        ly -= 0x17;
    }
    int found = h->floor;
    for (const FloorEdgeCell& e : h->edges) { // (the last edge over it wins)
        if (underEdge(lx, ly, edgeShape(e.rows, e.cols, e.direction))) {
            found = e.floor;
        }
    }
    return found;
}

std::uint8_t World::wallHealth(int material) const
{
    return 0 <= material && static_cast<std::size_t>(material) < wallDefHealth_.size() ? wallDefHealth_[static_cast<std::size_t>(material)] : 0;
}

std::uint8_t World::wallVariations(int material) const
{
    return 0 <= material && static_cast<std::size_t>(material) < wallDefVariations_.size() ? wallDefVariations_[static_cast<std::size_t>(material)]
                                                                                           : 0;
}

int World::wallMaterial(std::string_view name) const
{
    // wallTileByName.
    for (std::size_t i = 0; i < wallDefNames_.size(); ++i) {
        if (wallDefNames_[i] == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

World::Line World::line(std::string_view label) const
{
    if (!strings) {
        return {strings::fromUtf8(label), {}};
    }
    if (const strings::StringEntry* e = strings->find(label); e && !e->variants.empty()) {
        // Str_GetString: the entry's count under 2 its first, else
        // randInt(0, count - 1); the text and the speech file the same one's.
        std::size_t i = 0;
        if (2 <= e->variants.size()) {
            i = static_cast<std::size_t>(nox::util::uniform_int_distribution<int>(0, static_cast<int>(e->variants.size()) - 1)(stringRng));
        }
        return {e->variants[i].text, e->variants[i].extra};
    }
    // (Str_GetString: L"MISSING:'%S'" of the qualified label.)
    return {u"MISSING:'" + strings::fromUtf8(label) + u"'", {}};
}

void World::textTo(const ServerObject& player, std::string_view label, std::optional<std::u16string_view> arg)
{
    if (!player.player) {
        return;
    }
    std::u16string text = this->text(label);
    if (arg) {
        // (the exe's wide sprintf: its one %s / %S / %ls the argument, or %d a number given as
        // text)
        for (std::size_t i = 0; i + 1 < text.size(); ++i) {
            if (text[i] != u'%') {
                continue;
            }
            std::size_t end = i + 1;
            if (text[end] == u'l' && end + 1 < text.size()) {
                ++end;
            }
            if (text[end] == u's' || text[end] == u'S' || text[end] == u'd' || text[end] == u'i') {
                text.replace(i, end + 1 - i, *arg);
                break;
            }
        }
    }
    broadcasts_.push_back({textMessage(text, 0), true, player.extent});
}

std::vector<std::uint8_t> World::textMessage(std::u16string_view text, std::uint8_t flags)
{
    // At most 254 characters, so the length byte (with the end) holds it: a longer one would wrap
    // it and every client would misread the rest of the packet (the exe overflows its stack at 256
    // and crashes; VD in docs/vanilla_deviations.md).
    text = text.substr(0, std::min<std::size_t>(text.size(), kMaxTextMessage));
    // MSG_TEXT_MESSAGE 0xA8 from no one: narrow (flag 2) when every character fits a byte
    // (cliCanTalkMB's test), else wide (4); the length with its end.
    const bool narrow = std::ranges::all_of(text, [](char16_t c) { return c < 0x100; });
    // Layout: 0xA8, u16 net code, u8 flags, u16 x, u16 y, u8 length (with its end), u16 duration,
    // then the text (unitSay fills in the speaker's fields).
    std::vector<std::uint8_t> m = {0xA8, 0, 0, static_cast<std::uint8_t>(flags | (narrow ? 2 : 4)), 0, 0, 0, 0,
                                   static_cast<std::uint8_t>(text.size() + 1), 0, 0};
    util::appendText(m, text, narrow);
    return m;
}

void World::textToAll(std::u16string_view text, std::uint8_t flags)
{
    // SrvNet_BroadcastTextMessage.
    broadcasts_.push_back({textMessage(text, flags), true, 0});
}

void World::unitSay(const ServerObject& unit, std::u16string_view text, std::uint16_t duration)
{
    // As textMessage, from the unit: its net code, its place (chopped to whole units), the
    // duration after the count.
    std::vector<std::uint8_t> m = textMessage(text, 0);
    const std::uint16_t code = netCodeOf(unit);
    const auto x = static_cast<std::uint16_t>(static_cast<int>(unit.x));
    const auto y = static_cast<std::uint16_t>(static_cast<int>(unit.y));
    m[1] = static_cast<std::uint8_t>(code);
    m[2] = static_cast<std::uint8_t>(code >> 8);
    m[4] = static_cast<std::uint8_t>(x);
    m[5] = static_cast<std::uint8_t>(x >> 8);
    m[6] = static_cast<std::uint8_t>(y);
    m[7] = static_cast<std::uint8_t>(y >> 8);
    m[9] = static_cast<std::uint8_t>(duration);
    m[10] = static_cast<std::uint8_t>(duration >> 8);
    broadcasts_.push_back({std::move(m), true, 0});
}

void World::chatKill(const ServerObject* unit)
{
    const std::uint16_t code = unit ? netCodeOf(*unit) : std::uint16_t{0xDEAD};
    broadcasts_.push_back({{0xCA, static_cast<std::uint8_t>(code), static_cast<std::uint8_t>(code >> 8)}, true, 0});
}

std::uint32_t World::polygonAt(int x, int y, std::uint32_t current, bool scripted)
{
    const auto inside = [&](const MapPolygon& p) {
        if (scripted && p.onEnter.function.empty() && p.onLeave.function.empty()) {
            return false;
        }
        if (!(p.minX <= x && x <= p.maxX && p.minY <= y && y <= p.maxY) || p.points.empty()) { // (pointInRect)
            return false;
        }
        // The crossings of a ray from the point to a world corner (map/polygon_hit.hpp); odd, inside.
        return map::polygonHit(p.points, x, y, polygonRay_);
    };
    if (current != 0 && current != kPolygonUnknown && current <= polygons_.size() && inside(polygons_[current - 1])) {
        return current;
    }
    for (std::uint32_t i = 1; i <= polygons_.size(); ++i) {
        if (i != current && inside(polygons_[i - 1])) {
            return i;
        }
    }
    return 0;
}

bool World::noTeleportAt(float x, float y) const
{
    const int f = floorAt(x, y);
    return 0 <= f && static_cast<std::size_t>(f) < noTeleportFloors_.size() && noTeleportFloors_[static_cast<std::size_t>(f)];
}

std::uint32_t World::newExtent()
{
    // Above the map's (from 0x4000: past every stock map's, so the players kept from map to map
    // never meet the next map's), fresh while there are any under 0x8000 (the net codes' room),
    // then the one freed longest ago.
    // (Never one of the kept things' while a map change holds them: reservedExtents.)
    const auto reserved = [&](std::uint32_t e) { return std::ranges::find(reservedExtents, e) != reservedExtents.end(); };
    for (;;) {
        std::uint32_t e = 0;
        if (nextExtent_ <= 0x7FFFu || freedExtents_.empty()) {
            e = nextExtent_++;
        } else {
            e = freedExtents_.front();
            freedExtents_.pop_front();
        }
        if (!reserved(e)) {
            return e;
        }
    }
}

void World::freeExtent(std::uint32_t extent)
{
    if (kDynamicExtents <= extent && extent <= 0x7FFFu && !objects_.contains(extent)) {
        freedExtents_.push_back(extent);
    }
}

void World::adopt(ServerObject o, bool asNew)
{
    if (objects_.contains(o.extent)) {
        throw std::runtime_error("a kept thing's extent is taken by the new map's");
    }
    nextExtent_ = std::max(nextExtent_, o.extent + 1);
    const std::uint32_t e = o.extent;
    noteRenumbered(o); // (a map's thing brought from another, a .plr's: by its map number too)
    ServerObject& placed = objects_.emplace(e, std::move(o)).first->second;
    if (asNew) {
        noteNew(placed);
    }
}

ServerObject& World::spawn(const things::ThingDb& things, std::string_view type, float x, float y)
{
    ServerObject o;
    o.extent = newExtent();
    o.scriptId = nextScriptId_++;
    describe(o, things, type, *this);
    o.x = o.frameStartX = o.newX = x; // (NewPos with it: nothing moves it till it's moved)
    o.y = o.frameStartY = o.newY = y;
    o.lastX = x; // (PrevPos too: createAt sets all three)
    o.lastY = y;
    // Made this frame, its timer from now (createAt's, as every thing the game makes): a caller
    // that sets its own keeps it. (Left at 0, a OneSecondDieUpdate thing went at its first update:
    // the meteor's explosion was never seen.)
    o.born = o.timer = frameNow;
    auto& placed = objects_[o.extent];
    placed = std::move(o);
    noteNew(placed);
    if (placed.classes & (things::ObjectClass::Monster | things::ObjectClass::Player)) {
        postCreateNotify(*this, placed); // (createAt's unitPostCreateNotify: its friendships, owed)
    }
    return placed;
}

MapGrid::Rect World::collideBoxAt(const ServerObject& o, float x, float y)
{
    // (as objectUnkUpdateCoords about PosVec) A box's from its corners
    // (its left, top, right and bottom). Each side one add, stored (chopped).
    using util::X87;
    switch (o.shape) {
    case ShapeKind::Center:
        return {x, y, x, y};
    case ShapeKind::Circle:
        return {(X87(x) - o.radius).f(), (X87(y) - o.radius).f(), (X87(o.radius) + x).f(), (X87(o.radius) + y).f()};
    case ShapeKind::Box: {
        // (A box extent lies turned 45 degrees: its corners from the shape's, boxCorners.)
        const BoxCorners c = boxCorners(o.boxWidth, o.boxHeight);
        return {(X87(c.c1x) + x).f(), (X87(c.c0y) + y).f(), (X87(c.c2x) + x).f(), (X87(c.c3y) + y).f()};
    }
    default:
        return {};
    }
}

void World::gridLink(ServerObject& o, float x, float y, const MapGrid::Rect* missileBox)
{
    // (not DESTROYED 0x20, not PARTITIONED 0x200, ACTIVE: on the map)
    if (o.pendingDelete || o.gone() || o.holder != 0 || grid_.linked(o)) {
        return;
    }
    const bool missile = (o.classes & things::ObjectClass::Missile) != 0;
    grid_.link(o, missile && missileBox ? *missileBox : collideBoxAt(o, x, y), x, y, missile);
}

void World::gridRelink(ServerObject& o, float x, float y, const MapGrid::Rect* missileBox)
{
    grid_.unlink(o);
    if (!MapGrid::inside(x, y)) {
        deleteObject(*this, o); // (delayedDeleteObject: its lasting spells cancelled too)
        return;
    }
    gridLink(o, x, y, missileBox);
}

void World::addUpdatable(ServerObject& o)
{
    if (!(o.classes & things::ObjectClass::Missile)) {
        grid_.addUpdatable(o);
    }
}

std::vector<std::uint32_t> World::takeNew()
{
    std::vector<std::uint32_t> out = std::move(fresh_);
    fresh_.clear();
    std::reverse(out.begin(), out.end());
    return out;
}

ServerObject* World::createAt(const things::ThingDb& things, std::string_view type, float x, float y, std::uint32_t frame)
{
    if (!things.findThing(type)) {
        return nullptr;
    }
    ServerObject& o = spawn(things, type, x, y);
    o.newX = o.lastX = o.homeX = x;
    o.newY = o.lastY = o.homeY = y;
    o.flags = (o.flags & 0x35E9FEDBu) | ObjFlag::Active;
    o.born = o.timer = frame;
    if (network() && !quest() && (o.typeName == "Gold" || (o.classes & 0x3001110u)) && !(o.classes & things::ObjectClass::Missile)) {
        o.flags |= ObjFlag::NoCollide;
    }
    return &o;
}

void World::sweepDeleted(std::minstd_rand* dropRng, const std::vector<std::uint32_t>* marked0, bool markedAll)
{
    // (What the caller gathered holds only while freeing last tick's holders marks nothing more.)
    if (!freeing_.empty()) {
        marked0 = nullptr;
    }
    // The last sweep's are freed now (at this tick's end: DeletedAt an earlier frame):
    // off the grid and the missiles' list (servFinalizeDelObject), their extents
    // given back.
    for (Dying& d : dying_) {
        grid_.release(d.node.mapped());
    }
    dying_.clear();
    for (const std::uint32_t e : std::exchange(lingeringExtents_, {})) {
        freedExtents_.push_back(e);
    }
    // What a holder deleted last tick still held (as it's freed now): it drops about
    // where the holder was (dropAllItems: Obj_DropAt, by its DROP; the random
    // numbers here, after this tick's others), what it keeps (a creature's clothing, unitDropCheckSome)
    // freed with it, and what that holds (objectFreeMem: no word to anyone). (A
    // thing deleted itself left its holder's inventory at once: deleteObject.)
    // Whether `i` is still held by `holder` and not deleted itself.
    const auto heldBy = [&](std::uint32_t i, std::uint32_t holder) {
        const auto it = objects_.find(i);
        return it != objects_.end() && !it->second.pendingDelete && it->second.holder == holder;
    };
    if (!freeing_.empty()) {
        static const things::ThingDb kNoThings{};
        std::minstd_rand own{1};
        Items items{*this, thingDb ? *thingDb : kNoThings, modifierDb, dropRng ? *dropRng : own, frameNow};
        // Marks a thing and everything nested in it deleted (freed with its holder).
        const auto freeWith = [&](std::uint32_t first) {
            std::vector<std::uint32_t> todo{first};
            while (!todo.empty()) {
                const std::uint32_t i = todo.back();
                todo.pop_back();
                ServerObject& o = objects_.at(i);
                o.pendingDelete = true;
                for (const std::uint32_t e : o.inventory) {
                    if (heldBy(e, i)) {
                        todo.push_back(e);
                    }
                }
            }
        };
        for (ServerObject& h : std::exchange(freeing_, {})) {
            std::erase_if(h.inventory, [&](std::uint32_t i) { return !heldBy(i, h.extent); });
            if (!h.inventory.empty()) {
                items.dropAll(h);
            }
            for (const std::uint32_t i : h.inventory) {
                if (heldBy(i, h.extent)) {
                    freeWith(i);
                }
            }
        }
    }
    std::vector<std::uint32_t> marked; // (deleted themselves)
    if (marked0) {
        marked = *marked0;
    } else {
        for (const auto& [extent, o] : objects_) {
            if (o.pendingDelete) {
                marked.push_back(extent);
            }
        }
    }
    if (marked.empty()) {
        return; // (nothing deleted this frame: each pass below starts from a deleted one)
    }
    // What a deleted one holds stays held by it till the exe frees it, at the next tick's
    // (DeletedAt this frame): found there by name and script id meanwhile (its
    // inventory is walked), then dropped (above).
    for (const std::uint32_t e : marked) {
        const ServerObject& o = objects_.at(e);
        if (std::ranges::any_of(o.inventory, [&](std::uint32_t i) { return heldBy(i, e); })) {
            freeing_.push_back(o);
        }
    }
    // Every one marked now, in the extents' order: the list itself when it's all of them (gathered
    // here, or the caller's with nothing marked since); else gathered. (Nothing below marks more:
    // the unwatching, the owners' changes.)
    std::vector<std::uint32_t> deleted;
    if (!marked0 || markedAll) {
        deleted = marked;
    } else {
        for (const auto& [extent, o] : objects_) {
            if (o.pendingDelete) {
                deleted.push_back(extent);
            }
        }
    }
    // (delayedDeleteObject) A player's watched creature it doesn't control (a pet)
    // is unwatched first (monsterRemoveMonitors).
    for (const std::uint32_t e : deleted) {
        ServerObject& o = objects_.at(e);
        if (!(o.classes & things::ObjectClass::Monster) || !(o.subclasses & 0x80u) || o.owner == 0) {
            continue;
        }
        const auto oi = objects_.find(o.owner);
        if (oi != objects_.end() && (oi->second.classes & things::ObjectClass::Player) && !controls(*this, oi->second, o)) {
            removeMonitors(*this, oi->second, o);
        }
    }
    // (servFinalizeDelObject, as each deleted object is freed)
    // What it owns passes to its owner (unitTransferSlaves), and it's nobody's
    // (unitClearOwner: a creature its player controls is lost to it, off its
    // minimap). (The exe frees them the frame after, its deleted list newest first; ours at the
    // frame's end in the world's order.)
    // Each one's owner as it was: the exe's lets go of it only as it frees it (at the
    // next sweep), so what reads a deleted thing's owner meanwhile (the units update's Obj130
    // fix-up, the owner chains of unitsHaveSameTeam) sees it; ours' lists are let
    // go of now, the field put back after.
    std::vector<std::uint32_t> ownerWas;
    if (!deleted.empty()) {
        std::ranges::sort(deleted);
        ownerWas.reserve(deleted.size());
        for (const std::uint32_t e : deleted) {
            ownerWas.push_back(objects_.at(e).owner);
        }
        // Each one's list (transferSlaves' walk, gathered for all of them in one pass).
        std::vector<std::pair<std::uint32_t, ServerObject*>> slaves; // (its owner, it)
        for (auto& [extent, o] : objects_) {
            if (o.owner != 0 && o.owner != extent && std::ranges::binary_search(deleted, o.owner)) {
                slaves.emplace_back(o.owner, &o);
            }
        }
        std::ranges::sort(slaves, [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first < b.first
                   : a.second->ownedOrder != b.second->ownedOrder ? a.second->ownedOrder > b.second->ownedOrder
                                                                  : a.second->extent > b.second->extent;
        });
        // The deleted ones and the things they own, both sorted by owner: walked side by side.
        auto next = slaves.begin();
        for (const std::uint32_t e : deleted) {
            ServerObject& o = objects_.at(e);
            ServerObject* to = o.owner ? byExtent(o.owner) : nullptr;
            for (; next != slaves.end() && next->first < e; ++next) {
            }
            for (; next != slaves.end() && next->first == e; ++next) {
                if (next->second->owner == e) { // (not let go of already: one deleted before it)
                    setOwner(*this, to, *next->second);
                }
            }
            clearOwner(*this, o);
        }
        for (std::size_t i = 0; i < deleted.size(); ++i) {
            objects_.at(deleted[i]).owner = ownerWas[i];
        }
    }
    // The named ones still in the exe's lists stay findable by name till the next sweep: one on
    // the map (ACTIVE), or held by one there and not deleted itself (its holder's inventory is
    // walked; dropped only as the holder is freed).
    std::vector<std::uint32_t> named;
    for (const std::uint32_t e : deleted) {
        const ServerObject& o = objects_.at(e);
        if (o.scriptName.empty()) {
            continue;
        }
        bool listed = o.holder == 0 && (o.flags & ObjFlag::Active);
        if (!listed && o.holder != 0 && !std::ranges::binary_search(marked, e)) {
            const auto h = objects_.find(o.holder);
            listed = h != objects_.end() && h->second.holder == 0 && (h->second.flags & ObjFlag::Active);
        }
        if (listed) {
            named.push_back(e);
        }
    }
    for (const std::uint32_t e : deleted) { // (in the world's order: the ones before already out)
        const auto it = objects_.find(e);
        if (const std::uint32_t g = it->second.spawnedBy; g != 0) {
            // (from delayedDeleteObject: its generator one short.)
            const auto gi = objects_.find(g);
            if (gi != objects_.end() && gi->second.generator) {
                --gi->second.generator->active;
            }
            it->second.spawnedBy = 0;
        }
        if (it->second.holder == 0) {
            removed_.push_back(it->first);
        }
        if (ServerObject* h = it->second.holder ? byExtent(it->second.holder) : nullptr) {
            std::erase(h->inventory, it->first);
        }
        if (kDynamicExtents <= it->first && it->first <= 0x7FFFu) {
            lingeringExtents_.push_back(it->first);
        }
        // (at this tick's end, DeletedAt this frame: off the updatable list,
        // unitRemoveFromUpdatable. The exe frees it at the next tick's end, its
        // links in the grid and the missiles' list kept meanwhile: the searches of the next
        // frame find it, several of their callbacks not testing 0x20. So ours keeps it, out of
        // the objects, till the next sweep.)
        grid_.removeUpdatable(it->second);
        const bool listed = std::ranges::binary_search(named, it->first);
        dying_.push_back(Dying{objects_.extract(it), listed});
    }
}

} // namespace nox::server
