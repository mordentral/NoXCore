// The map scripts' creature-control and object-state builtins (the ones the
// campaign maps call). Each is GAME.EXE's function from its builtin table, read from the
// exe; the AI ones push onto the monster's action stack (monsters.hpp) as the exe's do.
// The exe's slips are kept, each marked as an original bug.
#include "noxcore/server/script_host.hpp"

#include "noxcore/server/combat.hpp"
#include "noxcore/server/enchant.hpp"
#include "noxcore/server/host_client.hpp"
#include "noxcore/server/items.hpp"
#include "noxcore/server/monsters.hpp"
#include "noxcore/server/objects.hpp"
#include "noxcore/server/physics.hpp"
#include "noxcore/server/player.hpp"
#include "noxcore/server/quest.hpp"
#include "noxcore/server/script_names.hpp"
#include "noxcore/server/sight.hpp"
#include "noxcore/server/solo.hpp"
#include "noxcore/server/spells.hpp"
#include "noxcore/server/teleport.hpp"
#include "noxcore/server/trade.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/things/spell_ids.hpp"
#include "noxcore/util/x87.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <string_view>
#include <vector>

namespace nox::server {

namespace {

namespace oc = things::ObjectClass;
namespace ms = things::MonsterStatus;
using Item = ServerObject::Monster::Item;

/// The builtins here, by their ids in the table (script/opcodes.cpp's names).
enum CreatureBuiltin : std::uint32_t {
    LookAtDirection = 13, GroupLookAtDirection = 14,
    Wander = 29, GroupWander = 30,
    GoBackHome = 33,
    Damage = 51, GroupDamage = 52,
    AwardSpell = 55, GroupAwardSpell = 56, EnchantBuiltin = 57, GroupEnchant = 58,
    Raise = 69, LookWithAngle = 70, PushObjectTo = 71, PushObject = 72, GetLastItem = 73, GetPreviousItem = 74,
    HasItem = 75, Pickup = 77, Drop = 78, HasClass = 79,
    HasEnchant = 81, EnchantOff = 82, CurrentHealth = 83, MaxHealth = 84, RestoreHealth = 85,
    IsVisibleTo = 87,
    LookAtObject = 96, Walk = 97, GroupWalk = 98,
    IsOwnedBy = 105, GroupIsOwnedBy = 106, IsOwnedByAny = 107, GroupIsOwnedByAny = 108,
    CastSpellObjectObject = 131, CastSpellObjectLocation = 132, CastSpellLocationObject = 133, CastSpellLocationLocation = 134,
    GetElevatorStatus = 138, CreatureGuard = 139, CreatureGroupGuard = 140, CreatureHunt = 141, CreatureGroupHunt = 142,
    CreatureIdle = 143, CreatureGroupIdle = 144, CreatureFollow = 145, CreatureGroupFollow = 146,
    AggressionLevel = 147, GroupAggressionLevel = 148, HitLocation = 149, GroupHitLocation = 150,
    HitFarLocation = 151, GroupHitFarLocation = 152, SetRoamFlag = 153, GroupSetRoamFlag = 154,
    AttackBuiltin = 155, GroupAttack = 156,
    RetreatLevel = 160, GroupRetreatLevel = 161, ResumeLevel = 162, GroupResumeLevel = 163, RunAway = 164, GroupRunAway = 165,
    PauseObject = 166, GroupPauseObject = 167, IsAttackedBy = 168, GetGold = 169, ChangeGold = 170,
    HasSubclass = 173,
    SetHalberd = 186, Frozen = 188, DeleteObjectTimer = 191,
    IsSummoned = 197, ZombieStayDown = 198, ZombieGroupStayDown = 199, RaiseZombie = 200, RaiseZombieGroup = 201,
};

std::uint32_t bitsOf(float f) { return std::bit_cast<std::uint32_t>(f); }

bool isMonster(const ServerObject& o) { return (o.classes & oc::Monster) != 0 && o.monster.has_value(); }
bool aliveMonster(const ServerObject& o) { return isMonster(o) && !(o.flags & ObjFlag::Dead); }

/// server_scriptExecuteFnForEachGroupObj with kind 0: an object group's members by
/// map number (netGetUnitByExtent: World::byMapNumber), a group of groups (kind 3) each of its
/// groups in turn; any other kind nothing. (A
/// group met again on the way: the exe recurses till its stack runs out; ours goes no further,
/// as script_host.cpp's walk.)
template <class F>
void eachObjectOf(World& world, std::int32_t group, F&& fn, std::vector<std::int32_t>* walking = nullptr)
{
    const ServerGroup* g = world.group(group);
    if (!g) {
        return;
    }
    const std::vector<std::int32_t> members = g->members;
    if (g->kind == ServerGroup::Kind::Objects) {
        for (const std::int32_t m : members) {
            if (ServerObject* o = world.byMapNumber(static_cast<std::uint32_t>(m))) {
                fn(*o);
            }
        }
    } else if (g->kind == ServerGroup::Kind::Unknown3) {
        std::vector<std::int32_t> own;
        std::vector<std::int32_t>& path = walking ? *walking : own;
        if (std::ranges::find(path, group) != path.end()) {
            return;
        }
        path.push_back(group);
        for (const std::int32_t m : members) {
            eachObjectOf(world, m, fn, &path);
        }
        path.pop_back();
    }
}

/// The builtins that walk a group's list themselves (server_scriptGetGroup, then its
/// entries by map number, netGetUnitByExtent): its members whatever its kind.
template <class F>
void eachMemberOf(World& world, const ServerGroup& g, F&& fn)
{
    const std::vector<std::int32_t> members = g.members;
    for (const std::int32_t m : members) {
        if (ServerObject* o = world.byMapNumber(static_cast<std::uint32_t>(m))) {
            fn(*o);
        }
    }
}

/// unitHasThatParent: `parent` is `o` or up its owners (none: never).
bool hasParent(World& world, ServerObject* o, const ServerObject* parent)
{
    if (!o || !parent) {
        return false;
    }
    for (ServerObject* u = o; u; u = u->owner ? world.byExtent(u->owner) : nullptr) {
        if (u == parent) {
            return true;
        }
    }
    return false;
}

/// enchantByName: the ENCHANT_* name's number (-1 none; the exact name). (The names: enchant.hpp's
/// kEnchantNames, which the editor's script tools read too.)
int enchantByName(std::string_view name)
{
    for (std::size_t i = 0; i < std::size(kEnchantNames); ++i) {
        if (kEnchantNames[i] == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// HasClass's table (the CLASS names by bit) and HasSubclass's (32 names a class's table,
// searched table by table), compared exactly (strcmp): script_names.hpp's kScriptClassNames and
// scriptSubclassTables.

/// mathDirection4ToAngle's table: a 3 x 3 grid's way to a direction.
constexpr std::array<int, 9> kGridAngles{0xA0, 0xC0, 0xE0, 0x80, 0x00, 0x00, 0x60, 0x40, 0x20};

/// Direction1 alone set (the casts turning their caster): a monster's Direction2 stays what it
/// was, and the motion's end turns it back (updateUnits: Direction1 = Direction2).
void faceOnly(ServerObject& o, int direction)
{
    if (o.monster && o.monster->turnTo < 0) {
        o.monster->turnTo = o.direction;
    }
    o.direction = static_cast<std::uint16_t>(direction);
}

/// Items::drop's place for a forced drop (invForceDropItem: within 50 of the
/// holder).
std::pair<float, float> dropSpotNear(Fight& f, const ServerObject& holder)
{
    return clearPointNear(f.world, f.rng, holder.x, holder.y, 50.0f);
}

} // namespace

// ---- The shared pieces (monsters.hpp) ----

void freezeUnit(Fight& fight, ServerObject& o, int lock)
{
    // unitFreeze: NO_UPDATE; a player: the lock noted (the first
    // freezer's), MSG_REPORT_PLAYER_STATUS, standing (Plr_SetState 0xD), down to the floor
    // (unitRaise), the path grid's obstacles marked afresh at the next search, its summoned
    // creatures frozen; a live monster idles over what it did.
    World& world = fight.world;
    if (o.flags & ObjFlag::NoUpdate) {
        return;
    }
    o.flags |= ObjFlag::NoUpdate;
    if (o.classes & oc::Player) {
        if (world.freezeLock == 0) {
            world.freezeLock = static_cast<std::uint32_t>(lock);
        }
        reportPlayerStatus(world, o);
        if (o.player) {
            fight.setPlayerState(o, 0xD);
        }
        o.raise(0.0f);
        world.pathMarkedAt = 0;
        world.pathMarksStand = false;
        for (ServerObject* c : ownedCreatures(world, o)) {
            freezeUnit(fight, *c, lock);
        }
    }
    if ((o.classes & oc::Monster) && !(o.flags & ObjFlag::Dead)) {
        Monsters{fight}.push(o, MonsterAction::Idle);
    }
}

void unfreezeUnit(Fight& fight, ServerObject& o, int lock)
{
    // unitUnFreeze: a player stays frozen while another's lock holds and this one
    // has none (a dialog's end under a script's Frozen); else the lock cleared, NO_UPDATE off, its
    // client told, its summoned creatures thawed. A live monster's IDLE popped.
    World& world = fight.world;
    if (!(o.flags & ObjFlag::NoUpdate)) {
        return;
    }
    if (!(o.classes & oc::Player)) {
        o.flags &= ~ObjFlag::NoUpdate;
    } else {
        if (world.freezeLock != 0 && lock == 0) {
            return;
        }
        world.freezeLock = 0;
        o.flags &= ~ObjFlag::NoUpdate;
        reportPlayerStatus(world, o);
        for (ServerObject* c : ownedCreatures(world, o)) {
            unfreezeUnit(fight, *c, lock);
        }
    }
    if ((o.classes & oc::Monster) && !(o.flags & ObjFlag::Dead)) {
        Monsters{fight}.pop(o);
    }
}

void reportPlayerStatus(World& world, const ServerObject& unit)
{
    // SrvNet_SendReportPlayerStatus: MSG_REPORT_PLAYER_STATUS 0x66 {u32 its flags} to
    // its client, reliably.
    if (!unit.player) {
        return;
    }
    const std::uint32_t f = unit.flags;
    world.broadcasts().push_back({{0x66, static_cast<std::uint8_t>(f), static_cast<std::uint8_t>(f >> 8), static_cast<std::uint8_t>(f >> 16),
                                   static_cast<std::uint8_t>(f >> 24)},
                                  true,
                                  unit.extent});
}

// ---- The builtins ----

int ScriptHost::creatureBuiltin(std::uint32_t id, script::Vm& vm)
{
    Fight fight{world_, creatureRng_, frame_, &things_, modifiers_};
    Monsters ai{fight};
    const auto popObject = [&] { return object(vm.popInt()); };
    // A point moved by the prefab's offset: FILD, FADD, stored (chopped).
    const auto offsetX = [&](float x) { return (util::X87::fromDouble(vm.context().x) + x).f(); };
    const auto offsetY = [&](float y) { return (util::X87::fromDouble(vm.context().y) + y).f(); };
    const auto items = [&] { return Items{world_, things_, modifiers_, creatureRng_, frame_}; };
    const auto spells = [&] { return Spells{world_, things_, modifiers_, creatureRng_, frame_}; };

    // Most orders clear the monster's action stack first; a REPORT pushed under the action runs its
    // end-of-waypoint handler once the action is done.
    // The AI's orders (each as the exe's helper, also run for a group's members).
    const auto idle = [&](ServerObject& o) { // unitIdle
        if (aliveMonster(o)) {
            ai.clear(o);
            ai.push(o, MonsterAction::Idle);
        }
    };
    const auto hunt = [&](ServerObject& o) { // unitHunt
        if (aliveMonster(o)) {
            ai.clear(o);
            ai.push(o, MonsterAction::Hunt);
        }
    };
    const auto wander = [&](ServerObject& o) { // scriptMonsterRoam
        if (aliveMonster(o)) {
            ai.clear(o);
            if (Item* it = ai.push(o, MonsterAction::Report)) {
                it->args[0] = MonsterAction::Roam;
            }
            if (Item* it = ai.push(o, MonsterAction::Roam)) {
                it->args[0] = 0;
                it->args[2] = (it->args[2] & ~0xFFu) | o.monster->field333; // (its byte alone)
            }
        }
    };
    const auto lookAt = [&](ServerObject& o, int index) { // monsterLookAt
        // (The grid's index past 0-8 reads past the table in the exe: an original bug.)
        if (index < 0 || static_cast<int>(kGridAngles.size()) <= index) {
            return;
        }
        if (aliveMonster(o)) {
            const auto [dx, dy] = directionVector(kGridAngles[static_cast<std::size_t>(index)]);
            const float x = (util::X87(dx) * 10.0f + o.x).f(); // (on the x87, one store)
            const float y = (util::X87(dy) * 10.0f + o.y).f();
            if (Item* it = ai.push(o, MonsterAction::FaceLocation)) {
                it->args[0] = bitsOf(x);
                it->args[1] = bitsOf(y);
            }
        }
    };
    const auto walkTo = [&](ServerObject& o, float x, float y) { // monsterWalkTo
        if (aliveMonster(o)) {
            ai.clear(o);
            if (Item* it = ai.push(o, MonsterAction::Report)) {
                it->args[0] = MonsterAction::FarMoveTo;
            }
            if (Item* it = ai.push(o, MonsterAction::FarMoveTo)) {
                it->args[0] = bitsOf(x);
                it->args[1] = bitsOf(y);
                it->args[2] = 0;
            }
        }
    };
    const auto guard = [&](ServerObject& o, const std::array<float, 5>& a) { // monsterGoPatrol
        if (aliveMonster(o)) {
            ai.clear(o);
            if (Item* it = ai.push(o, MonsterAction::Guard)) {
                it->args[0] = bitsOf(a[0]);
                it->args[1] = bitsOf(a[1]);
                // (Each difference stored: chopped.)
                it->args[2] = static_cast<std::uint32_t>(directionOf((util::X87(a[2]) - a[0]).f(), (util::X87(a[3]) - a[1]).f()));
            }
            o.monster->sightRange = a[4];
        }
    };
    const auto follow = [&](ServerObject& o, ServerObject* leader) { // unitSetFollow
        if (leader && aliveMonster(o) && &o != leader) {
            ai.clear(o);
            if (Item* it = ai.push(o, MonsterAction::Escort)) {
                it->args[0] = bitsOf(leader->x);
                it->args[1] = bitsOf(leader->y);
                it->args[2] = leader->extent;
            }
        }
    };
    const auto attack = [&](ServerObject& o, ServerObject* target) { // mobSetFightTarg
        if (target && aliveMonster(o) && &o != target) {
            ai.clear(o);
            o.monster->focus = target->extent;
            world_.monstersLookAt = frame_ + 1;              // (frameCounterSetCopyToNextFrame)
            if (Item* it = ai.push(o, MonsterAction::Report)) {
                it->args[0] = MonsterAction::Fight;
            }
            ai.pushFight(o, *target, frame_);
        }
    };
    const auto hitLocation = [&](ServerObject& o, float x, float y) { // monsterActionMelee
        if (aliveMonster(o) && monsterCanMelee(o)) {
            ai.clear(o);
            if (Item* it = ai.push(o, MonsterAction::Report)) {
                it->args[0] = MonsterAction::MeleeAttack;
            }
            ai.push(o, MonsterAction::MeleeAttack);
            if (Item* it = ai.push(o, Dependency::LocationFartherThan)) {
                // (0x33, DEPENDENCY_LOCATION_FARTHER_THAN: the walk holds while it's farther than its
                // reach. monster.bin's melee range; plus its radius)
                const float reach = o.monster->def ? o.monster->def->meleeRange : 0.0f;
                it->args[0] = bitsOf(reach + o.radius);
                it->args[2] = bitsOf(x);
                it->args[3] = bitsOf(y);
            }
            if (Item* it = ai.push(o, MonsterAction::MoveTo)) {
                it->args[0] = bitsOf(x);
                it->args[1] = bitsOf(y);
                it->args[2] = 0;
            }
        }
    };
    const auto hitFarLocation = [&](ServerObject& o, float x, float y) { // monsterMissileAttack
        if (aliveMonster(o) && monsterCanShoot(o)) {
            ai.clear(o);
            if (Item* it = ai.push(o, MonsterAction::Report)) {
                it->args[0] = MonsterAction::MissileAttack;
            }
            if (Item* it = ai.push(o, MonsterAction::MissileAttack)) {
                it->args[0] = bitsOf(x);
                it->args[1] = bitsOf(y);
                it->args[2] = 0;
            }
        }
    };
    const auto runAway = [&](ServerObject& o, const ServerObject& from, std::int32_t frames) { // server_scriptFleeFrom
        if (aliveMonster(o) && !Monsters::has(o, MonsterAction::Flee)) {
            if (Item* it = ai.push(o, MonsterAction::Report)) {
                it->args[0] = MonsterAction::Flee;
            }
            if (Item* it = ai.push(o, Dependency::Time)) {
                it->args[0] = static_cast<std::uint32_t>(frames) + frame_;
            }
            if (Item* it = ai.push(o, MonsterAction::Flee)) {
                it->args[0] = bitsOf(from.x);
                it->args[1] = bitsOf(from.y);
                it->args[2] = 0;
            }
        }
    };
    const auto pause = [&](ServerObject& o, std::int32_t frames) {
        if (aliveMonster(o)) {
            if (Item* it = ai.push(o, MonsterAction::Report)) {
                it->args[0] = MonsterAction::Wait;
            }
            if (Item* it = ai.push(o, MonsterAction::Wait)) {
                it->args[0] = static_cast<std::uint32_t>(frames) + frame_;
            }
        }
    };
    const auto damage = [&](ServerObject& t, ServerObject* source, std::int32_t amount, std::int32_t type) {
        if (0 < amount) {
            fight.damage(t, fight.parentPlayer(source), source ? source->extent : 0, amount, type);
        }
    };
    const auto enchantFor = [&](ServerObject& o, int e, float seconds) {
        // (gameFPS x the seconds, chopped (__ftol), as a short; at power 5)
        const auto frames = util::ftol(util::X87::fromDouble(30.0) * seconds);
        enchant(world_, o, e, static_cast<std::int16_t>(frames), 5);
    };
    const auto grant = [&](ServerObject& o, std::uint32_t spell, bool show) { // Spell_GrantToPlayer(o, spell, 1, show, 0)
        return grantSpell(world_, things_, o, static_cast<int>(spell), true, show, 0);
    };
    const auto zombieStayDown = [&](ServerObject& o) {
        if (isMonster(o)) {
            o.monster->status |= ms::StayDead;
        }
    };
    const auto raiseZombie = [&](ServerObject& o) {
        if (isMonster(o) && (o.flags & ObjFlag::Dead)) {
            o.monster->status &= ~ms::StayDead;
            ai.raiseZombie(o);
        }
    };
    const auto spellNamed = [&](std::uint32_t name) -> std::uint32_t { // spellNameToN (0: none)
        const auto s = things::spellId(vm.string(name));
        return s ? *s : 0u;
    };
    const auto imaginaryCaster = [&]() -> ServerObject* {
        // imagCasterUnit: the exe's ImaginaryCaster, made at each map's load at
        // (2944, 2944) (Server::loadMapKeeping: World::imaginaryCaster). A world made without that
        // load (a test's) gets one as it's first wanted.
        if (ServerObject* c = world_.imaginaryCaster ? world_.byExtent(world_.imaginaryCaster) : nullptr) {
            return c;
        }
        ServerObject* c = world_.createAt(things_, "ImaginaryCaster", 2944.0f, 2944.0f, frame_);
        world_.imaginaryCaster = c ? c->extent : 0;
        return c;
    };

    switch (id) {
    case Frozen: { // Frozen(object, on): 1 freezes it (unitFreeze(o, 1)), else thaws
        const std::int32_t on = vm.popInt();
        if (ServerObject* o = popObject()) {
            if (on == 1) {
                freezeUnit(fight, *o, 1);
            } else {
                unfreezeUnit(fight, *o, 1);
            }
        }
        return 0;
    }
    case LookAtObject: { // LookAtObject(object, target): both its facings at it
        ServerObject* target = popObject();
        ServerObject* o = popObject();
        if (o && target) {
            const int d = directionOf(target->x - o->x, target->y - o->y);
            o->direction = static_cast<std::uint16_t>(d);
            if (o->monster) {
                o->monster->turnTo = d; // (Direction2)
            }
        }
        return 0;
    }
    case LookWithAngle: { // LookWithAngle(object, angle): both facings, the angle mod 256
        auto a = vm.popInt();
        if (ServerObject* o = popObject()) {
            // Wrap the angle into 0-255 (a negative one is first brought up by a multiple of 256).
            if (a < 0) {
                a = static_cast<std::int32_t>((static_cast<std::uint32_t>(0xFF - a) & 0xFFFFFF00u) + static_cast<std::uint32_t>(a));
            }
            if (0xFF < a) {
                a = a - (a >> 8) * 0x100;
            }
            o->direction = static_cast<std::uint16_t>(a);
            if (o->monster) {
                o->monster->turnTo = static_cast<std::uint16_t>(a);
            }
        }
        return 0;
    }
    case LookAtDirection: { // LookAtDirection(monster, grid way): FACE_LOCATION pushed
        const std::int32_t way = vm.popInt();
        if (ServerObject* o = popObject()) {
            lookAt(*o, way);
        }
        return 0;
    }
    case GroupLookAtDirection: {
        const std::int32_t way = vm.popInt();
        if (const ServerGroup* g = world_.group(vm.popInt())) {
            eachMemberOf(world_, *g, [&](ServerObject& o) { lookAt(o, way); });
        }
        return 0;
    }
    case AggressionLevel: { // both its aggression numbers
        const float v = vm.popFloat();
        if (ServerObject* o = popObject(); o && (o->classes & oc::Monster) && o->monster) {
            o->monster->aggression = o->monster->aggression2 = v;
        }
        return 0;
    }
    case GroupAggressionLevel: {
        const float v = vm.popFloat();
        eachObjectOf(world_, vm.popInt(), [&](ServerObject& o) {
            if (isMonster(o)) {
                o.monster->aggression = o.monster->aggression2 = v;
            }
        });
        return 0;
    }
    case RetreatLevel:
    case ResumeLevel: {
        const float v = vm.popFloat();
        if (ServerObject* o = popObject(); o && isMonster(*o)) {
            (id == RetreatLevel ? o->monster->retreatLevel : o->monster->resumeLevel) = v;
        }
        return 0;
    }
    case GroupRetreatLevel:
    case GroupResumeLevel: {
        const float v = vm.popFloat();
        eachObjectOf(world_, vm.popInt(), [&](ServerObject& o) {
            if (isMonster(o)) {
                (id == GroupRetreatLevel ? o.monster->retreatLevel : o.monster->resumeLevel) = v;
            }
        });
        return 0;
    }
    case SetRoamFlag: { // (script_SetRoamFlag) its roaming flags (a byte)
        const auto v = static_cast<std::uint8_t>(vm.pop());
        if (ServerObject* o = popObject(); o && isMonster(*o)) {
            o->monster->field333 = v;
        }
        return 0;
    }
    case GroupSetRoamFlag: {
        const auto v = static_cast<std::uint8_t>(vm.pop());
        eachObjectOf(world_, vm.popInt(), [&](ServerObject& o) {
            if (isMonster(o)) {
                o.monster->field333 = v;
            }
        });
        return 0;
    }
    case CreatureIdle:
        if (ServerObject* o = popObject()) {
            idle(*o);
        }
        return 0;
    case CreatureGroupIdle:
        eachObjectOf(world_, vm.popInt(), idle);
        return 0;
    case CreatureHunt:
        if (ServerObject* o = popObject()) {
            hunt(*o);
        }
        return 0;
    case CreatureGroupHunt:
        eachObjectOf(world_, vm.popInt(), hunt);
        return 0;
    case Wander:
        if (ServerObject* o = popObject()) {
            wander(*o);
        }
        return 0;
    case GroupWander:
        eachObjectOf(world_, vm.popInt(), wander);
        return 0;
    case GoBackHome: { // REPORT, facing where its home facing points from where it is now, home
        ServerObject* o = popObject();
        if (!o || !aliveMonster(*o)) {
            return 0;
        }
        auto& m = *o->monster;
        ai.clear(*o);
        if (Item* it = ai.push(*o, MonsterAction::Report)) {
            it->args[0] = MonsterAction::MoveToHome;
        }
        if (Item* it = ai.push(*o, MonsterAction::FaceLocation)) {
            // (From its position now, not its home: an original bug.)
            const auto [dx, dy] = directionVector(m.homeDirection);
            it->args[0] = bitsOf((util::X87(dx) * 10.0f + o->x).f()); // (on the x87, one store)
            it->args[1] = bitsOf((util::X87(dy) * 10.0f + o->y).f());
        }
        if (Item* it = ai.push(*o, MonsterAction::MoveToHome)) {
            it->args[0] = bitsOf(m.homeX);
            it->args[1] = bitsOf(m.homeY);
            it->args[2] = 0;
        }
        return 0;
    }
    case CreatureGuard:        // CreatureGuard(monster, x, y, toX, toY, sight)
    case CreatureGroupGuard: {
        std::array<float, 5> a{};
        for (int i = 4; 0 <= i; --i) {
            a[static_cast<std::size_t>(i)] = vm.popFloat();
        }
        if (id == CreatureGuard) {
            if (ServerObject* o = popObject()) {
                guard(*o, a);
            }
        } else {
            eachObjectOf(world_, vm.popInt(), [&](ServerObject& o) { guard(o, a); });
        }
        return 0;
    }
    case CreatureFollow: { // CreatureFollow(monster, leader): ESCORT
        ServerObject* leader = popObject();
        ServerObject* o = popObject();
        if (o && leader) {
            follow(*o, leader);
        }
        return 0;
    }
    case CreatureGroupFollow: {
        ServerObject* leader = popObject();
        const std::int32_t group = vm.popInt();
        if (leader) {
            eachObjectOf(world_, group, [&](ServerObject& o) { follow(o, leader); });
        }
        return 0;
    }
    case AttackBuiltin: { // Attack(monster, target): FIGHT it
        ServerObject* target = popObject();
        ServerObject* o = popObject();
        if (o) {
            attack(*o, target);
        }
        return 0;
    }
    case GroupAttack: {
        ServerObject* target = popObject();
        eachObjectOf(world_, vm.popInt(), [&](ServerObject& o) { attack(o, target); });
        return 0;
    }
    case HitLocation:        // HitLocation(monster, x, y): walk up and strike there
    case HitFarLocation: {   // HitFarLocation(monster, x, y): shoot there
        const float y = vm.popFloat();
        const float x = vm.popFloat();
        if (ServerObject* o = popObject()) {
            if (id == HitLocation) {
                hitLocation(*o, x, y);
            } else {
                hitFarLocation(*o, x, y);
            }
        }
        return 0;
    }
    case GroupHitLocation:
    case GroupHitFarLocation: {
        const float y = vm.popFloat();
        const float x = vm.popFloat();
        eachObjectOf(world_, vm.popInt(), [&](ServerObject& o) {
            if (id == GroupHitLocation) {
                hitLocation(o, x, y);
            } else {
                hitFarLocation(o, x, y);
            }
        });
        return 0;
    }
    case RunAway: { // RunAway(monster, from, frames)
        const std::int32_t frames = vm.popInt();
        ServerObject* from = popObject();
        const std::int32_t who = vm.popInt();
        if (from) {
            if (ServerObject* o = object(who)) {
                runAway(*o, *from, frames);
            }
        }
        return 0;
    }
    case GroupRunAway: {
        const std::int32_t frames = vm.popInt();
        ServerObject* from = popObject();
        const std::int32_t group = vm.popInt();
        if (from) {
            eachObjectOf(world_, group, [&](ServerObject& o) { runAway(o, *from, frames); });
        }
        return 0;
    }
    case PauseObject: { // PauseObject(monster, frames): WAIT
        const std::int32_t frames = vm.popInt();
        if (ServerObject* o = popObject()) {
            pause(*o, frames);
        }
        return 0;
    }
    case GroupPauseObject: {
        const std::int32_t frames = vm.popInt();
        eachObjectOf(world_, vm.popInt(), [&](ServerObject& o) { pause(o, frames); });
        return 0;
    }
    case Walk: { // Walk(monster, x, y): FAR_MOVE_TO, the prefab's offset added
        const float y = vm.popFloat();
        const float x = vm.popFloat();
        if (ServerObject* o = popObject()) {
            walkTo(*o, offsetX(x), offsetY(y));
        }
        return 0;
    }
    case GroupWalk: {
        const float y = vm.popFloat();
        const float x = vm.popFloat();
        if (const ServerGroup* g = world_.group(vm.popInt())) {
            eachMemberOf(world_, *g, [&](ServerObject& o) { walkTo(o, offsetX(x), offsetY(y)); });
        }
        return 0;
    }
    case Damage: { // Damage(target, source, amount, type)
        const std::int32_t type = vm.popInt();
        const std::int32_t amount = vm.popInt();
        const std::int32_t source = vm.popInt();
        if (ServerObject* t = popObject()) {
            damage(*t, object(source), amount, type);
        }
        return 0;
    }
    case GroupDamage: {
        const std::int32_t type = vm.popInt();
        const std::int32_t amount = vm.popInt();
        ServerObject* source = popObject();
        eachObjectOf(world_, vm.popInt(), [&](ServerObject& t) { damage(t, source, amount, type); });
        return 0;
    }
    case EnchantBuiltin: { // Enchant(object, "ENCHANT_...", seconds)
        const float seconds = vm.popFloat();
        const int e = enchantByName(vm.string(vm.pop()));
        const std::int32_t who = vm.popInt();
        if (e != -1) {
            if (ServerObject* o = object(who)) {
                enchantFor(*o, e, seconds);
            }
        }
        return 0;
    }
    case GroupEnchant: {
        const float seconds = vm.popFloat();
        const int e = enchantByName(vm.string(vm.pop()));
        const std::int32_t group = vm.popInt();
        if (e != -1) {
            eachObjectOf(world_, group, [&](ServerObject& o) { enchantFor(o, e, seconds); });
        }
        return 0;
    }
    case EnchantOff: { // EnchantOff(object, "ENCHANT_...")
        const int e = enchantByName(vm.string(vm.pop()));
        const std::int32_t who = vm.popInt();
        if (e != -1) {
            if (ServerObject* o = object(who)) {
                disenchant(world_, *o, e);
            }
        }
        return 0;
    }
    case HasEnchant: { // HasEnchant(object, "ENCHANT_...")
        const int e = enchantByName(vm.string(vm.pop()));
        const std::int32_t who = vm.popInt();
        ServerObject* o = e != -1 ? object(who) : nullptr;
        vm.push(o && enchanted(*o, e) ? 1 : 0);
        return 0;
    }
    case CurrentHealth:
    case MaxHealth: {
        ServerObject* o = popObject();
        vm.push(o ? (id == CurrentHealth ? o->health : o->maxHealth) : 0u);
        return 0;
    }
    case RestoreHealth: { // RestoreHealth(object, amount): up by a positive amount
        const std::int32_t amount = vm.popInt();
        if (ServerObject* o = popObject(); o && 0 < amount) {
            adjustHealth(world_, *o, amount);
        }
        return 0;
    }
    case IsAttackedBy: { // IsAttackedBy(a, b): Obj_IsEnemyTo(a, b)
        ServerObject* b = popObject();
        ServerObject* a = popObject();
        vm.push(a && b && isEnemy(world_, *a, *b) ? 1 : 0);
        return 0;
    }
    case IsVisibleTo: { // IsVisibleTo(a, b): within 512 each way, then canInteract
        ServerObject* b = popObject();
        ServerObject* a = popObject();
        bool r = false;
        if (a && b && std::fabs(a->x - b->x) <= 512.0f && std::fabs(a->y - b->y) <= 512.0f) {
            r = canInteract(world_, *a, *b, 0);
        }
        vm.push(r ? 1 : 0);
        return 0;
    }
    case IsOwnedBy: { // IsOwnedBy(unit, owner)
        ServerObject* owner = popObject();
        ServerObject* unit = popObject();
        vm.push(hasParent(world_, unit, owner) ? 1 : 0);
        return 0;
    }
    // The other owner tests, as the exe reads its arguments whatever their names say (a missing
    // group: nothing pushed, an original bug).
    case GroupIsOwnedBy: { // (object, group): every member the object or up its owners
        const ServerGroup* g = world_.group(vm.popInt());
        const std::int32_t who = vm.popInt();
        if (!g) {
            return 0;
        }
        ServerObject* unit = object(who);
        bool all = true;
        eachMemberOf(world_, *g, [&](ServerObject& m) { all = all && hasParent(world_, unit, &m); });
        vm.push(all ? 1 : 0);
        return 0;
    }
    case IsOwnedByAny: { // (group, object): the object each member's or up its owners
        const std::int32_t who = vm.popInt();
        const ServerGroup* g = world_.group(vm.popInt());
        if (!g) {
            return 0;
        }
        ServerObject* owner = object(who);
        bool all = true;
        eachMemberOf(world_, *g, [&](ServerObject& m) { all = all && hasParent(world_, &m, owner); });
        vm.push(all ? 1 : 0);
        return 0;
    }
    case GroupIsOwnedByAny: { // (group, group): each of the second's up the owners of each of the first's
        const ServerGroup* second = world_.group(vm.popInt());
        const ServerGroup* first = world_.group(vm.popInt());
        if (!second || !first) {
            return 0;
        }
        bool all = true;
        eachMemberOf(world_, *second, [&](ServerObject& owner) {
            if (!all) {
                return;
            }
            for (const std::int32_t m : first->members) {
                ServerObject* u = world_.byMapNumber(static_cast<std::uint32_t>(m));
                if (u && !hasParent(world_, u, &owner)) {
                    all = false;
                    break;
                }
            }
        });
        vm.push(all ? 1 : 0);
        return 0;
    }
    case HasItem: { // HasItem(holder, item)
        ServerObject* item = popObject();
        ServerObject* holder = popObject();
        bool r = false;
        if (holder && item && item->holder == holder->extent) {
            r = std::ranges::find(holder->inventory, item->extent) != holder->inventory.end();
        }
        vm.push(r ? 1 : 0);
        return 0;
    }
    case GetLastItem: { // its inventory's head (the newest): its script id
        ServerObject* o = popObject();
        ServerObject* first = o && !o->inventory.empty() ? world_.byExtent(o->inventory.front()) : nullptr;
        vm.pushInt(first ? first->scriptId : 0);
        return 0;
    }
    case GetPreviousItem: { // the next in its holder's list (the one before it)
        ServerObject* o = popObject();
        ServerObject* next = nullptr;
        if (o && o->holder) {
            if (ServerObject* h = world_.byExtent(o->holder)) {
                const auto it = std::ranges::find(h->inventory, o->extent);
                if (it != h->inventory.end() && it + 1 != h->inventory.end()) {
                    next = world_.byExtent(*(it + 1));
                }
            }
        }
        vm.pushInt(next ? next->scriptId : 0);
        return 0;
    }
    case Pickup: { // Pickup(taker, item): Obj_TryPickup(taker, item, 1, 1)
        ServerObject* item = popObject();
        ServerObject* taker = popObject();
        if (!taker || !item) {
            vm.push(0);
            return 0;
        }
        const bool solo = (world_.gameFlags & GameFlag::Solo) != 0;
        // (The taker's type tested for gold, where the item's was meant: never one, so gold given to
        // a full player drops its cheapest thing too. An original bug.)
        const bool gold = taker->typeName == "Gold" || taker->typeName == "QuestGoldPile" || taker->typeName == "QuestGoldChest";
        if (solo && (taker->classes & oc::Player) && pickupFrame_ != frame_) {
            pickupFrame_ = frame_; // (the co-op counts afresh each frame)
            pickupsThisFrame_ = 0;
            carryingTooMuchShown_ = false;
        }
        if (solo && (taker->classes & oc::Player) && !gold && world_.hostClient) {
            // Plr_DropCheapestItemIfFull: no more cells of the host's client's grid
            // that take one more than this frame's pickups: the cheapest thing it
            // may drop (not food, not equipped, not a glyph, not a quest item; its price
            // shopGetItemCost(1, no shop), the first of the lowest) goes within
            // 50, and "pickup.c:CarryingTooMuch" once a frame.
            const int room = world_.hostClient->inventoryRoom(item->type, 1);
            if (room - pickupsThisFrame_ <= 0) {
                ServerObject* cheapest = nullptr;
                int lowest = 999999;
                for (const std::uint32_t e : taker->inventory) {
                    ServerObject* i = world_.byExtent(e);
                    if (!i || (i->classes & oc::Food) || (i->flags & ObjFlag::Equipped) || i->typeName == "Glyph" || questItem(*i)) {
                        continue;
                    }
                    if (const int cost = itemCost(world_, modifiers_, 1, nullptr, *i); cost < lowest) {
                        cheapest = i;
                        lowest = cost;
                    }
                }
                if (cheapest) {
                    const auto [x, y] = dropSpotNear(fight, *taker);
                    items().drop(*taker, *cheapest, x, y);
                    if (!carryingTooMuchShown_) {
                        world_.broadcasts().push_back({informStringBytes("pickup.c:CarryingTooMuch"), true, taker->extent}); // (SrvNet_SendInformString)
                        carryingTooMuchShown_ = true;
                    }
                }
            }
        }
        const bool took = items().pickUp(*taker, *item, true);
        if (took && (taker->classes & oc::Player) && solo && item->typeName != "Gold") {
            ++pickupsThisFrame_;
        }
        vm.push(took ? 1 : 0);
        return 0;
    }
    case Drop: { // Drop(holder, item): within 50 of it (invForceDropItem)
        ServerObject* item = popObject();
        ServerObject* holder = popObject();
        if (!holder || !item) {
            vm.push(0);
            return 0;
        }
        const auto [x, y] = dropSpotNear(fight, *holder);
        vm.push(items().drop(*holder, *item, x, y) ? 1 : 0);
        return 0;
    }
    case HasClass: { // HasClass(object, "CLASS"): an unknown name pushes nothing
        const std::string name = vm.string(vm.pop());
        ServerObject* o = popObject();
        if (!o) {
            vm.push(0);
            return 0;
        }
        for (std::size_t i = 0; i < std::size(kScriptClassNames); ++i) {
            if (kScriptClassNames[i] == name) {
                vm.push((o->classes & (1u << i)) != 0 ? 1 : 0);
                return 0;
            }
        }
        return 0; // (no result: an original bug)
    }
    case HasSubclass: { // HasSubclass(object, "SUBCLASS"): the first table naming it, whatever its class
        const std::string name = vm.string(vm.pop());
        ServerObject* o = popObject();
        if (!o) {
            vm.push(0);
            return 0;
        }
        for (const ScriptSubclassTable& table : scriptSubclassTables()) {
            for (std::size_t i = 0; i < table.names.size(); ++i) {
                if (table.names[i] == name) {
                    vm.push((o->subclasses & (1u << i)) != 0 ? 1 : 0);
                    return 0;
                }
            }
        }
        return 0; // (the exe reads on past its tables: an original bug)
    }
    case IsSummoned: { // a monster's SUMMONED bit
        ServerObject* o = popObject();
        vm.push(o && isMonster(*o) && (o->monster->status & ms::Summoned) ? 1 : 0);
        return 0;
    }
    case GetGold: { // (object_getGold) a player's
        ServerObject* o = popObject();
        vm.push(o && (o->classes & oc::Player) && o->player ? o->player->gold : 0u);
        return 0;
    }
    case ChangeGold: { // (object_setGold) a player's up or down, not below 0
        const std::int32_t amount = vm.popInt();
        if (ServerObject* o = popObject(); o && (o->classes & oc::Player)) {
            if (amount < 0) {
                subGold(*o, 0u - static_cast<std::uint32_t>(amount));
            } else {
                addGold(*o, amount);
            }
        }
        return 0;
    }
    case AwardSpell: { // AwardSpell(player, "SPELL_..."): Spell_GrantToPlayer(p, spell, 1, 0, 0)
        const std::uint32_t spell = spellNamed(vm.pop());
        const std::int32_t who = vm.popInt();
        ServerObject* o = spell != 0 ? object(who) : nullptr;
        vm.push(o && grant(*o, spell, false) ? 1 : 0);
        return 0;
    }
    case GroupAwardSpell: { // each member's, shown in a solo game to a player new to it
        const std::uint32_t spell = spellNamed(vm.pop());
        const std::int32_t group = vm.popInt();
        if (spell != 0) {
            eachObjectOf(world_, group, [&](ServerObject& o) {
                const bool show = (world_.gameFlags & GameFlag::Solo) && (o.classes & oc::Player) && o.player &&
                                  o.player->spellLevels[spell] == 0;
                grant(o, spell, show);
            });
        }
        return 0;
    }
    case CastSpellObjectObject: { // CastSpellObjectObject("SPELL", caster, target)
        const std::int32_t to = vm.popInt();
        const std::int32_t by = vm.popInt();
        const std::uint32_t spell = spellNamed(vm.pop());
        if (spell != 0) {
            ServerObject* caster = object(by);
            if (caster && !(caster->flags & (ObjFlag::Dead | ObjFlag::Destroyed))) {
                if (ServerObject* target = object(to)) {
                    faceOnly(*caster, directionOf(target->x - caster->x, target->y - caster->y));
                    spells().castByUser(spell, *caster, target, target->x, target->y);
                }
            }
        }
        return 0;
    }
    case CastSpellObjectLocation: { // CastSpellObjectLocation("SPELL", caster, x, y)
        const float y = vm.popFloat();
        const float x = vm.popFloat();
        const std::int32_t by = vm.popInt();
        const std::uint32_t spell = spellNamed(vm.pop());
        if (spell != 0) {
            if (ServerObject* caster = object(by)) {
                faceOnly(*caster, directionOf(x - caster->x, y - caster->y));
                spells().castByUser(spell, *caster, nullptr, x, y);
            }
        }
        return 0;
    }
    case CastSpellLocationObject: { // the ImaginaryCaster at (x, y) casts at the target
        const std::int32_t to = vm.popInt();
        const float y = vm.popFloat();
        const float x = vm.popFloat();
        const std::uint32_t spell = spellNamed(vm.pop());
        if (spell != 0) {
            ServerObject* target = object(to);
            ServerObject* caster = target ? imaginaryCaster() : nullptr;
            if (caster) {
                setPosition(world_, *caster, x, y, frame_);
                caster->direction = static_cast<std::uint16_t>(directionOf(target->x - x, target->y - y));
                spells().castByUser(spell, *caster, target, target->x, target->y);
            }
        }
        return 0;
    }
    case CastSpellLocationLocation: { // the ImaginaryCaster at (x, y) casts at (x2, y2)
        const float y2 = vm.popFloat();
        const float x2 = vm.popFloat();
        const float y = vm.popFloat();
        const float x = vm.popFloat();
        const std::uint32_t spell = spellNamed(vm.pop());
        if (spell != 0) {
            if (ServerObject* caster = imaginaryCaster()) {
                setPosition(world_, *caster, x, y, frame_);
                caster->direction = static_cast<std::uint16_t>(directionOf(x2 - x, y2 - y));
                spells().castByUser(spell, *caster, nullptr, x2, y2);
            }
        }
        return 0;
    }
    case PushObjectTo: { // PushObjectTo(object, x, y): its force up by (x, y)
        const float y = vm.popFloat();
        const float x = vm.popFloat();
        if (ServerObject* o = popObject()) {
            o->forceX = (util::X87(x) + o->forceX).f(); // (one add, stored: chopped)
            o->forceY = (util::X87(y) + o->forceY).f();
        }
        return 0;
    }
    case PushObject: { // PushObject(object, strength, x, y): away from (x, y)
        const float y = vm.popFloat();
        const float x = vm.popFloat();
        const float strength = vm.popFloat();
        if (ServerObject* o = popObject()) {
            // (The prefab's offset added to the difference, not to the point: an original bug.)
            // (On the x87: the offset added by FIADD; dx kept on the stack, dy stored and squared
            // from the stack by the stored one; the root stored; each push one store.)
            using util::X87;
            const X87 dx = X87(o->x) - x + X87::fromDouble(vm.context().x);
            const X87 dyStack = X87(o->y) - y + X87::fromDouble(vm.context().y);
            const float dy = dyStack.f();
            const float len = util::sqrt(dyStack * dy + dx * dx).f();
            o->forceX = (X87(strength) * dx / len + o->forceX).f();
            o->forceY = (X87(strength) * dy / len + o->forceY).f();
        }
        return 0;
    }
    case Raise: { // (unitRaise) Raise(object, z)
        const float z = vm.popFloat();
        if (ServerObject* o = popObject()) {
            o->raise(z);
        }
        return 0;
    }
    case GetElevatorStatus: { // an elevator's state, else -1
        ServerObject* o = popObject();
        vm.pushInt(o && (o->classes & oc::Elevator) ? static_cast<std::int32_t>(o->elevatorState) : -1);
        return 0;
    }
    case DeleteObjectTimer: { // (unitSetDecayTime) gone `frames` on
        const std::int32_t frames = vm.popInt();
        if (ServerObject* o = popObject()) {
            // (Not a SHADOW, 0x10000: none here.)
            o->decayAt = frame_ + static_cast<std::uint32_t>(frames);
        }
        return 0;
    }
    case ZombieStayDown:
        if (ServerObject* o = popObject()) {
            zombieStayDown(*o);
        }
        return 0;
    case ZombieGroupStayDown:
        eachObjectOf(world_, vm.popInt(), zombieStayDown);
        return 0;
    case RaiseZombie:
        if (ServerObject* o = popObject()) {
            raiseZombie(*o);
        }
        return 0;
    case RaiseZombieGroup:
        eachObjectOf(world_, vm.popInt(), raiseZombie);
        return 0;
    case SetHalberd: { // (script_OblivionGive) SetHalberd(kind): the host's Oblivion staff swapped
        const std::int32_t kind = vm.popInt();
        ServerObject* host = hostPlayer(world_); // (Plr_GetByIndex(0x1F)->PlayerUnit: none, the exe crashes)
        static constexpr std::array<std::string_view, 4> kStaffs{"OblivionHalberd", "OblivionHeart", "OblivionWierdling", "OblivionOrb"};
        if (!host || kind < 0 || static_cast<int>(kStaffs.size()) <= kind) {
            return 0;
        }
        bool worn = false;
        auto it = items();
        for (const std::uint32_t e : std::vector<std::uint32_t>(host->inventory)) {
            ServerObject* i = world_.byExtent(e);
            if (i && (i->classes & oc::Weapon) && (i->subclasses & 0x7800000u)) {  // the Oblivion staffs (subclass bits 23-26)
                worn = (i->flags & ObjFlag::Equipped) != 0;
                it.removeFromInventory(*host, *i); // (delayedDeleteObject)
                i->pendingDelete = true;
                break;
            }
        }
        // (playerRespawnItem(host, staff, 0, 1, 1), then Plr_TryEquip when it wore one;
        // the last 1: worn as given, told to every client)
        ServerObject* staff = it.give(*host, kStaffs[static_cast<std::size_t>(kind)], {0xFF, 0xFF, 0xFF, 0xFF}, true);
        if (staff && worn) {
            it.tryEquip(*host, *staff);
        }
        return 0;
    }
    default:
        return -1;
    }
}

} // namespace nox::server
