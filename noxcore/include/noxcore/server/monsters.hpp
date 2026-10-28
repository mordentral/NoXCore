// Monsters, as GAME.EXE's server runs them.
//
//   - Their numbers come from monster.bin (things/monster_db.hpp) by MonsterCreate:
//     health, speed (x), retreat / resume ratios, status, flee range; the AI's defaults
//     (aggression 0.5, sight 150, no script hooks). The map's own (MonsterXfer) over
//     them: the script hooks, status, aggression, sight, the action it starts in
//     and its arguments.
//   - MonsterInit, before its first update here: its starting action pushed (ESCORT /
//     GUARD by where it stands, ROAM, FIGHT...), home and facing noted, its health history, its
//     speed x 0.95-1.05, flee range 100 for casters, 0 when it holds its ground.
//   - The action stack (AIStack[24], the monster's): push (monsterPushAction: the top's END
//     first when it had started; not past 23; nothing over DEAD but DYING), pop (monsterPopAction: its
//     END, then the conditions over 0x27 under it too; empty: IDLE), clear (monsterClearActionStack). Each
//     action's start / update / end (the table).
//   - MonsterUpdate: the stack's object arguments kept up (mobAction),
//     then for an enabled one: the hurt flag's IsHit script, regeneration (a point every 180 s /
//     its most once a second unhurt), the AI (stage B), then the top action's start (once) and
//     update; its stamina back; ON_FIRE off 3 s unhurt.
//   - Death (monsterCallDieFn, from takeHealth): the stack cleared, DEAD then
//     DYING; not a zombie: NOT_CLEAN off, its enchantments off, a summoned one's body gone in
//     10-20 s (a quest's 5-8 s; else it stays), its master told, it no one's, its things
//     dropped (not subclass 0x2000). DYING: the die sound, the Death script, the
//     monster's DIE_FUNCTION (stage B); till its animation ends. DEAD: out of
//     motion (NO_UPDATE-like: it stops updating, mobActionDead2), DEAD_FUNCTION; DESTROY_WHEN_DEAD:
//     gone. (Zombies: with the special creatures.)
// Sounds: the creature's own from soundset.bin (hurt, death, moves, attacks...).
#pragma once

#include "noxcore/server/combat.hpp"
#include "noxcore/server/world.hpp"

namespace nox::server {

namespace MonsterAction {
inline constexpr int Idle = 0, Wait = 1, WaitRelative = 2, Escort = 3, Guard = 4, Hunt = 5, Retreat = 6, MoveTo = 7,
                     FarMoveTo = 8, Dodge = 9, Roam = 10, PickupObject = 11, DropObject = 12, FindObject = 13,
                     RetreatToMaster = 14, Fight = 15, MeleeAttack = 16, MissileAttack = 17, CastSpellOnObject = 18,
                     CastSpellOnLocation = 19, CastDurationSpell = 20, BlockAttack = 21, BlockFinish = 22, WeaponBlock = 23,
                     Flee = 24, FaceLocation = 25, FaceObject = 26, FaceAngle = 27, SetAngle = 28, RandomWalk = 29, Dying = 30,
                     Dead = 31, Report = 32, MorphIntoChest = 33, MorphBackToSelf = 34, GetUp = 35, Confused = 36,
                     MoveToHome = 37, Invalid = 38;
/// "ACTION_IDLE"..., for the map's starting actions and the debug.
const char* name(int action);
/// The action of that name (MonsterXfer's), or Invalid.
int byName(std::string_view name);
} // namespace MonsterAction

/// The conditions an action is kept under (DEPENDENCY_*, 0x28-0x47: pushed under it, checked
/// each frame; one failing pops the actions down to it). OR makes those under
/// it, down to the next action, hold when any of them does; UNINTERRUPTABLE stops the checking
/// and the main AI under it.
namespace Dependency {
inline constexpr int Or = 0x28, Time = 0x29, Alive = 0x2A, UnderAttack = 0x2B, NotUnderAttack = 0x2C, CanSee = 0x2D,
                     CannotSee = 0x2E, BlockedLineOfFire = 0x2F, ObjectAtVisibleLocation = 0x30, ObjectFartherThan = 0x31,
                     ObjectCloserThan = 0x32, LocationFartherThan = 0x33, LocationCloserThan = 0x34, VisibleEnemy = 0x35,
                     VisibleFriend = 0x36, VisibleFood = 0x37, NoVisibleEnemy = 0x38, NoVisibleFriend = 0x39, NoVisibleFood = 0x3A,
                     NoInterestingSound = 0x3B, NoNewEnemy = 0x3C, Uninterruptable = 0x3D, IsEnchanted = 0x3E,
                     EnemyCloserThan = 0x3F, NotHealthy = 0x40, WaitForStamina = 0x41, EnemyFartherThan = 0x42,
                     UnderCursor = 0x43, NotCornered = 0x44, LocationIsSafe = 0x45, NotFrustrated = 0x46, NotMoved = 0x47;
} // namespace Dependency

/// The monsters' AI over one frame's fight context (made on the stack where it's needed).
struct Monsters {
    Fight& fight;

    /// MonsterInit (on its first update here).
    void init(ServerObject& m);
    /// MonsterUpdate.
    void update(ServerObject& m);
    /// The stack (null when it can't take another).
    ServerObject::Monster::Item* push(ServerObject& m, int action);
    /// The Fight action pushed at `at`: its place (as raw float bits) and `frame` the first three
    /// arguments, the fourth left as the slot held it.
    ServerObject::Monster::Item* pushFight(ServerObject& m, const ServerObject& at, std::uint32_t frame);
    void pop(ServerObject& m);
    void clear(ServerObject& m);
    /// monsterCallDieFn.
    void dies(ServerObject& m);
    /// Its sight (unitUpdateSightMB).
    void look(ServerObject& m);
    /// Whether the stack holds `action` (checkMobAction).
    static bool has(const ServerObject& m, int action);

private:
    /// An action's start (once, as it comes to the top), update (each frame on top) and end (popped).
    void start(ServerObject& m, int action);
    void act(ServerObject& m, int action);
    void end(ServerObject& m, int action);
    /// Another pushed over it once started (the table's fourth).
    void aside(ServerObject& m, int action);
    /// The seen creature at `index` dropped (its LostEnemy script; its enemy forgotten if it was).
    void lostSight(ServerObject& m, int index);
    /// `t` added to what it sees (a full list of 16 gives up its farthest); its sighted sound and script.
    void sees(ServerObject& m, ServerObject& t);
    /// Its enemy: the one it keeps to while seen, else the nearest killable enemy seen.
    void chooseEnemy(ServerObject& m);
    /// The main AI (monsterMainAIFn): what to push this frame (stop for the host's cursor,
    /// counter a spell, flee, retreat, block, dodge, get unstuck, eat).
    void think(ServerObject& m);
    /// The conditions on the stack checked from the top; a failing one pops down past it.
    void checkDependencies(ServerObject& m);
    /// The attacks, casts and turns on top popped.
    void popAttacks(ServerObject& m);
    /// A blow it hasn't answered: true when it pushed a FIGHT against an enemy creature.
    bool reactToHurt(ServerObject& m);
    /// Hurt this frame: it turns to face the blow.
    bool lookAtDamager(ServerObject& m);
    /// A sound heard within 3 s (forgotten once asked).
    bool chaseSound(ServerObject& m);
    /// FIGHT's update: a spell or an attack while its enemy's seen; else it goes to where it
    /// last saw it; 10 s unseen (or its body found there), done.
    void fightUpdate(ServerObject& m);
    /// The approach and the attack (melee or missile) on `enemy`, pushed.
    void engage(ServerObject& m, ServerObject& enemy);
    /// IDLE's, GUARD's and ESCORT's updates.
    void idle(ServerObject& m);
    void guard(ServerObject& m);
    void escort(ServerObject& m);
    /// A turn of 8 towards (x, y); true (popped) once facing it.
    bool faceTowards(ServerObject& m, float x, float y);
    // Moving (B2): a path to a point (creatureSetDetailedPath), a waypoint route,
    // the step along the path (creatureActuallyMove: true at its
    // end), along the waypoints, to the top's point (creatureSetMovePath).
    void detailedPath(ServerObject& m, float x, float y);
    void waypointPath(ServerObject& m, float x, float y);
    bool walkPath(ServerObject& m);
    bool followWaypoints(ServerObject& m);
    bool moveAlong(ServerObject& m);
    void moveTo(ServerObject& m);
    void roam(ServerObject& m);
    bool roamOn(ServerObject& m, std::uint32_t waypoint);
    void randomWalk(ServerObject& m);
    // Striking (B3): the nearest enemy in front within its reach
    // (monsterPickMeleeTarget), and monster.bin's STRIKE_FUNCTION (its functions' table):
    // whether it hit.
    ServerObject* meleeTarget(ServerObject& m, bool any);
    bool strike(ServerObject& m);
    // Stage C: a missile coming at it (monsterTestBlockShield), a side step out
    // of the way (monsterCheckDodgeables), the shot (mobActionMissileAtt),
    // RETREAT, FLEE, RETREAT_TO_MASTER, DODGE.
    ServerObject* incoming(ServerObject& m);
    bool dodge(ServerObject& m);
    void shoot(ServerObject& m);
    void retreat(ServerObject& m);
    void flee(ServerObject& m);
    void retreatToMaster(ServerObject& m);
    void sidestep(ServerObject& m);
    // Stage D: a spell cast as its own (mobCast -> castSpellByUser),
    // the actions for one (monsterCast), the choices (against a spell coming
    // mobCastInversion, on itself monsterBuffSelf, at an enemy monsterCastOffensive, a summons or hold mobCastRelated2, to get
    // away mobCastRelated, a heal mobHealSomeone), and the casting at the cast frame (mobActionCast).
    void castAs(ServerObject& m, std::uint32_t spell, ServerObject* target, float x, float y);
    void castAt(ServerObject& m, std::uint32_t spell, ServerObject& target);
    bool counterSpell(ServerObject& m);
    bool buffSelf(ServerObject& m);
    bool castOffensive(ServerObject& m, ServerObject& enemy);
    bool castHold(ServerObject& m, ServerObject& enemy);
    bool castAway(ServerObject& m);
    bool healSomeone(ServerObject& m);
    void castAction(ServerObject& m, int mode);
    void burnZombie(ServerObject& m);

public:
    // Stage E1: monster.bin's DIE / DEAD functions (the table), a zombie risen
    // (mobRaiseZombie) or burnt away (zombieBurnDelete).
    void deathFunction(ServerObject& m, const std::string& name);
    /// mobRaiseZombie (also the script's RaiseZombie): a dead zombie gets up.
    void raiseZombie(ServerObject& m);
};

/// monsterTestBlockShield (a monster's, and a player's shield in Plr_UpdateState):
/// of the missiles within 100 (getMissilesInCircle, each tested), the nearest that would
/// hit it, seen on its front side from its PrevPos, flying at it, that it sees.
ServerObject* incomingMissile(World& world, ServerObject& o);
/// monsterCanMelee / monsterCanShoot (a monster's).
bool monsterCanMelee(const ServerObject& m);
bool monsterCanShoot(const ServerObject& m);
/// unitFreeze (the script's Frozen, a dialog, a shop): NO_UPDATE; a player its
/// client told (MSG_REPORT_PLAYER_STATUS), standing (state 0xD) on the floor, its summoned
/// creatures frozen with it, `lock` kept as World::freezeLock unless one is; a live monster
/// idles (IDLE pushed). Nothing when it's frozen already.
void freezeUnit(Fight& fight, ServerObject& o, int lock);
/// unitUnFreeze: NO_UPDATE off (a player not while World::freezeLock holds and
/// `lock` is 0: a dialog's end under the script's Frozen), the lock cleared, its client told, its
/// summoned creatures thawed; a live monster's top action (the IDLE) popped.
void unfreezeUnit(Fight& fight, ServerObject& o, int lock);
/// SrvNet_SendReportPlayerStatus: MSG_REPORT_PLAYER_STATUS 0x66 {u32 flags} to its client.
void reportPlayerStatus(World& world, const ServerObject& unit);

/// MonsterCreate's part (the numbers and the AI's defaults), as describe makes a monster.
void createMonster(ServerObject& m, const things::MonsterDef* def, std::uint32_t gameFlags, const things::ThingDef* type = nullptr);
/// The casters' own spells (monsterAutoSpells: the urchin shaman, wizards,
/// beholder, lich and lich lord, demon, will-o'-wisp).
void typeSpells(ServerObject& m, std::uint32_t gameFlags);
/// The creature guide (from 1: Bat ... UrchinShaman; a summoning's spell less 0x4A).
const char* guideName(int index);
/// Its place in the guide (creatureIsCharmableByTT: 0 none; a Bomber never).
int guideIndex(std::string_view type);
/// A guide creature's size (loadGuides: its type's subclass 1 small, 2 medium,
/// else 4).
int guideSize(const things::ThingDb& things, int index);
/// A creature's own size the same way.
int creatureSize(const ServerObject& creature);
/// One of `owner`'s summoned or charmed (creatureIsMonitored): a monster (alive,
/// or a zombie), SUMMONED, `owner` up its owners.
bool controls(const World& world, const ServerObject& owner, const ServerObject& creature);
/// Room for one more of that guide creature: its size and those it controls under 5
/// (checkSummonedCreaturesLimit / countControlledCreatures).
bool roomFor(const World& world, const things::ThingDb& things, const ServerObject& owner, int index);
/// A creature summoned (unitDoSummonAt): at (x, y) facing `direction`, SUMMONED,
/// its owner's: a player's follows its creature order (SummonOrderAll), its player told
/// (MSG_REPORT_ACQUIRE_CREATURE) and its team joined; another's escorts it.
ServerObject* summonAt(Fight& fight, std::string_view type, float x, float y, ServerObject* owner, int direction);
/// An order to one of `owner`'s creatures, or to all it commands (AI_OrderCreatures ->
/// enactUnitOrder): 0 banish (its own), 1 a player watches it (observeCreature),
/// 2 idle, 3 guard, 4 escort, 5 hunt. A player's 3-5 to all is kept as its order
/// (MSG_REPORT_CREATURE_CMD).
void orderCreatures(Fight& fight, ServerObject& owner, ServerObject* creature, int order);
/// Its player told it's lost (MSG_REPORT_LOSE_CREATURE, monsterCallDieFn's).
void loseCreature(World& world, ServerObject& creature);

/// Monster generators (stage E3). MonsterGeneratorUpdate (at its first,
/// MonsterGeneratorInit: its most out by difficulty, its facing by its subclass): not
/// while xStatus 0x800, enabled and whole; past its spawn rate (SpawnRate*Value by difficulty;
/// a quest's hardcore stages sooner), under its most and on the frames with bit 8, one of its
/// monsters at random where it may (mobGeneratorPick / mgenSetCreaturePos) made (mobGeneratorSpawn).
void generatorUpdate(Fight& fight, ServerObject& generator);
/// whether a generator may spawn at (x, y): no quest player whose view holds it
/// sees MaxOnscreenMonsterCount monsters or more in that view.
bool generatorRoomOnScreen(World& world, float x, float y);
/// MonsterGeneratorInit, the exe's at its creation (ours at its first update, or a
/// quest map's load before the map's difficulty is drawn): its most out by the map's difficulty,
/// its facing by its subclass.
void generatorInit(World& world, ServerObject& generator);
/// MonsterGeneratorDie: its death handler, its burst (0xF0 0x19), a
/// DestroyedGenerator where it stood, gone.
void generatorDies(Fight& fight, ServerObject& generator);
/// MonsterGeneratorCollide: a player touching it runs its touch handler.
void generatorTouched(Fight& fight, ServerObject& generator, ServerObject* other);
/// One it made gone (its generator's count down; at its deletion, a quest's at
/// its death).
void unlinkSpawn(World& world, ServerObject& monster);
/// Its player told it has the creature (MSG_REPORT_ACQUIRE_CREATURE) and its health
/// (MSG_REPORT_TOTAL_HEALTH); the creature marked on its minimap (bit 1).
void reportAcquired(World& world, ServerObject& player, ServerObject& creature);
/// SrvNet_SendReportMonitorCreature: MSG_REPORT_MONITOR_CREATURE and its health, to
/// `player` (its minimap the caller's).
void reportMonitored(World& world, ServerObject& player, ServerObject& creature);
/// SrvNet_SendSimpleObjReliable: `o`'s record to `player` straight away, reliable
/// (MSG_SIMPLE_OBJ 0x2F: its code, type, x, y), as a creature's acquired and a player joins.
void simpleObjTo(World& world, const ServerObject& player, const ServerObject& o);
/// Put at the head of its owner's list (unitSetOwner: ServerObject::ownedOrder); a
/// monster's enemy forgotten.
void ownedNow(World& world, ServerObject& creature);
/// unitClearOwner, the one way anything (a creature, an item, a missile, a ball,
/// a crown) becomes nobody's: a creature its player controls (creatureIsMonitored)
/// is lost to it first (loseCreature: subclass 0x80 off, MSG_REPORT_LOSE_CREATURE, off its
/// minimap, bit 1); out of its owner's list; a monster's enemy forgotten.
void clearOwner(World& world, ServerObject& o);
/// unitSetOwner: clearOwner, then `owner`'s, or, while that's being deleted, its
/// owner's up the chain (none past the last), at the head of its list (ownedNow); a monster's
/// enemy forgotten either way.
void setOwner(World& world, ServerObject* owner, ServerObject& o);
/// monsterMarkUpdate: a creature's or player's friendship to each player (not
/// its enemy, unitIsHostileMimic; a quest's ownerless Mimic never), each change
/// owed to that player (MSG_OBJECT_FRIEND_ADD / _REMOVE as it's next sent).
void markFriends(World& world, ServerObject& o);
/// unitPostCreateNotify (createAt, for a creature or player): its friendship bits cleared, then
/// set for each player it's a friend of, each owed (its client told MSG_OBJECT_FRIEND_ADD).
void postCreateNotify(World& world, ServerObject& o);
/// Every object's friendship to `player` afresh (the join's and the team change's walk): its bit
/// cleared on each, set again on a creature or player that's its friend, the change owed. No
/// MSG_RESET_FRIENDS (refriend sends that).
void friendsOf(World& world, ServerObject& player);
/// A player's team changed (Team_AddMember): MSG_RESET_FRIENDS to it, every
/// object's friendship to it afresh, then its own and its creatures' marked.
void refriend(World& world, ServerObject& player);
/// unitTransferSlaves: what `o` owns, its list from the head, each to `o`'s owner
/// (setOwner).
void transferSlaves(World& world, ServerObject& o);
/// unitBecomePet (the script's BecomePet): `player`'s, watched (subclass 0x80,
/// MSG_REPORT_MONITOR_CREATURE 0xDB and its health), on its minimap (bit 1).
void becomePet(World& world, ServerObject& player, ServerObject& creature);
/// monsterRemoveMonitors (the script's BecomeEnemy; a watched creature deleted):
/// unwatched (MSG_REPORT_UNMONITOR_CREATURE 0xDC), off its minimap, nobody's.
void removeMonitors(World& world, ServerObject& player, ServerObject& creature);
/// `owner`'s list (newest first) of the monsters it commands (SUMMONED: MonsterStatus bit 0x80), as
/// playerObserverFindGoodSlave2 / playerObserverFindGoodSlave walk it.
std::vector<ServerObject*> ownedCreatures(World& world, const ServerObject& owner);
/// The script's Move / GroupMove for a monster (server_scriptMoveTo): it walks to
/// the waypoint, roams on from it when it's linked, else runs its end-of-waypoint script there.
void scriptMove(Fight& fight, ServerObject& monster, const ServerWaypoint& waypoint);
/// A bomber's glyph, carried, of its three spells aimed where it is: a map's
/// copies the first as many as are set (MonsterXfer), a generator's all three.
ServerObject* bomberGlyph(World& world, const things::ThingDb& things, ServerObject& bomber, bool allThree);
/// The same of these spells (the script's TrapSpells, all three).
ServerObject* carriedGlyph(World& world, const things::ThingDb& things, ServerObject& bomber, const std::array<std::uint32_t, 3>& spells, bool allThree);

/// MonsterXfer's end for a map's monster (monsterOnSpawnSpellcaster): monster.bin's
/// health (unless the map's is kept), its status (or only the map's hold-ground, see-friends,
/// heal and run bits), its retreat / resume levels when flagged, its type's spells.
void spawnFixup(ServerObject& m, std::uint32_t gameFlags);
/// The animation state an action shows (mobActionToAnimation: MonsterDraw's
/// numbers, 8 IDLE, 12 / 13 MOVE (running), 1 ATTACK, 9 DIE, 10 DEAD...).
int monsterAnimState(const ServerObject& m);

} // namespace nox::server
