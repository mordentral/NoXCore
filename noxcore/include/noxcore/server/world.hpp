// The server's world: a map's objects, walls, waypoints and groups as the game's server keeps
// them, for the script builtins (and, later, the game) to act on.
// ServerObject is one thing on the map (or in an inventory); World holds them all with the walls,
// waypoints, polygons, groups, the map grid and the game's globals the logic reads. world.cpp
// loads a map into it; the other server files (physics, combat, monsters, spells...) act on it.
#pragma once

#include "noxcore/server/boxed.hpp"
#include "noxcore/server/indexed_map.hpp"
#include "noxcore/server/map_grid.hpp"
#include "noxcore/server/name.hpp"
#include "noxcore/map/map_file.hpp"
#include "noxcore/map/objects.hpp"
#include "noxcore/map/sections.hpp"
#include "noxcore/strings/string_table.hpp"
#include "noxcore/things/game_data.hpp"
#include "noxcore/things/modifier.hpp"
#include "noxcore/things/monster_db.hpp"
#include "noxcore/things/sound_sets.hpp"
#include "noxcore/things/thing_db.hpp"
#include "noxcore/util/x87.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <deque>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace nox::server {

class HostClient;
struct ModeHooks;
class ScriptHost;
class SoundTable;
struct PlayerAnimations;
struct QuestMaps;
struct Fight;

/// Object flags (the exe's ObjFlags) the server looks at so far.
namespace ObjFlag {
// (Names from the exe's flag-name table, bit 0 first.)
inline constexpr std::uint32_t Below = 0x1;
inline constexpr std::uint32_t NoUpdate = 0x2;     ///< doesn't update or move
inline constexpr std::uint32_t Active = 0x4;       ///< in the world (createAt sets it)
inline constexpr std::uint32_t AllowOverlap = 0x8; ///< bodies don't push it, nor it them
inline constexpr std::uint32_t Destroyed = 0x20;   ///< gone: skipped by every lookup
inline constexpr std::uint32_t NoCollide = 0x40;   ///< set while a trap / light / fire is off
inline constexpr std::uint32_t Equipped = 0x100;
inline constexpr std::uint32_t NoPushCharacters = 0x2000; ///< creatures don't push it
inline constexpr std::uint32_t Airborne = 0x4000;  ///< jumping: over windows, small ones through
inline constexpr std::uint32_t Dead = 0x8000;
inline constexpr std::uint32_t Falling = 0x20000;   ///< in the air, coming down
inline constexpr std::uint32_t InHole = 0x40000;    ///< falling down an elevator's shaft
inline constexpr std::uint32_t Respawn = 0x80000;   ///< RESPAWN: put on the respawn list when made
inline constexpr std::uint32_t OnObject = 0x100000; ///< held up by an elevator (no gravity this frame)
inline constexpr std::uint32_t Bouncing = 0x800000; ///< falls and bounces (a fist)
inline constexpr std::uint32_t Landed = 0x80000000u; ///< (a fist's: it has struck)
inline constexpr std::uint32_t Enabled = 0x1000000;
inline constexpr std::uint32_t Resting = 0x8000000; ///< came to rest; a body's push wakes it
inline constexpr std::uint32_t KeptOnDeath = 0x10000000; ///< worn clothing (Obj_PlayerEquipArmor): never dropped
} // namespace ObjFlag

/// The game's flags (common_gameFlags, Game_HasFlags) the game logic plays by: the mode
/// layer (a network host, a solo or quest game) sets them, the logic asks.
namespace GameFlag {
inline constexpr std::uint32_t GameOver = 0x8;    ///< the match is over (its winner told)
inline constexpr std::uint32_t KingOfTheRealm = 0x10;
inline constexpr std::uint32_t CaptureTheFlag = 0x20;
inline constexpr std::uint32_t FlagBall = 0x40;
inline constexpr std::uint32_t Chat = 0x80;
inline constexpr std::uint32_t Arena = 0x100;
inline constexpr std::uint32_t Coop = 0x200;      ///< (OpenNox's name) players' glyphs spare players
inline constexpr std::uint32_t Elimination = 0x400;
inline constexpr std::uint32_t Solo = 0x800;      ///< the campaign (co-op)
inline constexpr std::uint32_t Quest = 0x1000;
inline constexpr std::uint32_t Network = 0x2000;  ///< a network game: gamedata's ARENA lines
inline constexpr std::uint32_t Paused = 0x40000;  ///< a solo game paused (solo.hpp)
inline constexpr std::uint32_t SoloOther = 0x200000; ///< also solo for gamedata (SOLO lines)
/// The versus modes (0x570): every spell known, at power 3.
inline constexpr std::uint32_t Versus = KingOfTheRealm | CaptureTheFlag | FlagBall | Arena | Elimination;
} // namespace GameFlag

/// The enchantments (the exe's buffs: bit numbers into Buffs, its ENCHANT_* name table).
namespace Enchant {
inline constexpr int Invisible = 0;
inline constexpr int Moonglow = 1;
inline constexpr int Blinded = 2;
inline constexpr int Confused = 3;
inline constexpr int Slowed = 4;
inline constexpr int Held = 5;
inline constexpr int Detecting = 6;
inline constexpr int Ethereal = 7;
inline constexpr int Run = 8;
inline constexpr int Hasted = 9;
inline constexpr int Villain = 10;
inline constexpr int Afraid = 11;
inline constexpr int Burning = 12;
inline constexpr int Vampirism = 13;
inline constexpr int Anchored = 14;
inline constexpr int Light = 15;
inline constexpr int Death = 16;
inline constexpr int ProtectFromFire = 17;
inline constexpr int ProtectFromPoison = 18;
inline constexpr int ProtectFromMagic = 19;
inline constexpr int ProtectFromElectricity = 20;
inline constexpr int Infravision = 21;
inline constexpr int Shock = 22;
inline constexpr int Invulnerable = 23;
inline constexpr int Telekinesis = 24;
inline constexpr int Freeze = 25;
inline constexpr int Shield = 26;
inline constexpr int ReflectiveShield = 27;
inline constexpr int Charming = 28;
inline constexpr int AntiMagic = 29;
inline constexpr int Crown = 30;
inline constexpr int Sneak = 31;
} // namespace Enchant

/// An object's extent (the THNG EXTENT's shape): none, a point, a circle or a box.
enum class ShapeKind : std::uint8_t { None = 0, Center = 1, Circle = 2, Box = 3 };

/// A player's own state (its unit's PlayerUpdateData) as the player update uses it.
struct PlayerControl {
    /// ud->State: 0 walking, 5 running, 0xC jumping, 0xD idle (Plr_SetState).
    std::uint8_t state = 0xD;
    std::uint8_t previousState = 0xD;
    std::uint32_t stateFrame = 0; ///< when it began
    std::uint8_t classId = 0;     ///< 0 warrior, 1 wizard, 2 conjurer
    /// noxcpp: its character's own class, as it joined (0xFF: unknown); a forced class
    /// (World::forceClass) gives way to it where none's forced.
    std::uint8_t ownClass = 0xFF;
    float cursorX = 0;            ///< where its cursor is in the world (MSG_MOUSE)
    float cursorY = 0;
    std::uint32_t cursorObject = 0; ///< what's under its cursor, found each frame
    std::uint32_t dialogWith = 0;   ///< (DialogWith) the NPC it's talking to (dialog.hpp; 0 none)
    std::uint32_t statusFlags = 0;  ///< (Player->statusFlags) 1 observing, 0x20 eliminated, 0x400 poisoned...
    std::uint8_t creatureOrder = 4; ///< (SummonOrderAll) its creatures' standing order: 4 escort
    /// The creatures in its field guide (by guide index): what it may summon or charm in
    /// a co-op or quest game.
    std::array<bool, 41> fieldGuide{};
    int lastWallX = -1;             ///< the wall cell it last touched (-1: none)
    int lastWallY = -1;
    /// A warrior's abilities' cool-downs (g_abilCooldown[player][ability], frames left).
    std::array<int, 6> abilityCooldown{};
    // A warrior's harpoon: what it's stuck in, the bolt, when it was thrown,
    // where its target was at the last check and when that was.
    std::uint32_t harpoonTarget = 0;
    std::uint32_t harpoonBolt = 0;
    std::uint32_t harpoonThrown = 0;
    float harpoonTargetX = 0;
    float harpoonTargetY = 0;
    std::uint32_t harpoonChecked = 0;
    /// Its four teleport marks (TeleportGlyph1-4's extents) and the uses each has left
    /// (3 when set).
    std::array<std::uint32_t, 4> marks{};
    std::array<std::uint8_t, 4> markUses{};
    /// Who last teleported it, and when (0 none).
    std::uint32_t teleportedBy = 0;
    std::uint32_t teleportedAt = 0;
    /// Half its screen (from its join: Swap's sight rectangle).
    float viewHalfWidth = 320;
    float viewHalfHeight = 240;
    std::uint8_t stamina = 100;
    /// Its minimap's marks (player.hpp's markMinimap), the list's head first.
    struct MinimapMark {
        std::uint32_t object = 0;
        std::uint32_t bits = 0;
    };
    std::vector<MinimapMark> minimapMarks;
    std::uint32_t markSentAt = 0;     ///< when a mark out of view was last sent
    /// The camper's watch (player.hpp's camperWatch): where it was at the last look
    /// (campX / campY), the last look that found it within CamperRadiusSq of that (campStillAt),
    /// and whether it's marked a camper for all (camping: MSG_INTERESTING_ID kind 2).
    float campX = 0;
    float campY = 0;
    std::uint32_t campStillAt = 0;
    bool camping = false;
    std::uint16_t hurtLookHealth = 0; ///< its health at the last look for a hurt cry
    /// A quest's: in it (the quest join's, the sixth and later not), its lives
    /// (questLives), its tallies (kills, generators destroyed, secrets found and
    /// credited, the stage it joined at).
    std::uint32_t questActive = 0;
    std::uint32_t questLives = 0;
    std::uint32_t questKills = 0;
    std::uint32_t questGenerators = 0;
    std::uint32_t questSecrets = 0;
    std::uint32_t questSecretsCredited = 0;
    std::uint32_t questJoinStage = 0;
    /// A quest's exits: the quest exit it went out by, the warp gate it waits in
    /// both by extent (0: none; cleared at a map change by playerMakeDefItems);
    /// the highest stage it reached (MSG_GAUNTLET 0x1D).
    std::uint32_t exitGate = 0;
    std::uint32_t warpGate = 0;
    std::uint32_t questHighestStage = 0;
    /// Its checkpoint: the soul gate it last touched (cleared at a map change).
    std::uint32_t soulGate = 0;
    /// Its gold and what its client was last told (MSG_REPORT_GOLD 0x4A).
    std::uint32_t gold = 0;
    std::uint32_t goldSent = 0;
    /// Its lives as each player was last told them (by player index, MSG_GAUNTLET 4).
    std::array<std::uint8_t, 32> livesSent{};
    /// Whether it holds a SilverKey / GoldKey as each player was last told (by
    /// player index, MSG_GAUNTLET 0x16 / 0x17).
    std::array<std::uint8_t, 32> silverKeySent{};
    std::array<std::uint8_t, 32> goldKeySent{};
    /// The rest of the quest's tallies: the maps it ended in the quest, its time run
    /// out or out by a quest exit (questTimeUps), questTally1230 (nothing adds to it), its deaths,
    /// the secrets found on this map (questSecretsCredited at
    /// the map's change), questTally124C (never called); its flags
    /// (0x3F; 1 a time-out, 2 a death, 4 a kill, 8 a generator, 0x10 a secret, 0x20 questTally124C). (Its
    /// generators destroyed have a tally too, but its adder is never called: always 0.)
    std::uint32_t questTimeUps = 0;
    std::uint32_t questTally1230 = 0;
    std::uint32_t questDeaths = 0;
    std::uint32_t questSecretsMap = 0;
    std::uint32_t questTally124C = 0;
    std::uint32_t questFlags = 0x3F;
    std::uint32_t questGameOverAt = 0; ///< the frame it ran out of lives
    /// Come into a quest, its client's saved player not loaded yet (set by
    /// SrvNet_OnIncomingClient, cleared by the load of its SAVE_SERVER).
    bool questAwaitingSave = false;
    /// The ankhs it took a life from on this map (five; cleared at a map change).
    std::array<std::uint32_t, 5> ankhsTaken{};
    /// Its name (its join data's) and CD key serial (the ankhs' memory).
    std::u16string name;
    std::string serial;
    std::uint8_t hurtLookWait = 0;    ///< frames till the next look (7 after a cry)
    bool running = false;         ///< this frame's choice (Plr_UpdateState's)
    std::uint32_t strength = 0;   ///< (a network game's: its class's best)
    std::uint8_t level = 10;      ///< Player->Level: a solo game's stats grow with it (1-10)
    int lessons = 0;              ///< its score (kills, a mode's points)
    std::uint32_t crownAt = 0;    ///< the frame it last took a crown
    int deaths = 0;               ///< its deaths (elimination's lives used)
    /// Observing (its unit's Update is Plr_UpdateObserver); its camera (the view's
    /// middle and where it hears from: an observer's own, else set each frame by
    /// updatePlayer to what it follows or its unit, and by Obj_SetPosition), what it
    /// follows, whether it pans freely (observerFreeCam) and whether a move held this
    /// frame.
    bool observing = false;
    /// (noxcpp: a stock player a NoX Core-only mode holds out, GameHost::Options::stockRefusal)
    /// Action never brings it in, nor a quest's start or a map's change; never sent.
    bool heldOut = false;
    float camX = 0;
    float camY = 0;
    std::uint32_t cameraFollow = 0;
    bool freeCam = false;
    bool moveHeld = false;
    bool moveHeldBefore = false; ///< last frame's
    /// Its spell book (each spell's level, 0 unknown): a versus game's all at 3,
    /// a quest's the class's first, a solo game's its saved book.
    std::array<std::uint8_t, 0x89> spellLevels{};
    /// noxcpp's multiclass (World::multiclass, a mode's; a noxsdl player's alone): any class's
    /// spells, abilities, guides and weapons, a conjurer's mana. Its abilities then have a book
    /// of their own (`abilityLevels`): a warrior's are spells 1-5's levels (the exe's), which a
    /// multiclass player's spells 1-5 (Anchor, Arachnaphobia, Blind, Blink, Burn) are.
    bool multiclass = false;
    std::array<std::uint8_t, 6> abilityLevels{};
    /// An ability's level (1-5): its own book's for a multiclass player, else spells 1-5's.
    std::uint8_t& abilityLevel(std::size_t a) { return multiclass ? abilityLevels[a] : spellLevels[a]; }
    std::uint8_t abilityLevel(std::size_t a) const { return multiclass ? abilityLevels[a] : spellLevels[a]; }
    /// Whether it uses abilities (a warrior, or multiclass).
    bool usesAbilities() const { return classId == 0 || multiclass; }
    /// Whether it casts spells (not a warrior, or multiclass).
    bool castsSpells() const { return classId != 0 || multiclass; }
    /// Whether its traps are a wizard's glyphs, counted against MaxTrapCount (a wizard, or a
    /// multiclass player not a conjurer: a conjurer's are bombers whatever).
    bool laysGlyphs() const { return classId == 1 || (multiclass && classId != 2); }
    /// Whether its creatures go with it as it leaves (a conjurer's, or a multiclass player's).
    bool keepsCreatures() const { return classId == 2 || multiclass; }
    std::uint32_t attackReady = 0; ///< no new attack before this frame (0: any time)
    std::uint8_t punch = 0;       ///< the punch it's throwing: 0x17 / 0x18 jabs, 0x19 the hook
    bool staffStrike = false;     ///< its staff or spent wand is striking, not blasting
    std::uint8_t animFrame = 0;   ///< its animation's frame, for the update
    std::uint8_t index = 0;       ///< its player index, 0-31
    /// The map polygon it's in (1-based, 0 none, kPolygonUnknown at first) and
    /// that polygon's group (1 outside any).
    std::uint32_t polygon = 0xDEADFACEu;
    std::uint8_t polygonGroup = 0;
    std::uint16_t killType = 0;   ///< what last hurt it: the kill feed's weapon
    std::uint8_t killKind = 0;    ///< 1 a thing's type, 2 a damage type
    float damageCarry = 0;        ///< the fraction of a point of damage left over
    /// The last player to cast on it, the kill
    /// feed's assist within 10 s.
    std::uint32_t assistBy = 0;
    std::uint32_t assistFrame = 0;
    std::int32_t healthSent = -1; ///< what its client was last told (MSG_REPORT_PLAYER_HEALTH)
    // Its equipment (Player.WeaponEquip / ArmorEquip, ud->currentWeapon, the armour
    // value) and its outfit's colours (CLIENT_ACCEPT: pants, shirt 1 and 2, shoes 1 and 2).
    std::uint32_t weaponEquip = 0;
    std::uint32_t armorEquip = 0;
    std::uint32_t currentWeapon = 0;
    float armorValue = 0;
    float armorSent = 0; ///< what its client was last told (MSG_REPORT_ARMOR_VALUE)
    std::uint8_t itemBuffsSent = 0; ///< its unit's itemBuffs its client was last told
    std::array<std::uint8_t, 5> clothing{};
    /// Its info block (the exe's PlayerInfo: the 97 bytes of its MSG_CLIENT_ACCEPT,
    /// which a .plr's Attrib section writes and reads): wchar name[25], u32 speed (+0x32),
    /// strength (+0x36), max mana (+0x3A), max health (+0x3E), u8 class (+0x42), female (+0x43),
    /// five rgb colours (+0x44), clothing (+0x53), +0x58, wchar suffix[4]. Ours keeps the name,
    /// speed, strength, class and clothing in their own fields (those count); the host copies
    /// the join's block here (Plr_CreateFromJoinData) for the rest.
    std::array<std::uint8_t, 97> info{};
    /// Its speed as plrReadVals rounds it (info + 0x32).
    std::uint32_t speed = 0;
    /// Its journal (newest first: Plr_AddJournalEntry): each entry's
    /// name (at most 63 letters) and type.
    struct JournalEntry {
        std::string name;
        std::uint16_t type = 0;
    };
    std::vector<JournalEntry> journal;
    /// The map name its .plr's GameData last wrote or read (32 bytes: the name,
    /// its end, and whatever a longer name left after it).
    std::array<char, 32> savedMapName{};
    /// Its trade (a shop or another player's, World::trades by id; 0 none: trade.hpp).
    std::uint32_t trade = 0;
    /// Its .plr being applied (its bit in a global mask): no
    /// MSG_INFORM strings to it (SrvNet_SendInformString), no sound from it in a quest
    /// (aud), no item used (Obj_UseItem), no weapon worn as it's picked up
    /// (Obj_WeaponPickup), no ability award told to the others (Abil_GrantToPlayer); a quest's
    /// loaded weapons and armour keep their health (WeaponXfer / ArmorXfer).
    bool transfer = false;
    /// noxcpp: a mode holds it dead (ModeHooks): neither the Action key nor the automatic
    /// respawn brings it back; the mode does (Fight::respawn), and clears this.
    bool respawnHeld = false;
    bool reportStamina = false;   ///< its client is owed MSG_REPORT_STAMINA
    /// (set by plrReadVals) its client is owed MSG_REPORT_STATS
    /// after its totals (playerReportAnything).
    bool reportStats = true;
    // Mana (ManaCur, ManaMax; a class's MaxMana), and what its client was told.
    std::uint16_t mana = 0;
    std::uint16_t maxMana = 0;
    std::int32_t manaSent = -1;      ///< -1: owed all of it (MSG_REPORT_TOTAL_MANA)
    /// A spell being spoken (the pending-cast list's magicEnt, Spell_StartCast): up to
    /// five spells (a glyph's), the one being spoken, its phonemes spoken, when the next one is,
    /// whether it's cast on the caster (MSG_TRY_SPELL's last byte), and whether it builds a
    /// trap (the list holds Glyph).
    struct Chant {
        std::array<std::uint32_t, 5> spells{};
        int count = 0;
        int index = 0;
        int spoken = 0;
        std::uint32_t next = 0;
        bool targetSelf = false;
        bool trap = false;
        /// Its node in the phoneme tree (from the root, each spell's own).
        std::optional<std::vector<std::uint8_t>> node = std::vector<std::uint8_t>{};
    };
    std::optional<Chant> chant;
    /// A spell being spoken by book or by hand (Plr_UpdateState's gestures): when it began
    /// (SpellCastStart, 0: none); the phoneme tree's node it has reached (
    /// SpellPhonemeLeaf, as the phonemes from the tree's root: spells.hpp's walkPhoneme; nullopt
    /// off the tree, as at first); whether its own phonemes are sent it (set while a
    /// book's spell is spoken); where a queued spell goes and what a spoken
    /// spell is cast at (set as a book's spell ends).
    std::uint32_t castStart = 0;
    std::optional<std::vector<std::uint8_t>> phonemeLeaf;
    bool hearsOwnPhonemes = false;
    float castX = 0;
    float castY = 0;
    std::uint32_t spellTarget = 0;
    /// The trap being built (TrapSpells and their count): the spells spoken for
    /// a glyph so far, their mana paid. A wizard's glyphs out (CurTraps).
    std::array<std::uint32_t, 5> trapSpells{};
    std::uint8_t trapCount = 0;
    std::uint8_t curTraps = 0;
    /// The frame's control events (the 128-entry control buffer, SrvNet_OnPlayerInput), oldest first;
    /// `active` false for those the de-duplication dropped.
    struct Event {
        std::uint8_t code = 0;
        std::uint32_t data = 0;
        bool active = true;
    };
    std::vector<Event> controls;
};

/// What a client is sent of an object, and how often it moves (clock.hpp).
enum class Priority : std::uint8_t {
    Low,  ///< the frame's: elevators, effects, sounds, scenery
    High, ///< every substep: characters, missiles and spells
};

/// Counts each time any object's record of a kind comes or goes (ServerObject::player's and
/// sentry's watches), so the worlds' lists of them know when to look again (World::playerUnits,
/// World::sentryUnits).
template <int Kind>
struct RecordWatch {
    static inline std::atomic<std::uint32_t> changes{0};
    static void changed() noexcept { changes.fetch_add(1, std::memory_order_relaxed); }
};
using PlayerRecordWatch = RecordWatch<0>;
using SentryRecordWatch = RecordWatch<1>;
using BallRecordWatch = RecordWatch<2>;

/// ServerObject::pendingDelete: a bool that also counts, process-wide, each time any object is
/// marked (set, copied or made marked), so the frame's deletions pass can tell that nothing has
/// been marked since it last looked without walking every object (Server::updateWorld).
class DeleteMark {
public:
    DeleteMark() = default;
    DeleteMark(bool marked) noexcept : marked_(marked) { count(); }
    DeleteMark(const DeleteMark& o) noexcept : marked_(o.marked_) { count(); }
    DeleteMark& operator=(const DeleteMark& o) noexcept { return *this = o.marked_; }
    DeleteMark& operator=(bool marked) noexcept
    {
        marked_ = marked;
        count();
        return *this;
    }
    operator bool() const noexcept { return marked_; }

    /// How many times any object has been marked so far (it only grows, wrapping).
    static std::uint32_t marks() noexcept { return marks_.load(std::memory_order_relaxed); }

private:
    void count() const noexcept
    {
        if (marked_) {
            marks_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    static inline std::atomic<std::uint32_t> marks_{0};
    bool marked_ = false;
};

/// One thing in the world: a player's unit, a monster, an item, a missile, a door, scenery... (the
/// exe's object record). The common fields are inline; the rare, big parts (a player's, a
/// monster's, a trigger's...) are Boxed, on the heap, so most objects stay small.
struct ServerObject {
    struct Monster;
    // First, together, what the frame's walks of every object (its start and end passes, the
    // units update, its last pass) read of each: a line or two of the object's, not a line here
    // and there across it.
    std::uint32_t extent = 0;   ///< the map's number for it; the network's id
    std::int32_t scriptId = 0;  ///< what scripts call it by (Object("name") returns it)
    std::uint16_t type = 0;     ///< thing.bin index, 1-based (0: unknown)
    DeleteMark pendingDelete;   ///< Delete: gone at the end of the frame (delayedDeleteObject)
    bool updates = false;       ///< has an UPDATE function
    bool collides = false;      ///< has a COLLIDE function
    /// Moves with the frame though it wouldn't join the motion (put on the updatable list by
    /// hand, unitAddToUpdatable: a cleansing flame).
    bool alwaysMoves = false;
    Name typeName;
    std::uint32_t classes = 0;
    std::uint32_t subclasses = 0;
    Name scriptName;
    float x = 0;
    float y = 0;
    float newX = 0;      ///< where this frame's steps have it (NewPos)
    float newY = 0;
    /// Its classic position, this frame's; x / y are this, carried on by the substeps for an
    /// important object (clock.hpp).
    float frameStartX = 0;
    float frameStartY = 0;
    std::uint32_t flags = ObjFlag::Enabled;
    std::uint32_t holder = 0;   ///< the extent whose inventory it's in; 0: on the ground
    float vx = 0;        ///< per classic frame (a mobile object's: its velocity, the exe's VelVec)
    float vy = 0;
    float forceX = 0;    ///< this frame's own push, walking (ForceVec)
    float forceY = 0;
    float z = 0;
    float zVel = 0;      ///< its height's change a frame
    std::uint32_t decayAt = 0; ///< gone at this frame (unitSetDecayTime; 0 never)
    std::uint32_t hurtBy = 0;  ///< what last hurt it (with hurtType and hurtFrame, below)
    Boxed<PlayerControl, PlayerRecordWatch> player; ///< a player's unit (on the heap: boxed.hpp)
    Boxed<Monster> monster;                          ///< a monster's AI (Monster, below; on the heap)
    Name updateFn; ///< its UPDATE function's name ("": none)
    // (The rest.)
    float bounce = 0; ///< a bouncer's bounce (0.1 x it of its fall back up)
    std::uint32_t timer = 0; ///< its own next or last frame (a cloud's tick, a shower's meteor)
    std::uint16_t soundFrame = 0; ///< the frame of its last repeated sound (blue flames' hiss)
    float destX = 0;         ///< a TeleportWake's destination (its CollideData)
    float destY = 0;
    float homeX = 0;         ///< its own spot: a cleansing flame's origin, the shaft it's falling down
    float homeY = 0;
    float fallToX = 0;       ///< where one falling down a shaft comes out: its elevator
    float fallToY = 0;
    float lastX = 0;         ///< (PrevPos) where it was a frame ago, for those whose update asks
    float lastY = 0;
    std::uint32_t expiresAt = 0;   ///< a pixie's end
    std::uint32_t seenOwnerAt = 0; ///< a pixie's last sight of its owner
    std::uint32_t castBy = 0;      ///< the spell that made it
    /// A spell projectile's (Magic) first caster, kept when it's
    /// turned (its owner changes); its spell is castBy, its power value.
    std::uint32_t flyCaster = 0;
    /// noxcpp: the mode events it opted into (mode_hooks.hpp ModeEvent: Death, Damage...): asked
    /// or told to World::modeHooks for it alone. 0: none, as every object of the game's. (In the
    /// padding before `value`: the object's size is the same.)
    std::uint8_t modeEvents = 0;
    int value = 0;           ///< its own number (a fist's or meteor's damage, a cloud's time)
    // Poison (its level; frames to its next fall, and a slowed one's; the
    // frame it began).
    std::uint8_t poison = 0;
    std::int16_t poisonTimer = 0;
    std::uint32_t poisonedAt = 0;
    // Motion (physics.hpp; its velocity, force and NewPos at the object's start).
    float pushX = 0;     ///< the collisions' push this step
    float pushY = 0;
    float drag = 0.5f;   ///< 0.5, a missile's 1
    float mass = 1;
    ShapeKind shape = ShapeKind::None;
    float radius = 0;    ///< a circle's
    float boxWidth = 0;  ///< a box's
    float boxHeight = 0;
    /// noxcpp: a mode's own size for a missile (a script's nox.setHitRadius and
    /// nox.setBurstScale); none for every thing of the game's. While `hit` is set its shape above
    /// is the mode's (its type's kept here to give back), and its flight meets what that shape
    /// touches (Missiles::bodyAt), not only what holds its centre. Its burst's reach and push
    /// reach are its type's times `burstScale`. Kept when it's reflected or turned (the same
    /// object, now another's); dies with it. `ownerCleared`: the owner (by extent) its flight has
    /// first come clear of; until then that owner is met only as the stock test meets it.
    /// `drawScale` (nox.setDrawScale, any thing the stream sends, not only a missile) changes
    /// nothing on the server: it's what NoX Core clients of client level 5 are told to draw it at
    /// (GameHost::Options::onScaledSeen).
    struct ModSize {
        ShapeKind typeShape = ShapeKind::None;
        float typeRadius = 0;
        float typeBoxWidth = 0;
        float typeBoxHeight = 0;
        bool hit = false;
        float burstScale = 1;
        std::uint32_t ownerCleared = 0;
        float drawScale = 1;
        /// Nothing of a mode's left (the part let go; `ownerCleared` matters only with `hit`).
        bool idle() const noexcept { return !hit && burstScale == 1.0f && drawScale == 1.0f; }
    };
    Boxed<ModSize> modSize;
    float speedBase = 0;   ///< SPEED / 32, a player's from its class
    float speedBonus = 0;
    float speedCur = 0;    ///< this frame's: (5 - slowed) x (base + bonus) / 5, doubled running
    std::uint8_t slowed = 0;
    /// In the motion queue (physics.hpp queueMotion), which the
    /// frame's five steps run over, in the order they joined (`motionTicket`), and then empty.
    bool inMotion = false;
    /// Its entry in the map grid and the units update's lists (World::grid: the exe's
    /// grid nodes, CollideP, the updatable list's links).
    GridSlot grid;
    std::uint64_t motionTicket = 0;
    /// Made and not yet through the new things' pass (unitsNewAddToList, at a
    /// tick's end), which puts one with a COLLIDE in the motion.
    bool fresh = true;
    // Health (HealthData; max 0: none), and what last hurt it, with what and when.
    std::uint16_t health = 0;
    std::uint16_t maxHealth = 0;
    /// Whether it has HealthData at all (its type's HEALTH line, 0 or not: the street clothes,
    /// potions, shopkeepers... have one at 0), which the sentry's ray and the wear ask.
    bool healthData = false;
    /// HealthData: the HEALTH line, any health, or a creature (every monster's and the player's
    /// type has the line).
    bool hasHealthData() const noexcept { return healthData || maxHealth != 0 || (classes & 0x6u) != 0; }
    /// An item's wear not yet taken off its health (the fraction of a point:
    /// equipDamage / playerDamageWeapon).
    float wear = 0;
    /// A player's experience; anything else's worth to its killer (its type's EXPERIENCE).
    float experience = 0;
    std::uint8_t hurtType = 0;
    std::uint32_t hurtFrame = 0;
    float hurtFromX = 0; ///< where the blow came from (its source's last position)
    float hurtFromY = 0;
    // Enchantments (enchant.hpp): the bits (Buffs), each one's frames left (BuffsDur, 0: for
    // good) and power (BuffsPower).
    std::uint32_t buffs = 0;
    std::array<std::uint16_t, 32> buffTime{};
    std::array<std::uint8_t, 32> buffPower{};
    /// Players (a bit each, by slot) owed MSG_REPORT_ENCHANTMENT, and
    /// those who know it (playersKnowMask), which decides who's owed one when it changes.
    std::uint32_t enchantOwed = 0;
    std::uint32_t knownBy = 0;
    /// An NPC's report (a state, the same for every player, set by
    /// setNPCColor / npcSetItemEquipFlags through) and
    /// the players owed MSG_REPORT_NPC (those who know it at the change, and
    /// one that gets to know it while the state is set; npc.hpp).
    bool npcState = false;
    std::uint32_t npcOwed = 0;
    /// An item's modifiers (InitData's four: effectiveness, material, enchantments): modifier.bin
    /// ids, 0xFF for none.
    std::array<std::uint8_t, 4> modifiers{0xFF, 0xFF, 0xFF, 0xFF};
    /// The players owed MSG_REPORT_MODIFIER (a weapon, armour or flag, class
    /// 0x13001000, as it gets known's 0x200, or its modifiers set,
    /// modifSetItemAttrs): SrvNet_SendReportModifier's its four ids.
    std::uint32_t modifierOwed = 0;
    /// A player's or a generator's health as each player last had it (by slot),
    /// for MSG_REPORT_HEALTH_DELTA's floating numbers
    /// (SrvNet_SendPlayerObj / SrvNet_SendSimpleObj). (A monster's: Monster::healthSeen.)
    std::array<std::uint16_t, 32> healthSeen{};
    /// (itemBuffs) What its worn enchantments give it, set as they're put on
    /// (item_effects.hpp): 1 fire, 2 poison, 4 lightning protection, 8 brilliance, 0x10 speed,
    /// 0x20 regeneration. A player's client is told (MSG_REPORT_ITEM_ENCHANTMENT 0x5B).
    std::uint8_t itemBuffs = 0;
    /// A weapon's or armour's item flags (WeaponArmorUpdate's, after the wear): bit 1
    /// set as it's given as an outfit item (playerRespawnItem); kept by its xfer
    /// (WeaponXfer from version 0x40, ArmorXfer from 0x3E: a .plr's).
    std::uint32_t itemFlags = 0;
    std::uint16_t material = 0;      ///< MATERIAL bits (FLESH 1 ... METAL 0x10 ... NONE 0x4000)
    std::uint8_t weight = 0;          ///< WEIGHT
    std::uint16_t carryCapacity = 0;  ///< CARRYCAPACITY (an arena player's: from its strength)
    Name pickup;                      ///< its PICKUP function's name ("": DefaultPickup)
    Name dropFn;                      ///< its DROP function's name ("": DefaultDrop)
    Name useFn;                       ///< its USE function's name ("": none)
    int useArg = 0;                   ///< its USE's number (PotionUse 50)
    std::uint32_t owner = 0;          ///< ObjOwner (a missile's shooter)
    /// When it was put in its owner's list (unitSetOwner puts the newest first:
    /// firstOwned / nextOwned), for the creatures (World::ownedCount); the lists' order.
    std::uint32_t ownedOrder = 0;
    std::uint32_t born = 0;           ///< the frame it was made
    // A quiver's or a chakram's ammunition (UseData: max, left, infinite).
    std::uint8_t charges = 0;
    std::uint8_t maxCharges = 0;
    bool infiniteAmmo = false;
    // A wand's USE (WandUse: charges, its missile, shots a second, SINGLE / MULTI_SHOT,
    // sound): what it fires, the frames between shots (FPS / shots a second), the last shot.
    Name wandMissile;              ///< "": not a WandUse wand (WandCastUse: spells, not yet)
    std::uint32_t wandDelay = 0;
    bool wandMultiShot = false;
    std::uint16_t wandSound = 0;   ///< WandUse's sound after a shot (0 none)
    std::uint32_t wandLastShot = 0;
    /// A staff that casts (WandCastUse: charges, uses a second, its spell): the spell
    /// and its casting flag (set as it casts, off at the end of a
    /// lasting spell it cast).
    std::uint32_t wandSpell = 0;
    bool wandCasting = false;
    /// A wand's or staff's charge out of 100 (set from its charges as it's used;
    /// what an obelisk tops up, rechargeItem).
    int chargePercent = 100;
    bool staffMelee = false;       ///< a spent wand swung as a staff
    // Its COLLIDE function and argument ("SparkExplosionCollide 128": a fireball's size).
    Name collideFn;
    int collideArg = 0;
    int collideArg2 = 0; ///< its second number (MonsterArrowCollide's outside a solo game)
    std::uint8_t collideType = 0; ///< its COLLIDE's damage type (DamageCollide 2 DAMAGE_FLAME: 1)
    /// Its DIE function's name ("": none, it's just deleted) and its first argument (the type
    /// CreateObjectDie / SpawnObjectDie make: DeathData) and the sound after it (
    /// 0 none).
    Name dieFn;
    Name dieType;
    std::uint16_t dieSound = 0;
    /// Its DAMAGE function's name ("": DefaultDamage's, or PlayerDamage's for a player).
    Name damageFn;
    Name damageSoundFn; ///< DAMAGESOUND (PlayerDamageSound; else DefaultDamageSound's)
    std::uint32_t seekTarget = 0; ///< a homing missile's target
    /// A thrown round chakram in flight (ChakramInMotion's UpdateData: the chakram itself rides
    /// in its inventory). Bounces left, what it homes on, what it last hit,
    /// where its thrower is, and its leg: 2 out, 0 home, 1 lost.
    struct Chakram {
        std::uint8_t bounces = 4;
        std::uint8_t leg = 2;
        std::uint32_t target = 0;
        std::uint32_t lastHit = 0;
        float homeX = 0;
        float homeY = 0;
    };
    Boxed<Chakram> chakram;
    /// A Glyph's trap (its InitData, GlyphInit): its spells (five at most) and
    /// their count, what set it off and the point its spells go at.
    struct Trap {
        std::array<std::uint32_t, 5> spells{};
        std::uint8_t count = 0;
        std::uint32_t target = 0;
        float x = 0;
        float y = 0;
    };
    Boxed<Trap> trap;
    /// A map script handler (the exe's event block, read by xferReadScriptHandler): its flags (2 runs
    /// once, 1 has run) and function (by name here; none: empty).
    struct ScriptBlock {
        ScriptBlock() = default;
        ScriptBlock(std::uint32_t f, Name fn) : flags(f), function(fn) {}
        std::uint32_t flags = 0;
        /// noxcpp: the mode's handler (ModeHooks::scriptBlock), not the map script's: `function`
        /// is the mode's own name for it. Never set by a map or a save. (In the padding before
        /// `function`.)
        bool mode = false;
        Name function;
    };
    static_assert(sizeof(ScriptBlock) == sizeof(std::uint64_t) + sizeof(Name), "ScriptBlock::mode must sit in the padding");
    /// Its pickup handler (ScriptPickup, the header's from version 0x3F:): run once
    /// when it's picked up (Obj_TryPickup), its function then none (index -1), as the save
    /// writes it (an empty name and its flags).
    ScriptBlock onPickup;
    /// A trigger's (pressure plates, buttons, levers, switches: TriggerUpdateData, TriggerXfer
    /// unitTriggerXfer). Its flags (1 touched this frame, 2 once only, 4 touched last frame, 8 set
    /// up), what touched it, its state (0 up, 1 down, 3 a toggle down and let go, 5
    /// spent), its handlers (on a touch, deciding; pressed; released), what may
    /// press it (classes, not these; team, not this team).
    struct Trigger {
        std::uint32_t flags = 0;
        std::uint32_t toucher = 0;
        std::uint8_t state = 0;
        ScriptBlock collided;
        ScriptBlock pressed;
        ScriptBlock released;
        std::uint32_t allowedClasses = 0;
        std::uint32_t ignoredClasses = 0;
        std::uint8_t allowedTeam = 0;
        std::uint8_t ignoredTeam = 0;
    };
    Boxed<Trigger> trigger;
    /// A teleport pentagram's (TeleportPentagram / InvisiblePentagram, TransporterXfer):
    /// its partner (by extent: a map load links it, World::linkTransporters; the record's number
    /// for it is what a save writes), its state (0 at rest, 1 sending, 2
    /// receiving), its frame and the counts that pace it (the cycle, the frames
    /// into one), and whether something touched it this frame.
    struct Transport {
        std::uint32_t link = 0;
        std::uint8_t state = 0;
        std::uint8_t cycle = 0;
        std::uint8_t step = 0;
        std::uint8_t frame = 0;
        bool touched = false;
    };
    Boxed<Transport> transport;
    /// Its animation frame (servMarkObjAnimFrame), and the players (a bit
    /// each) owed MSG_REPORT_ANIMATION_FRAME.
    std::uint32_t animFrame = 0;
    std::uint32_t animOwed = 0;
    /// The players owed its being turned on or off (MSG_ENABLE / DISABLE_OBJECT,
    /// unitSetOnOff).
    std::uint32_t enableOwed = 0;
    /// The players it's a friend of (not their enemy, monsterMarkUpdate),
    /// and those owed the change (MSG_OBJECT_FRIEND_ADD / _REMOVE, netSendObjects2Plr).
    std::uint32_t friendOf = 0;
    std::uint32_t friendOwed = 0;
    /// Its extra drawing state (xStatus: a pit's 2 shut, 4 opening, 8 open...), and the players
    /// owed it (MSG_REPORT_X_STATUS).
    std::uint32_t xStatus = 0;
    std::uint32_t xStatusOwed = 0;
    /// The players (a bit each, by slot) owed its height (MSG_REPORT_Z_PLUS / _MINUS),
    /// set by raise().
    std::uint32_t heightOwed = 0;
    /// A pit's (HoleXfer, its CollideData): its handler on the first step on it shut,
    /// where what falls in comes out, when it opens (0: never; the frames
    /// from the step) and whether it's been stepped on.
    struct Pit {
        ScriptBlock stepped;
        float fallX = 0;
        float fallY = 0;
        std::uint32_t opensAt = 0;
        std::uint16_t delay = 0;
        bool sprung = false;
    };
    Boxed<Pit> pit;
    /// A shooting trap's (SkullUpdate: arrow traps, fire-breathing skulls; UpdateData): its
    /// missile (the UPDATE's word), the frames to its next look and next shot,
    /// whether it saw a target and whether it was on last frame.
    struct Shooter {
        std::string missile;
        int look = 0;
        int reload = 0;
        bool sees = false;
        bool wasOn = false;
    };
    Boxed<Shooter> shooter;
    /// A sentry globe's (SentryXfer, UpdateData): its beam's way now (radians),
    /// where it starts again when turned off and its turn a frame. The beam's end is
    /// homeX / homeY.
    struct Sentry {
        float angle = 0;
        float start = 0;
        float speed = 0;
    };
    Boxed<Sentry, SentryRecordWatch> sentry;
    /// A mover's (MoverXfer, UpdateData): its state (0 to start, 1 moving, 2
    /// paused, 3 done), speed (x 0.25 its SPEED), first waypoint, the waypoint it heads
    /// for and the one before (by id here), and what it carries.
    struct Mover {
        std::uint8_t state = 0;
        int speed = 0;
        std::int32_t start = 0;
        std::int32_t target = 0;
        std::int32_t previous = 0;
        /// The map number of the thing it carries (its record's; the save writes it) and the
        /// thing it found by it (its extent, 0 none yet): the exe finds it at an update that has
        /// none (netGetUnitByExtent: World::byMapNumber) and keeps it till it's off the map.
        std::uint32_t carried = 0;
        std::uint32_t carriedObject = 0;
    };
    Boxed<Mover> mover;
    /// A CTF flag's (FlagUpdate's UpdateData): its home (where TeamXfer put it) and the
    /// frame it was dropped (0 at home or carried).
    struct Flag {
        float homeX = 0;
        float homeY = 0;
        std::uint32_t dropped = 0;
    };
    Boxed<Flag> flag;
    /// The flagball's (BallUpdate's UpdateData): the player who last had it
    /// and its team, when it was last held (the exe's ms clock; here the frame), when the
    /// player took it, how long it may keep it (FlagballPossDuration frames) and
    /// the speed under which a loose ball is nobody's (FlagballResetVel).
    struct Ball {
        std::uint32_t toucher = 0;
        std::uint8_t toucherTeam = 0;
        std::uint32_t heldAt = 0;
        std::uint32_t touchedAt = 0;
        std::uint32_t possession = 0;
        float resetSpeed = 0;
    };
    Boxed<Ball, BallRecordWatch> ball;
    /// A crown's (CrownUpdate's UpdateData): who it goes back to (CrownDrop's choice of its
    /// team's player, or a dead king's killer; taken by them on its next update).
    struct Crown {
        std::uint32_t returnTo = 0;
    };
    Boxed<Crown> crown;
    /// A monster's AI state (MonsterUpdateData, monsters.hpp).
    struct Monster {
        /// A saved map's ActionData (read, from MonsterXfer version 0x29): where the exe
        /// keeps script ids in the object fields till turns them into objects
        /// (the stack's by waypoints by id server_getWaypointById). Ours keeps
        /// them here, the fields themselves 0, till the load's fix-ups resolve them: what it
        /// sees (`seenCount` is set), its enemy, what it heard
        /// from, its last enemy, its focus, the transits
        /// and each stack entry's object arguments (type 1: script ids) and waypoint
        /// arguments (type 2: waypoint ids), by entry and argument.
        struct SavedRefs {
            std::vector<std::uint32_t> seen;
            std::uint32_t enemy = 0;
            std::uint32_t heardFrom = 0;
            std::uint32_t lastEnemy = 0;
            std::uint32_t focus = 0;
            std::vector<std::uint32_t> transits;
            std::array<std::array<std::uint32_t, 2>, 24> stackArgs{};
        };
        std::optional<SavedRefs> savedRefs;
        const things::MonsterDef* def = nullptr;
        std::uint32_t status = 0;                ///< StatusFlags (MonsterStatus; 0x200 hurt)
        float aggression = 0.5f;
        float aggression2 = 0.5f;
        float sightRange = 150.0f;
        float field329 = 30.0f;  // escort distance: it stays within this (plus 30) of whom it escorts
        float field330 = 0.5f;  // missile aim accuracy, 0..1: the lower, the wider its shots spread
        std::uint8_t field331 = 0x1E;  // an NPC's strength (unitGetStrength reads it)
        float field332 = 0.5f;                   ///< a speed stat
        std::uint8_t field333 = 0xFF;            ///< roam flags
        float retreatLevel = 0;
        float resumeLevel = 0;
        float field338 = 1.0f;  // its starting health as a fraction of the full
        float fleeRange = 0;
        int startAction = 0;                     ///< the action it starts with (38: none)
        bool initialised = false;                ///< (MonsterInit run)
        /// The action stack: an action, its arguments (positions as float bits, objects by
        /// extent), whether its start has run.
        struct Item {
            int action = 0;
            std::array<std::uint32_t, 4> args{};
            bool started = false;
        };
        std::array<Item, 24> stack{};
        int top = 0;                             ///< AIStackInd
        bool stackChanged = false;               ///< (a push or pop this pass)
        std::uint32_t enemy = 0;                 ///< CurrentEnemy
        std::uint32_t actionFrame = 0;           ///< when the stack last changed
        /// Its sight (unitUpdateSightMB): the creatures it sees (at most
        /// 16, and their count), the enemy it had before, when it last
        /// looked about, when it may next, when it last checked it can still
        /// reach them, the one it keeps to while it sees it, the
        /// nearest player's distance at its last look (-1 none within 1000).
        std::array<std::uint32_t, 16> seen{};
        std::uint8_t seenCount = 0;
        std::uint32_t lastEnemy = 0;
        std::uint32_t lookedAt = 0;
        std::uint32_t nextLook = 0;
        std::uint32_t reachCheckedAt = 0;
        std::uint32_t focus = 0;
        float nearestPlayer = 0;
        std::uint32_t sightedSoundAt = 0;        ///< (its next "enemy sighted" sound)
        std::int16_t lookDelay = 0;              ///< (MonsterXfer: its LookingForEnemy's moment idle)
        /// Getting stuck (the main AI): since when it's been within 15 of stuckX / stuckY (field1f0).
        std::uint32_t field1f0 = 0;
        float stuckX = 0;
        float stuckY = 0;
        std::uint32_t movedAwayAt = 0;           ///< (fleeing and stuck; a move's attempt)
        std::uint32_t nextMelee = 0;
        std::uint32_t hurtHandledAt = 0;         ///< (the last hurt it reacted to)
        std::uint32_t idleSoundAt = 0;           ///< (its next idle sound)
        std::uint32_t tauntSoundAt = 0;          ///< (its next taunt after a blow lands)
        /// What it heard: whether, whose, where, when
        /// and how loud.
        std::uint32_t heard = 0;
        std::uint32_t heardFrom = 0;
        float heardX = 0;
        float heardY = 0;
        std::uint32_t heardAt = 0;
        int heardLevel = 0;
        /// What its last blow is put down to (DefaultDamage: 0 at each blow; 1 the
        /// striker's or the weapon's type, else 2 the damage type), as a player's kill
        /// feed (killKind / killType). Kind 2 type 2 at its death: a Berserker kill.
        std::uint32_t killKind = 0;
        std::uint32_t killType = 0;
        /// The facing it turns to (Direction2), taken at the motion's end (-1: none).
        int turnTo = -1;
        /// An NPC's (subclass 0x10) worn things (npc.hpp): its worn weapons' equip-flag bits
        /// (weaponInventoryEquipFlags's) and armour's (
        /// unitArmorInventoryEquipFlags's), the weapon it holds (not a quiver; 0 none), its
        /// fist's animation (0 till its first bare-handed blow draws one) and its
        /// armour value (recalculateArmorVal's).
        std::uint32_t weaponFlags = 0;
        std::uint32_t armorFlags = 0;
        std::uint32_t weapon = 0;
        std::uint8_t punch = 0;
        float armorValue = 0;
        /// A blow's fraction of a point carried to the next (playerDecrementHPMana
        /// for an NPC's PlayerDamage).
        float damageCarry = 0;
        /// Its colours (six RGB: setNPCColor; a female NPC's MonsterCreate
        /// sets them all 0xD2AE79), sent in MSG_REPORT_NPC.
        std::array<std::array<std::uint8_t, 3>, 6> colors{};
        /// Its sound set: unset, MonsterCreate's (getDefaultSoundSet
        /// by its type's name); set by an NPC's voice (readNPCVoiceSet ->
        /// setNPCVoiceSet), the set's name ("" none: a name no set has).
        std::optional<std::string> soundSet;
        /// Its path (pathfind.hpp): the points (32), how many (0: none), the one it's
        /// on, where it leads, when it was made, how its search ended
        /// (0 reached, 1 cut short, 2 no further), when one was last cut short.
        std::array<std::pair<float, float>, 32> path{};
        int pathCount = 0;
        int pathIndex = 0;
        float pathToX = 0;
        float pathToY = 0;
        std::uint32_t pathAt = 0;
        std::uint8_t pathStatus = 0;
        std::uint32_t blockedAt = 0;
        /// Its waypoints (16 (and the one the exe writes past them); a ROAM's history
        /// too): by index + 1; how many, the one it's at,
        /// where they lead.
        std::array<std::uint32_t, 17> waypoints{};
        int waypointCount = 0;
        int waypointIndex = 0;
        float waypointsToX = 0;
        float waypointsToY = 0;
        /// The transporters and elevators on its path (at most 8, and how many).
        std::vector<std::uint32_t> transits;
        /// Its spells (stage D): by spell id (137), the ways it uses each (0x8000000
        /// against a spell coming at it, 0x10000000 on itself, 0x20000000 at an enemy,
        /// 0x40000000 a summons or a hold on an enemy, 0x80000000 to get away); the least and
        /// most frames between such casts (those five in turn) and the
        /// frame each may next; its power.
        std::array<std::uint32_t, 137> spells{};
        std::array<std::uint16_t, 10> spellDelays{0, 15, 90, 300, 90, 180, 0, 90, 0, 180};
        std::array<std::uint32_t, 5> spellAt{};
        int spellPower = 1;
        /// A bomber's glyph's spells (MonsterXfer's), and what it ran into
        /// (BombCollide: its glyph's target).
        std::array<std::uint32_t, 3> bombSpells{};
        std::uint32_t bombTarget = 0;
        /// Its health as each player was last told it (by player index: the health
        /// deltas, MSG_REPORT_HEALTH_DELTA, SrvNet_SendComplexObj).
        std::array<std::uint16_t, 32> healthSeen{};
        /// MonsterXfer's: monster.bin's resume and retreat levels and status
        /// over the map's; the map's health kept; the type's own spells
        /// set again (monsterAutoSpells).
        std::uint8_t resumeFromDef = 1;
        std::uint8_t retreatFromDef = 1;
        std::uint8_t statusFromDef = 1;
        std::uint8_t healthFromMap = 0;
        std::uint8_t typeSpells = 1;
        /// Whom it escorts (MonsterXfer: a script name, "**PLAYER**" or "**OWNER**"),
        /// looked up once by ESCORT.
        std::string escortName;
        /// Its animation (updateNPCAnimData): the MonsterDraw states' frames,
        /// delays and looping (its type's table), the frame and its delay count
        /// and whether it ran out.
        struct Anim {
            std::uint8_t frames = 0;
            std::uint8_t delay = 0;
            bool loop = false;
        };
        std::array<Anim, 16> anims{};
        bool hasAnims = false;
        std::uint8_t animFrames = 0;             ///< its animation's frame count, as last read
        std::uint8_t animFrame = 0;
        std::uint8_t animDelay = 0;
        std::uint8_t animDone = 0;               ///< its animation ran out
        std::uint32_t polygon = 0xDEADFACEu;     ///< the map polygon it's in (kPolygonUnknown at first)
        std::uint32_t deadFrames = 0;            ///< a zombie's time dead
        float homeX = 0;
        float homeY = 0;
        int homeDirection = 0;
        std::array<std::uint16_t, 32> healthGraph{}; ///< HealthGraph
        std::uint8_t stamina = 0;
        std::uint32_t hurtSoundAt = 0;
        std::uint8_t field523 = 0;               ///< a countdown
        /// The map's script hooks (MonsterXfer).
        ScriptBlock onEnemySighted, onDeath, onLookingForEnemy, onChangeFocus, onIsHit, onRetreat, onCollision,
            onHearEnemy, onEndOfWaypoint, onLostEnemy;
        /// Its dialog (dialog.hpp; ActionData saves them): the start and end functions
        /// (script function indices; -1 none, as MonsterCreate sets them), its type
        /// (0 NORMAL, 1 YESNO, 2 NEXT, 3 YESNONEXT), the answer kept and its
        /// portrait (StoryPic's).
        std::int32_t dialogStart = -1;
        std::int32_t dialogEnd = -1;
        std::uint8_t dialogType = 0;
        std::uint8_t dialogAnswer = 0;
        std::string portrait;
    };
    /// A monster generator's (MonsterGeneratorXfer, its UpdateData): the monsters it
    /// makes, four at most for each of the three difficulties (whole monster records out of
    /// the world, with what they carry); its hurt, death, spawn and touch
    /// handlers; its spawn rate and most at a time by difficulty, how many
    /// are out and its most now (MonsterGeneratorInit's), when it last made one
    /// and where (1 by a player within 300, 2 where a player sees it).
    struct Generator {
        struct Template {
            std::shared_ptr<ServerObject> object;
            std::vector<std::shared_ptr<ServerObject>> items;
        };
        std::array<std::vector<Template>, 3> groups;
        ScriptBlock onHurt, onDie, onSpawn, onCollide;
        std::array<std::uint8_t, 3> rate{};
        std::array<std::uint8_t, 3> mostBy{};
        std::uint8_t active = 0;
        std::uint8_t most = 0;
        std::uint32_t lastSpawn = 0;
        std::uint32_t flags = 2;
        bool initialised = false;
    };
    Boxed<Generator> generator;
    std::uint32_t spawnedBy = 0; ///< (a monster's) the generator that made it
    /// An exit's map (ExitXfer: CollideData; empty: none).
    Name exitMap;
    /// And where a warp gate puts out those it turns away (ExitXfer from version 0x1F,
    /// its CollideData).
    float exitX = 0;
    float exitY = 0;
    /// A sign's text (ReadableXfer: a string table id, its UseData).
    Name text;
    /// When a player last read it (ReadUse / WarpReadUse; 0 never).
    std::uint32_t readAt = 0;
    /// A RewardMarker's or RewardMarkerPlus's (its InitData: RewardMarkerCreate's, then
    /// RewardMarkerXfer's; rewards.hpp): the kinds it may give (1 spell book, 2
    /// ability book, 4 field guide, 8 weapon, 0x10 armour, 0x20 gold or a gem, 0x40 potion, 0x80
    /// the ankh's), which of the lists it draws from (1 spells, 2 abilities, 4 guides), the
    /// lists (by spell, by ability, by guide creature), its odds (1, 2, 3, 4
    /// give on a roll of 0-100 at most 75, 50, 25, 5; else always) and its flags (1 always
    /// placed, 0x80 chosen to be).
    struct RewardMarker {
        std::uint32_t kinds = 0xFF;
        std::uint32_t listed = 0;
        std::array<bool, 0x89> spells{};
        std::array<bool, 6> abilities{};
        std::array<bool, 0x29> guides{};
        std::uint32_t chance = 0;
        std::uint8_t flags = 0;
    };
    Boxed<RewardMarker> rewardMarker;
    /// A spell book's spell or an ability book's ability (a byte), a field guide's
    /// creature (its UseData, the guide name), a quest gold pile's or chest's gold:
    /// what the reward generator puts in them (rewards.hpp).
    std::uint8_t bookEntry = 0;
    Name guideCreature;
    std::uint32_t goldAmount = 0;
    /// An ankh's (AnkhCollide's InitData): the last 64 who took a life from it (their name, class,
    /// serial, the frame; forgotten after 240 s), the next place to note one.
    struct AnkhTaker {
        std::u16string name;
        std::uint8_t classId = 0;
        std::string serial;
        std::uint32_t frame = 0;
    };
    struct Ankh {
        std::array<AnkhTaker, 64> takers{};
        std::uint8_t next = 0;
    };
    Boxed<Ankh> ankh;
    /// A shopkeeper's (monster subclass 8, its InitData: MonsterCreate's defaults, MonsterXfer's
    /// shop XFer_ReadShopItem; trade.hpp): what it buys at and sells for, its greeting
    /// (a string id, at most 31 letters: SetShopkeeperText), its stock (the count, then
    /// records of the type, how many, the spell / ability / guide creature, the four
    /// modifiers).
    struct Shopkeeper {
        struct Stock {
            std::string type;
            std::uint8_t count = 0;
            std::int32_t parameter = 0; ///< a spell or ability book's (by id)
            std::string guide;          ///< a field guide's creature (the type it names)
            std::array<std::uint8_t, 4> modifiers{0xFF, 0xFF, 0xFF, 0xFF};
        };
        float buyMultiplier = 1.0f;
        float sellMultiplier = std::bit_cast<float>(0x3EAAAA3Bu);
        std::string greeting;
        std::vector<Stock> stock;
    };
    Boxed<Shopkeeper> shopkeeper;
    /// A soul gate's last touch (SoulGateCollide's: a player told again after 1 s).
    std::uint32_t soulGateAt = 0;
    std::uint16_t direction = 0;
    std::uint8_t team = 0;
    std::uint8_t lock = 0;           ///< doors: 0 unlocked, 5 locked
    std::uint8_t doorDirection = 0;  ///< doors: where it's swung to, 0-31
    std::uint8_t doorClosed = 0;     ///< doors: its shut direction
    bool doorQuestOpened = false;    ///< doors: unlocked in a quest, its marker cleared
    std::uint8_t doorPrevious = 0;   ///< doors: last frame's direction
    std::int16_t doorAngle = 0;      ///< doors: its angle, 0-255 (direction x 8)
    float doorSwing = 0;             ///< doors: this step's swing from pushes
    bool doorSwinging = false;       ///< doors: in the step's swing list
    std::uint32_t doorTouched = 0;   ///< doors: the frame something last touched it
    /// Elevators and their shafts (ElevatorUpdateData): the other of the pair (its extent: a
    /// map load links them, World::linkElevators; the record's number for it is what a save
    /// writes), an elevator's state (0 down, 3 rising, 2 up, 1 sinking; a shaft's
    /// copy of its elevator's), its height (0-64) and the frame it stopped.
    std::uint32_t elevatorLink = 0;
    std::uint8_t elevatorState = 0;
    int elevatorLevel = 0;
    std::uint32_t elevatorStopped = 0;
    std::vector<std::uint32_t> inventory;
    Priority priority = Priority::Low;
    /// Players (a bit each, by slot) who're owed an update of it (set by
    /// unitNeedSync when it changes): all at first.
    std::uint32_t syncMask = 0xFFFFFFFFu;
    /// The record it was read from (World::load, a generator's template, a .plr's item): what
    /// the save (object_save.hpp) writes back for the fields ours doesn't keep, as the exe's
    /// object holds what its xfer read into it. None for a thing made in play.
    std::shared_ptr<const map::ObjectXfer> record;
    /// The exe's Extent: the number the map (or .plr) gave it, 0 for a thing made in play. Ours
    /// (`extent`) is our key and may differ: a map numbering two things alike, a thing carried to
    /// a map using its number (Server::loadMapKeeping). The save writes this.
    std::uint32_t fileExtent = 0;

    bool enabled() const noexcept { return (flags & ObjFlag::Enabled) != 0; }
    bool gone() const noexcept { return (flags & ObjFlag::Destroyed) != 0; }
    /// It changed: every player is owed an update.
    void touch() noexcept { syncMask = 0xFFFFFFFFu; }
    /// unitRaise: its height changed: an update owed (unitNeedSync)
    /// and its height (heightOwed): to every player for the always-sent kinds
    /// (0x20400004), else (by the report-kind test's 0x40, the height not 0) to those who
    /// know it, and at 0 no longer to those who don't.
    void raise(float to) noexcept
    {
        if (z == to) {
            return;
        }
        touch();
        z = to;
        heightOwed = (classes & 0x20400004u) ? 0xFFFFFFFFu : z != 0 ? (heightOwed | knownBy) : knownBy;
    }};

/// A wall in a grid cell. The exe's flags: 4 secret, 8 destructible, 0x10 door, 0x20
/// broken, 0x40 window.
struct WallCell {
    std::uint8_t material = 0;
    std::uint8_t direction = 0; ///< its shape: 0 / and 1 \ straight, 2 cross, 3-10 the rest
    std::uint8_t variant = 0;   ///< which of its looks
    std::uint8_t health = 0;    ///< a breakable one's
    /// Its id in the wall messages (MSG_DESTROY_WALL / OPEN_WALL / CLOSE_WALL): its index
    /// among the map's secret walls (server_mapRWSecretWalls), then, a breakable one,
    /// among its breakable walls (server_mapRWDestructableWalls, read after), each counting only the walls found.
    std::uint16_t wallId = 0xFFFF;
    bool secret = false;       ///< opens and closes (the wall's flag 4)
    bool destructible = false;
    bool window = false;
    bool broken = false;
    /// The door it's the leaf of (flag 0x10, doorAttachWall): its extent.
    std::uint32_t door = 0;
    /// A secret wall's record (server_mapRWSecretWalls): state 1 closed, 2 closing, 3 open,
    /// 4 opening; how far open, 0-23; flags (4 shuts / opens by itself, 8 open
    /// at first); the seconds it waits and the frames left of it.
    std::uint8_t secretState = 1;
    std::uint8_t secretOpen = 0;
    std::uint8_t secretFlags = 0;
    std::uint32_t secretWait = 0;
    std::int32_t secretTimer = 0;
    /// From the secret record: the players (a bit each, by index) last told it's open
    /// (as it comes into a player's view: MSG_OPEN_WALL / CLOSE_WALL at
    /// a player's CLIENT_READY: as the map has it, by flag 8). The map's value, or for a record
    /// without a state all or none by its flag 8. The solo save writes it back.
    std::uint32_t secretSeen = 0;
    /// The wall's flag 0x80 (WallMap's direction bit 7, server_mapRWWallMap).
    bool flag80 = false;
    /// WallMap's last byte, written back by the save.
    std::uint8_t extra = 0;
    /// Its place in the exe's wall list (g_wallList, Map_CreateWallAt puts each new wall at its
    /// head): the order it was made in, so the list runs from the highest down.
    std::uint32_t serial = 0;
};

/// A wall the Wall spell raised (the magic wall records): the spell's, where, whether
/// it changed a wall that was there (and what that was), and the players it's been sent to.
struct MagicWall {
    std::uint32_t spell = 0; ///< its duration spell's serial
    int x = 0;
    int y = 0;
    bool replaced = false;
    std::uint8_t oldMaterial = 0;
    std::uint8_t oldDirection = 0;
    std::uint8_t oldVariant = 0;
    std::uint8_t direction = 0; ///< the wall's shape once raised (the map save puts it back, map)
    std::uint32_t sentTo = 0; ///< by player slot
};

/// The walls' 256 x 256 cells beside their map (World::walls: indexed_map.hpp), each cell's
/// node or null, for the wall tests on every ray's cell (no search). A cell off the grid is in the
/// map alone. Beside the cells, a bit a cell that holds a wall (8 KB, where the cells are 512 KB):
/// most cells a ray or a step crosses hold none, told by the bit alone.
struct WallTable {
    std::vector<WallCell*> cells;       ///< x * 256 + y (none till a wall is put in)
    std::vector<std::uint64_t> walled;  ///< the same cells, 64 a word: those with a wall
    static bool inside(int x, int y) noexcept { return 0 <= x && x < 256 && 0 <= y && y < 256; }
    static std::size_t slot(int x, int y) noexcept { return static_cast<std::size_t>(x) * 256 + static_cast<std::size_t>(y); }
    void add(const std::pair<int, int>& at, WallCell& c)
    {
        if (inside(at.first, at.second)) {
            if (cells.empty()) {
                cells.assign(256 * 256, nullptr);
                walled.assign(256 * 256 / 64, 0);
            }
            const std::size_t i = slot(at.first, at.second);
            cells[i] = &c;
            walled[i / 64] |= std::uint64_t{1} << (i % 64);
        }
    }
    void remove(const std::pair<int, int>& at, WallCell&) noexcept
    {
        if (inside(at.first, at.second) && !cells.empty()) {
            const std::size_t i = slot(at.first, at.second);
            cells[i] = nullptr;
            walled[i / 64] &= ~(std::uint64_t{1} << (i % 64));
        }
    }
    void clear() noexcept
    {
        cells.clear();
        walled.clear();
    }
    /// The cell's wall, (x, y) inside.
    WallCell* at(int x, int y) const noexcept
    {
        if (cells.empty()) {
            return nullptr;
        }
        const std::size_t i = slot(x, y);
        return (walled[i / 64] >> (i % 64)) & 1u ? cells[i] : nullptr;
    }
};

/// The objects by extent beside their map (World::objects: indexed_map.hpp), each one's node or
/// null, for byExtent's lookups (no search); an extent past kLimit is in the map alone. And counts
/// of the players' units, the sentries and the balls put in or taken out (World::playerUnits,
/// sentryUnits, ballUnits). Beside the table, a bit an extent that has an object, so a walk of
/// them all in the extents' order needn't step through the map (World::eachObject).
struct ExtentTable {
    static constexpr std::uint32_t kLimit = 0x10000;
    std::vector<ServerObject*> objects; ///< by extent (grown as they come)
    std::vector<std::uint64_t> present; ///< by extent, 64 a word: those in `objects`
    std::uint32_t playersMoved = 0;
    std::uint32_t sentriesMoved = 0;
    std::uint32_t ballsMoved = 0;
    void add(std::uint32_t extent, ServerObject& o)
    {
        if (extent < kLimit) {
            if (extent >= objects.size()) {
                objects.resize(std::min<std::size_t>(kLimit, std::max<std::size_t>({extent + 1u, objects.size() * 2, 1024})), nullptr);
                present.resize((objects.size() + 63) / 64, 0);
            }
            objects[extent] = &o;
            present[extent / 64] |= std::uint64_t{1} << (extent % 64);
        }
        counted(o);
        if (o.pendingDelete) {
            o.pendingDelete = true; // (one coming back already marked counts as a new mark, so the deletions pass sees it)
        }
    }
    void remove(std::uint32_t extent, ServerObject& o) noexcept
    {
        if (extent < objects.size()) {
            objects[extent] = nullptr;
            present[extent / 64] &= ~(std::uint64_t{1} << (extent % 64));
        }
        counted(o);
    }
    void clear() noexcept
    {
        objects.clear();
        present.clear();
        ++playersMoved;
        ++sentriesMoved;
        ++ballsMoved;
    }
    void counted(const ServerObject& o) noexcept
    {
        if (o.player) {
            ++playersMoved;
        }
        if (o.sentry) {
            ++sentriesMoved;
        }
        if (o.ball) {
            ++ballsMoved;
        }
    }
    /// The extent's object (null: none), extent < kLimit.
    ServerObject* at(std::uint32_t extent) const noexcept { return extent < objects.size() ? objects[extent] : nullptr; }
};

/// The objects with a record of a kind, in the extents' order (World::playerUnits, sentryUnits): a
/// snapshot, kept alive while it's walked whatever the world does meanwhile.
template <class O>
class UnitList {
public:
    using List = std::vector<ServerObject*>;
    explicit UnitList(std::shared_ptr<const List> list) noexcept : list_(std::move(list)) {}
    O* const* begin() const noexcept { return list_->data(); }
    O* const* end() const noexcept { return list_->data() + list_->size(); }
    std::size_t size() const noexcept { return list_->size(); }
    bool empty() const noexcept { return list_->empty(); }

private:
    std::shared_ptr<const List> list_;
};

/// mapTraceRay's flags.
namespace Trace {
inline constexpr std::uint8_t Doors = 0x01;         ///< closed doors block (and odd cells' walls count)
inline constexpr std::uint8_t ThroughWindows = 0x04;
inline constexpr std::uint8_t NearWindows = 0x08;   ///< windows within 60 of the start don't block
inline constexpr std::uint8_t SightOnly = 0x40;     ///< only walls that block sight (WALL flag 1)
inline constexpr std::uint8_t PassSecret = 0x80;    ///< secret walls with their record's flag 2 don't
/// The line of sight the update stream checks (netSendObjects2Plr: 'E').
inline constexpr std::uint8_t Sight = Doors | ThroughWindows | SightOnly;
} // namespace Trace

struct RayHit {
    float x = 0; ///< where the ray met the wall
    float y = 0;
    int cellX = 0;
    int cellY = 0;
};

/// A box shape's corners about its centre (the game's box shape set-up from its EXTENT, which
/// keeps them in the shape): its top (c0), left (c1), right (c2) and bottom (c3).
struct BoxCorners {
    float c0x, c0y, c1x, c1y, c2x, c2y, c3x, c3y;
};

/// The shape set-up's half side turned 45 degrees: 0.35355 (its float, 0x3EB50481; not 1 / (2 sqrt
/// 2) to the float's last place, 0x3EB504F3).
inline constexpr float kShapeBoxHalf = std::bit_cast<float>(0x3EB50481u);

/// The game's shorter root half, 0.7071 (0x3F350481): a box's side tests, the walls' diagonals,
/// the path finder's box test (not 1 / sqrt 2's float, 0x3F3504F3).
inline constexpr float kShortRootHalf = std::bit_cast<float>(0x3F350481u);

/// 1 / 23, the float the game multiplies a coordinate by for its wall cell (0x3D321643).
inline constexpr float kInvWallCell = 1.0f / 23;
static_assert(std::bit_cast<std::uint32_t>(kInvWallCell) == 0x3D321643u);

/// A coordinate's wall cell as the game takes it inline: FMUL 1/23, FSTP float, Math_FloatToInt
/// (the chopped product, truncated). From a value still on the stack, the same with its 53 bits.
inline int wallCellOf(util::X87 v) { return util::floatToInt(v * kInvWallCell); }
inline int wallCellOf(float v) { return wallCellOf(util::X87(v)); }

/// A box shape's corners, as the shape set-up makes them in the game's x87: a = w x 0.35355 and
/// b = h x 0.35355 stored to floats; top x is b's unstored product less a; each corner stored.
inline BoxCorners boxCorners(float width, float height)
{
    using util::X87;
    const float a = (X87(width) * kShapeBoxHalf).f();
    const X87 bRaw = X87(height) * kShapeBoxHalf;
    const float b = bRaw.f();
    const float bLessA = (bRaw - a).f();
    const float sumNeg = (-X87(a) - b).f();
    const float sum = (X87(b) + a).f();
    const float aLessB = (X87(a) - b).f();
    return {bLessA, sumNeg, sumNeg, bLessA, sum, aLessB, aLessB, sum};
}

/// Whether (px, py) is inside a box shape (corners c) about (bx, by), strictly: the shape's point
/// test's box arm (the path finder's marking, the missiles' body test, the generators' spot test).
/// On the x87 as the game has it: the top's and the right's corners stored, the left's kept on the
/// stack; each side's test, times 0.7071, against 0 on the stack (a NaN fails the second).
inline bool insideBoxCorners(const BoxCorners& c, float bx, float by, float px, float py)
{
    using util::X87;
    const float ax = (X87(c.c0x) + bx).f();
    const float ay = (X87(c.c0y) + by).f();
    const X87 cx = X87(c.c1x) + bx;
    const X87 cy = X87(c.c1y) + by;
    const float ex = (X87(c.c2x) + bx).f();
    const float ey = (X87(c.c2y) + by).f();
    const X87 zero(0.0f);
    return (X87(ay) - ax + px - py) * kShortRootHalf < zero && zero < (cy - cx + px - py) * kShortRootHalf &&
           zero < (X87(ey) + ex - px - py) * kShortRootHalf && (X87(ay) + ax - px - py) * kShortRootHalf < zero;
}

struct ServerWaypoint {
    std::int32_t id = 0;
    std::string name;
    float x = 0;
    float y = 0;
    bool enabled = true;
    std::uint8_t linkFlags = 0; ///< its links' flags and those of links to it
    std::vector<std::int32_t> links; ///< the waypoints it leads to, by id
    std::vector<std::uint8_t> linkTypes; ///< each link's flags, as the map has them
};

/// A map polygon (g_mapPolygons, index from 1): its corners (truncated), their
/// box, its group (sounds carry within one), the scripts run on entering it (the map
/// reader's playerEnter) and on leaving it (its monsterEnter), its flags (1 a
/// quest's secret area) and the players who've been in it (by player index).
struct MapPolygon {
    std::vector<std::pair<int, int>> points;
    int minX = 0;
    int minY = 0;
    int maxX = 0;
    int maxY = 0;
    std::uint8_t group = 0;
    ServerObject::ScriptBlock onEnter;
    ServerObject::ScriptBlock onLeave;
    std::uint32_t flags = 0;
    std::uint32_t visited = 0;
};
/// A sound played this frame (the exe's list): its id, where, its object
/// (0: none), and who hears it (mode 0 everyone, 1 the team `arg`, 2 the player of net code `arg`).
struct SoundEvent {
    std::uint32_t id = 0;
    float x = 0;
    float y = 0;
    std::uint32_t source = 0;
    std::uint8_t mode = 0;
    std::uint32_t arg = 0;
};

/// "No polygon looked up yet" (0xDEADFACE, the players' and monsters' polygon at first).
inline constexpr std::uint32_t kPolygonUnknown = 0xDEADFACEu;

struct ServerGroup {
    enum class Kind : std::uint8_t { Objects = 0, Waypoints = 1, Walls = 2, Unknown3 = 3 };
    std::int32_t id = 0;
    std::string name;
    Kind kind = Kind::Objects;
    std::vector<std::int32_t> members;              ///< extents, waypoint ids or group ids: the list's order, its head (the file's last) first
    std::vector<std::pair<int, int>> walls;         ///< grid cells, in the same order
};

/// A spell that lasts (the exe's spellDuration records): its caster's and its
/// target's, its start / update / end by its spell (spells.cpp), till it's cancelled.
struct DurationSpell {
    std::uint32_t serial = 0;    ///< its number
    std::uint32_t spell = 0;
    int power = 0;
    std::uint32_t source = 0;    ///< what cast it (a wand, the caster)
    std::uint32_t caster = 0;    ///< 0: gone (a glyph's still runs)
    bool glyph = false;          ///< from a glyph (glyphExtent: which)
    std::uint32_t glyphExtent = 0;
    float x = 0;                 ///< where it was cast from
    float y = 0;
    std::uint32_t rayTarget = 0; ///< what its beam was last shown to (a drain's, a bolt's)
    std::uint32_t target = 0;    ///< the spell's target
    float targetX = 0;
    float targetY = 0;
    std::uint32_t start = 0;
    std::uint32_t last = 0;
    std::uint32_t end = 0;       ///< its end (== start: none)
    int value = 0;               ///< its own number (a shield's strength)
    float carry = 0;             ///< the fraction of a point not yet given
    /// Chain Lightning's bolts, from and to (its child records, spell 7, newest first),
    /// and the last frame's.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> bolts;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> lastBolts;
    /// Firewalk's trail: where the last flames went, and the ones before.
    std::array<float, 4> trail{};
    std::uint32_t object = 0; ///< its own object: Force of Nature's or Mana Bomb's charge
    bool burst = false;       ///< Mana Bomb's gone off
    /// A summoning's: the creature's type, where and facing it comes, its number for
    /// the clients' effect (FX_SUMMON / _CANCEL), and whether it came.
    std::string summonType;
    float summonX = 0;
    float summonY = 0;
    std::uint8_t summonDir = 0;
    std::uint16_t summonId = 0;
    bool summoned = false;
    bool cancelled = false;      ///< to go next pass
    bool fromStaff = false;      ///< cast by its caster's staff
    std::uint32_t staff = 0;     ///< Chain Lightning's and Plasma's staff: they pay in its charges
    /// Its create, update and destroy null: the Wall a .plr's walls are raised with,
    /// until restoreMagicWalls gives it the Wall's (solo.hpp).
    bool noFunctions = false;
    /// noxcpp's (memory, not logic): the frame's update is in its call (Spells::updateLasting), and
    /// it was ended meanwhile (finishLasting: a cast that call led to, through a script, freed it).
    /// Ended so, it stays in the list till its call returns: the walk's hold on it stays good.
    bool walked = false;
    bool finished = false;
};

/// A team (the exe's Team table, 16 records): its name (at most 20),
/// members (newest first, by extent), score (Lessons), colour, id (1-16),
/// its clan's id, whether clients show its colour's title for the name
/// and its base (its flag, by extent).
struct ServerTeam {
    bool active = false;
    std::u16string name;
    std::vector<std::uint32_t> members;
    int lessons = 0;
    std::uint8_t color = 0;
    std::uint8_t id = 0;
    std::uint32_t clan = 0;
    bool useTitle = false;
    std::uint32_t base = 0;
    std::uint32_t crown = 0; ///< team KotR's crown
};

/// A warrior's ability under way (the exe's executingAbilityClass records, Abil_TryExecute):
/// which, whose, its end frame and its own flag (1 till its work's done, e.g. War
/// Cry's shout).
struct ActiveAbility {
    int ability = 0;
    std::uint32_t unit = 0;
    std::uint32_t end = 0;
    int flag = 1;
};

/// A trade (trade.hpp; the exe's TradeSession, of the "TradeSessions" class): a shop
/// (a player and a shopkeeper) or two traders. Its state (1 while its traders hold it), the
/// frame it began, its two sides (by extent: a shop's player first), a shop's
/// flag, the shop's stock in its order (each item and its price, the item out of the
/// world: World::tradeObjects), the sides' accepts, their offers, newest first
/// (offersA / offersB), their totals and the gold each side put up (the gold
/// of the two piles).
struct TradeSession {
    struct Item {
        std::uint32_t object = 0;
        std::uint32_t price = 0;
    };
    std::uint32_t id = 0; ///< ours: what PlayerControl::trade holds
    bool active = false;
    std::uint32_t started = 0;
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    bool shop = false;
    std::vector<Item> stock;
    bool acceptedA = false;
    bool acceptedB = false;
    std::vector<Item> offersA;
    std::vector<Item> offersB;
    std::uint32_t totalA = 0;
    std::uint32_t totalB = 0;
    std::uint32_t goldA = 0;
    std::uint32_t goldB = 0;
};
/// The script ids' counter at a game's start (respawnAllow).
inline constexpr std::int32_t kFirstScriptId = 1000000000;

/// Where the extents of things made in play start (above every stock map's: 15317 at most).
inline constexpr std::uint32_t kDynamicExtents = 0x4000;

/// The net code clients know an object by (netGetUnitCodeServ): a map object the
/// client loads itself (IMMOBILE or 0x20000000) its extent with bit 15, anything else its extent
/// (0 past 0x7FFF).
constexpr std::uint16_t netCodeOf(const ServerObject& o) noexcept
{
    if (o.extent > 0x7FFFu) {
        return 0;
    }
    // (A map thing the client loads: its map number, the exe's Extent; ours may differ for one the
    // map numbered twice. One made in play has Extent 0 in the exe (nothing but a map record's read
    // sets it: createAt doesn't), so such a one of these classes is 0x8000: an original bug.)
    const std::uint32_t mapNumber = o.fileExtent;
    if ((o.classes & 0x20400000u) && mapNumber > 0x7FFFu) {
        return 0;
    }
    return static_cast<std::uint16_t>((o.classes & 0x20400000u) ? (mapNumber | 0x8000u) : o.extent);
}

/// A wall's script handle (Wall(x, y)): x << 16 | y.
constexpr std::uint32_t wallHandle(int x, int y) noexcept
{
    return static_cast<std::uint32_t>(x) << 16 | static_cast<std::uint32_t>(y & 0xFFFF);
}

/// The map being played: its objects by extent, walls, waypoints, groups, the map grid, and the
/// game-wide state the server's logic shares (flags, teams, quest numbers, lasting spells...).
class World {
public:
    /// The game's flags (GameFlag: the mode, network or solo), as the host runs it; what plays
    /// by them asks here. A network arena unless the mode layer says otherwise.
    std::uint32_t gameFlags = GameFlag::Network | GameFlag::Arena;
    /// gamedata.bin (the install's, loaded at start-up; null: every number 0), read as gameFlags
    /// has it (number()).
    const things::GameData* gameData = nullptr;
    const strings::StringTable* strings = nullptr; ///< nox.csf (null: lines go as their labels)
    /// The map's script (null: none), for the objects' handlers (triggers...).
    ScriptHost* script = nullptr;
    /// The host's own client (host_client.hpp: GAME.EXE's direct calls into it), if any.
    HostClient* hostClient = nullptr;
    /// noxcpp: a NoX Core mode's hooks inside the frame (mode_hooks.hpp: a hosted mode's);
    /// null: none, and every call site plays as the game's.
    ModeHooks* modeHooks = nullptr;
    /// A string-table line as Str_GetString gives it: of a label with several
    /// variants one at random (randInt(0, count - 1)), its text and its speech
    /// file (the second out: a WRTS variant's, else none); a label the table lacks
    /// "MISSING:'<label>'"; no table at all, the label itself.
    struct Line {
        std::u16string text;
        std::string speech;
    };
    Line line(std::string_view label) const;
    /// What Str_GetString's pick among a line's variants draws (a stream of its own here).
    mutable std::minstd_rand stringRng{1};
    /// The server's random stream (randInt's), for an object's INIT as it's made
    /// (GoldInit's pile); null in a world with no server.
    std::minstd_rand* rng = nullptr;
    /// A line to one player (SrvNet_SendTextMessageToUnit: MSG_TEXT_MESSAGE 0xA8 from
    /// no one), the string table's text for `label`.
    void textTo(const ServerObject& player, std::string_view label, std::optional<std::u16string_view> arg = std::nullopt);
    /// A line to everyone (SrvNet_BroadcastTextMessage), `flags` with it (0x10 the
    /// console's broadcast).
    void textToAll(std::u16string_view text, std::uint8_t flags = 0);
    /// MSG_TEXT_MESSAGE from no one: the text's first kMaxTextMessage characters (its length byte
    /// counts the end too).
    static std::vector<std::uint8_t> textMessage(std::u16string_view text, std::uint8_t flags);
    static constexpr std::size_t kMaxTextMessage = 254;
    /// `unit` says `text` to everyone (SrvNet_BroadcastUnitSay: MSG_TEXT_MESSAGE 0xA8
    /// with its net code, where it is and how long; a bubble over it).
    void unitSay(const ServerObject& unit, std::u16string_view text, std::uint16_t duration);
    /// Its bubble gone (SrvNet_BroadcastChatKill: MSG_CHAT_KILL 0xCA); null: every
    /// bubble (SrvNet_BroadcastChatKillAll: net code 0xDEAD).
    void chatKill(const ServerObject* unit);
    /// The string table's text for `label` (line(): Str_GetString, a random variant of
    /// several; a label it lacks gives "MISSING:'<label>'"; no table at all: the label itself).
    std::u16string text(std::string_view label) const { return line(label).text; }
    /// The host's gameplay switches (CheckGameplayFlags): bit 1 lets a player's
    /// own side hurt each other.
    std::uint32_t gameplayFlags = 0;
    /// A quest's damage to players (1, set by up to PlayerDamageCap).
    float questDamage = 1.0f;
    /// noxcpp: a NoX Core mode's multiplier on the damage players take outside a quest (as the
    /// quest's questDamage); 0: none, the game's own.
    float modeDamage = 0.0f;
    /// A quest's monster health multiplier (0.2 till a quest sets it up
    /// to SystemHealthCap) and the map's difficulty (a quest's 0-2 at random, else 0).
    float questHealth = 0.2f;
    int mapDifficulty = 0;
    int questStage = 1; ///< (game_getQuestStage: 1 till a quest moves on)
    /// A quest's difficulty (1): ((players - 1) x PlayerDifficultyDelta + 1) x the
    /// stage, every 5 s (quest.hpp).
    float questDifficulty = 1.0f;
    /// A quest's warp gates on, its bosses this map,
    /// the frame till which a player may rejoin (a quest
    /// map's load + 1 s), the warp's state and its window shown.
    /// These and the quest numbers above are the exe's globals: kept
    /// from map to map (Server::loadMapKeeping).
    bool warpGatesOn = false;
    bool questBosses = false;
    std::uint32_t questRejoinUntil = 0;
    int questWarpState = 0;
    bool questWarpGui = false;
    /// The frame the last player went into a warp gate (g_questWarpEnterFrame), the
    /// next quest map the exits picked (the time limit's AutoExitToNextMap loads it), a
    /// warp just taken.
    std::uint32_t questWarpEnterFrame = 0;
    std::string questNextMap;
    /// isQuest (setQuest): a quest game was chosen to host (the main
    /// menu's Solo Quest, noxworld's Host Quest); the quest itself is game flag 0x1000, begun by
    /// startSoloQuest (OTQuest.rul) or a quest map after a chat one.
    bool questChosen = false;
    /// questFlag: startSoloQuest's frames to go, run down at the tick's end.
    std::uint32_t soloQuestCountdown = 0;
    bool questWarped = false;
    /// The secret areas (polygons flagged 1) of this map and of the one before it (as
    /// a map's polygons load: MSG_GAUNTLET 0x0C's total).
    int questSecretsHere = 0;
    int questSecretsLast = 0;
    /// The map's quest briefing (its MapInfo's two quest-intro fields, version 3): its text's id and
    /// its picture (MSG_GAUNTLET 0x0D / 0x0E).
    std::string questIntro;
    std::string questGraphic;
    /// noxcpp (Server::Options::questScreens): false, a quest map starts as any other, faded in
    /// (MSG_FADE_BEGIN {1, 1}), with no rules (MSG_GAUNTLET 0x0E), stage (0x0D) or results (0x0C)
    /// screen: a mode's levels follow on.
    bool questScreens = true;
    /// noxcpp (Server::Options::questSaves): false, no quest save goes to a client (a level's end,
    /// a game over's save-then-kick: out at once instead): a mode whose players play their
    /// network characters, which the stock client would overwrite with the quest's save.
    bool questSaves = true;
    /// noxcpp (Server::Options::forceClass): every player of this class (0 warrior, 1 wizard, 2
    /// conjurer) whatever its character's, in a quest without quest saves only (forcedClass):
    /// a mode's conjurers. The stock client takes its own class from its MSG_NEW_PLAYER.
    std::optional<std::uint8_t> forceClass;
    /// noxcpp (Server::Options::multiclass): a mode's multiclass for its noxsdl players
    /// (PlayerControl::multiclass, set as one joins: a stock client's stays its class's).
    bool multiclass = false;
    /// noxcpp (Server::Options::monstersAlert): a mode's monsters look for enemies all
    /// about them, as an ALERT one does (Monsters::look), not only in front: quicker to react to
    /// a player coming in from the side or behind. Stock: off.
    bool monstersAlert = false;
    /// noxcpp (Server::Options::keepThingsOnDeath): outside a quest a dead player keeps its
    /// things, as a quest's does (Fight::playerDies: only a carried flag or crown dropped, its
    /// worn things told to every client again after MSG_PLAYER_DIED; Fight::respawn: no default
    /// outfit, MSG_PLAYER_RESPAWN's flag 0). Read on the death path only. In a quest: nothing (the
    /// quest's own rules). Stock: off.
    bool keepThingsOnDeath = false;
    /// noxcpp (Server::Options::modeOwnsBook; a script mode's configure key bookIsTheMod): outside
    /// a quest, a network game's book is the mode's, not the whole one the stock network game
    /// gives (modeBook()): a player starts with nothing and has only what the mode grants,
    /// checked as a quest checks it. Read on a join, a use, a cast and a grant only. In a quest or
    /// a solo game it does nothing (their own rules). Stock: off.
    bool modeOwnsBook = false;
    /// noxcpp (Server::Options::playersCantDie): the map editor's play-test sandbox: a player's
    /// health never drops below 1 (Fight::takeHealth); monsters still die. Stock and every ordinary
    /// game: off.
    bool sandboxPlayersCantDie = false;
    /// noxcpp: every spell at its full power (3), as in the versus modes (Spells::power), outside
    /// them too: a mode that runs under game flags of no stock mode and plays as one (a script's
    /// Arena). Stock and every ordinary game: off.
    bool versusSpells = false;
    /// noxcpp (Server::Options::noScoring): a map editor session's sandbox ("no scoring"), or a
    /// mode that keeps its own score (the game's scoring would count beside it): kills,
    /// deaths, flag captures and goals count nothing (scoreDeath, the flag's and the ball's
    /// scoring: the flag still goes home, the ball to a start), so a map whose only modes score
    /// (CTF, flagball) has no score or end of round while it's edited. Play-test rounds, the
    /// editor's own play-test, stock and every ordinary game: off.
    bool sandboxNoScoring = false;
    /// noxcpp (Server::Options::modeOwnsLimits): a mode that plays as a stock one and keeps that
    /// mode's lesson and time limits itself (a script mode's looksLike). lessonLimits and timeLimits
    /// stay the host's and are told to every client as the stock mode's (MSG_GAME_SETTINGS, the
    /// settings records), but the game's own checks don't act on them (enforcedLessonLimit,
    /// enforcedTimeLimit: 0, so no native win, timer or time-up). Stock and every other game: off.
    bool modeOwnsLimits = false;
    /// noxcpp (a mode's): players have nothing to fight with by hand, a bare-handed punch too
    /// (the player's attack does nothing). Stock: off.
    bool noBareHands = false;
    /// When a client came in at each player slot (0: none): 30 s on and
    /// still not in the quest, the others forget what they were told of it.
    std::array<std::uint32_t, 32> questSlotSeenAt{};
    /// The map's types as its MapInfo has them (set by mapLoad for the
    /// map being loaded: 1 co-op, 2 quest, 4 arena... 0x80000000 chat): keeps the
    /// players' things for a quest map (2), MakeDefItems gives a quest's weapon unless a chat map.
    std::uint32_t mapTypes = 0;
    /// The quest's maps (quest.hpp; the server's, null: none known).
    QuestMaps* questMaps = nullptr;
    /// The host's side of a quest's saved players (GameHost: the file transfers; none without a
    /// host): `questSendSave` is the map start's step for a player (Plr_SaveServerPlayerData,
    /// MSG_GAUNTLET 0x1C, SrvNet_SendSaveDataToClient), `questSaveAndKick` (its save
    /// to its client, then out once the client has it; or out at once).
    std::function<void(Fight&, ServerObject&)> questSendSave;
    std::function<void(Fight&, ServerObject&)> questSaveAndKick;
    /// The spells that last, newest first (the exe's list: new ones at its head).
    std::list<DurationSpell>& durationSpells() noexcept { return durationSpells_; }
    /// The spells being spoken, by their casters' extents, newest first (g_pendingSpellCastList,
    /// g_pendingSpellCastList: Spell_StartCast puts each at its head; Spell_UpdatePendingCasts and
    /// Spell_WarcryInterruptCasting walk it from there). The spell itself is its player's
    /// PlayerControl::chant; one whose chant is gone (a death, a respawn) is passed over and
    /// dropped at the next walk.
    std::vector<std::uint32_t>& pendingCasts() noexcept { return pendingCasts_; }
    /// The warriors' abilities under way, newest first (g_abilActiveList).
    std::list<ActiveAbility>& activeAbilities() noexcept { return activeAbilities_; }
    const std::list<ActiveAbility>& activeAbilities() const noexcept { return activeAbilities_; }
    std::uint32_t nextSpellSerial = 0;
    /// The next of BoulderDie's twelve rocks (one count for every
    /// boulder, carried from one to the next).
    std::uint32_t nextRock = 0;
    /// The teams (modes.hpp), by slot.
    std::array<ServerTeam, 16> teams{};
    /// Each mode's lesson limit (the host's setting, in its order: arena,
    /// elimination, CTF, KotR, flagball, quest; at most 999; 0 none). Elimination's is its lives.
    std::array<std::uint16_t, 6> lessonLimits{};
    /// Teams by clan (a chat game whose settings' clan bit is set).
    bool clanTeams = false;
    /// Each mode's time limit in minutes (the same order; 0 none), and the match timer
    /// (g_gameTimerRunning, g_gameTimerEndTicks: here the frame it ends).
    std::array<std::uint8_t, 6> timeLimits{};
    bool timerRunning = false;
    std::uint32_t timerEnd = 0;
    /// A countdown under way (elimination's sudden death, servStartCountdown).
    bool countdown = false;
    /// The server's ticks: frames run, paused or not (the real time's 30 Hz: what the exe times in
    /// real time while the frame count stands, solo.hpp).
    std::uint64_t ticks = 0;
    /// A timed pause's end, in ticks (g_pauseUntilInputSeq; 0: none).
    std::uint64_t pauseUntilTick = 0;
    /// The level-up moment (levelUpMoment's globals: on, the player, its LevelUp, the kind, its
    /// start, timed).
    struct LevelUpMoment {
        bool active = false;
        std::uint32_t unit = 0;
        std::uint32_t object = 0;
        int kind = 0;
        std::uint64_t startTick = 0;
        bool timed = false;
    } levelUp;
    /// A solo game's save asked for (requestSave's globals: g_gameSavePending, the exit that asked,
    /// the frame, the frames it waits), its name (setSaveFileName: "WORKING", "AUTOSAVE"...), and
    /// "came by an exit".
    struct SaveRequest {
        bool pending = false;
        std::uint32_t exit = 0;
        std::uint32_t at = 0;
        std::uint32_t delay = 0;
    } saveRequest;
    std::string saveName;
    bool cameByExit = false;
    /// The last save request named an exit (another requestSave global): then drops a
    /// visited map's saved creatures (the live ones came along).
    bool saveByExit = false;
    /// A visited map's saved copy to load next (found Save\WORKING\<map>\<map>.map):
    /// switch-to-solo, the "not multiplayer" flag (the weapons' and
    /// armour's xfers read it) and the path (gameSetSoloSavePath).
    bool switchToSolo = false;
    bool noMultiplayer = false;
    std::string soloPath;
    /// A saved game being loaded (mapLoadOrSaveMB(1)): its map's
    /// load asks no AUTOSAVE, and clears it; the frame the last load ran.
    bool loadingSavedGame = false;
    std::uint32_t savedGameLoadedAt = 0;
    /// The host's choice of mode (the game settings' mode bits; setupMode may change it).
    std::uint32_t chosenMode = GameFlag::Arena;
    /// The flags on the map.
    int flagCount = 0;
    /// The dead come back by themselves, half a second on (OpenNox's misnamed
    /// ruleSetNoRespawn; read by server_doPlayersAutoRespawn, not in a
    /// quest): on for elimination, a host's option; and the host's setting that keeps it from map
    /// to map (a game settings bit).
    bool autoRespawn = false;
    bool keepAutoRespawn = false;
    /// A game hosted on WOL (noxsdl logged in to XWIS), where the host's Advanced
    /// Server switches count; a LAN game always has both: a player who dies leaves its
    /// bones (Plr_Respawn's respawnPlayerImpl) and a caster's words are
    /// heard by the others (Spell_UpdatePendingCasts'). Their defaults off and
    /// on (mapSetDataDefault).
    bool wolGame = false;
    bool leaveBones = false;
    bool gestures = true;
    /// Elimination's sudden death (resets it): from when it's counted
    /// (20 s on it may come), observers shut out once a death has come after that
    /// and by the frame's check, the players in play under
    /// which it comes (SuddenDeathPlayerThreshold) and the seconds a player's health lasts in it
    /// (SuddenDeathLifeTime).
    std::uint32_t suddenDeathFrom = 0;
    bool suddenDeathShutOut = false;
    bool suddenDeathChecked = false;
    int suddenDeathThreshold = 0;
    int suddenDeathLifeTime = 0;
    /// The map's ImaginaryCaster (imagCasterUnit), the extent: made at each
    /// map's load before its things (mapSwitchLevel -> the pending casts' clear), the
    /// script's CastSpellLocation* builtins cast from it.
    std::uint32_t imaginaryCaster = 0;
    /// Each team's flag as last reported (by team id), sent again
    /// to a client coming in: its state (0 home, 1 carried, 2 dropped), colour and carrier.
    struct FlagStatus {
        std::uint8_t state = 0;
        std::uint8_t color = 0;
        std::uint16_t carrier = 0;
    };
    std::array<FlagStatus, 256> flagStatus{};
    /// The flagball's status as last reported (0 at its start, 1
    /// loose, 2 / 4 carried by a team, blue 4), and its carrier's net code.
    std::uint8_t ballState = 0;
    std::uint16_t ballCarrier = 0;
    /// The last "objcoll.c:CantPickupBall" (g_ballCantPickupMsgFrame).
    std::uint32_t ballRefusedAt = 0;
    /// The host's server options (g_serverOptionFlags, MSG_GAME_SETTINGS +13): 2 weapons respawn
    /// ("weaponsrespawn"), 0x2000 ...
    std::uint32_t serverOptions = 0;
    /// Whether a server option is on. The camper alarm (0x2000) is always on in
    /// capture the flag and elimination (Game_HasFlags(0x420)), whatever the option's bit.
    bool serverOption(std::uint32_t bit) const noexcept
    {
        return (bit == 0x2000u && (gameFlags & 0x420u)) || (serverOptions & bit) != 0;
    }
    /// The respawn list (respawnAdd): what the map put in
    /// the world, each with its type, spot, facing, modifiers and ammunition, the thing there
    /// now, and when a new one is due (waiting). Taken before the first frame's pass
    /// (respawnsOpen: respawnAllow). Newest last here.
    struct Respawn {
        std::string type;
        std::uint32_t object = 0;
        float x = 0;
        float y = 0;
        std::uint16_t direction = 0;
        std::uint32_t at = 0;
        bool waiting = false;
        std::array<std::uint8_t, 4> modifiers{0xFF, 0xFF, 0xFF, 0xFF};
        std::uint8_t charges = 0;
        std::uint8_t maxCharges = 0;
    };
    std::vector<Respawn> respawns;
    bool respawnsOpen = true;
    /// Spells this map's mode turns off (spellDisable: flagball's Wall); a cast of
    /// one is refused (10).
    std::array<bool, 256> spellDisabled{};
    /// Types the host turned off (the console's "set weapon / armor / staff NAME off": their
    /// type's allowed flag cleared): not to be picked up (Obj_TryPickup).
    std::set<std::string> disabledTypes;
    /// A solo game's god mode (the console's "set god": engine flags 0x30, set_god).
    bool godMode = false;
    /// The classes' health and mana multipliers (warrior, wizard, conjurer: warriorMaxHealth...; 1, a
    /// quest's its own: MSG_STAT_MULTIPLIERS), for the potions.
    std::array<float, 3> healthMultiplier{1.0f, 1.0f, 1.0f};
    std::array<float, 3> manaMultiplier{1.0f, 1.0f, 1.0f};
    /// And their strength and speed's (the stats: applyStats's maxima times these).
    std::array<float, 3> strengthMultiplier{1.0f, 1.0f, 1.0f};
    std::array<float, 3> speedMultiplier{1.0f, 1.0f, 1.0f};
    /// The four kept as a quest began, for its end.
    std::array<std::array<float, 3>, 4> multipliersBeforeQuest{};
    /// monster.bin's monsters (this game's kind's numbers; null: monsters without them).
    const things::MonsterDb* monsterDb = nullptr;
    /// soundset.bin's creature sound sets (null: creatures make none of their own sounds).
    const things::SoundSets* soundSets = nullptr;
    /// The sounds' numbers (sound.hpp; the server's), for what asks whether a sound has files
    /// (getSevenDwords3). Null: none has.
    const SoundTable* soundTable = nullptr;
    /// thing.bin (the server's), for what only has the world (the enchantments' spell sounds).
    const things::ThingDb* thingDb = nullptr;
    /// The player animations' lengths (the server's), for the creatures drawn as players.
    const PlayerAnimations* playerAnimations = nullptr;
    /// The frame a monster went to FIGHT (frameCounterSetCopy):
    /// every monster looks about afresh that frame.
    std::uint32_t monstersLookAt = 0;
    /// Who froze the players (unitFreeze's lock, 1 the script's
    /// Frozen, 0 a dialog's or a shop's; reset at a new game's first map).
    std::uint32_t freezeLock = 0;
    /// The game frame now (gameFrame__), as the server's frame began.
    std::uint32_t frameNow = 0;
    /// Creatures put in an owner's list so far (ServerObject::ownedOrder).
    std::uint32_t ownedCount = 0;
    /// The sounds monsters may hear (the MonsterListen list, newest last): a sound with
    /// a hearing of a creature or player (up its owners), not to one player; kept 2 frames.
    struct HeardSound {
        std::uint32_t id = 0;
        std::uint32_t source = 0; ///< what played it (an extent; 0 once it's gone)
        float x = 0;
        float y = 0;
        std::uint32_t frame = 0;
    };
    std::vector<HeardSound> heardSounds;
    /// The most sounds() holds (Server::Options::frameSounds): a host's 0x80, the exe's
    /// "AudEvent" class, freed once a frame; 0 none (a world run without a host,
    /// whose sounds pile up over frames).
    std::size_t soundLimit = 0;
    /// The golems' rocks and the skeletons' bones dealt in turn.
    std::uint32_t golemRock = 0;
    std::uint32_t nextBone = 0;
    std::uint16_t nextSummonId = 0; ///< (g_nextSummonId: to 64999, then 0)
    /// The path finder's grid (pathfind.hpp; the exe's 256 x 256): a tile's search
    /// stamp, its moving obstacles' stamp and its flags. Made at the first search.
    struct PathTile {
        std::uint32_t visit = 0;
        std::uint32_t stamp = 0;
        std::uint16_t flags = 0;
        /// Water at one of its edges' middles (the path finder's dryTile, by the floor, which
        /// stands from the map's load): 0 not looked at yet, 1 none, 2 water.
        std::uint8_t water = 0;
    };
    std::vector<PathTile> pathTiles;
    std::uint32_t pathVisit = 0;    ///< the search's stamp
    std::uint32_t pathStamp = 0;    ///< the moving obstacles' marking
    std::uint32_t pathMarkedAt = 0; ///< its frame
    /// The moving obstacles' marks stand: set by a marking, cleared when
    /// an obstacle or a fire (class 0x2008) joins the motion (from
    /// unitHasCollideOrUpdateFn) and by a freeze. A search marks
    /// afresh only while it's clear.
    bool pathMarksStand = false;
    /// The motion queue's next number (physics.hpp queueMotion: the queue's order).
    std::uint64_t motionTicket = 0;
    /// The motion queue itself, in joining order: each one's extent and ticket (one that
    /// left it and joined again counts at its later place).
    std::vector<std::pair<std::uint32_t, std::uint64_t>> motionQueue;
    /// moveObjects' own lists (the queue it took, and its movers), kept from frame to frame to save
    /// their allocations: empty between its calls.
    std::vector<std::pair<std::uint32_t, std::uint64_t>> motionTaken;
    std::vector<ServerObject*> motionMoving;
    std::vector<std::uint64_t> contactPairs; ///< (the motion's touches' pairs, in order: each once)
    int waypointPathStatus = 0;     ///< the last waypoint route's: 0 found, 1 too long, 2 none
    /// A player went through an exit to this map (ExitCollide's mapLoad): the host
    /// loads it.
    std::optional<std::string> exitTo;
    /// The map's name (Map_GetCurrentName: its file's, without the directory and the
    /// four-letter extension).
    std::string mapName;
    /// The scripts' quest variables (newest first: journalQuestSet
    /// type 0, journalQuestSetBool type 1), a solo .plr's GameData's; kept from map
    /// to map.
    struct QuestVariable {
        std::string name;
        std::uint32_t type = 0;
        std::uint32_t value = 0;
    };
    std::vector<QuestVariable> questVariables;
    /// A solo .plr's magic walls waiting for the map (at most 15): where,
    /// whether it changed a wall there and what that was, the wall's material, look, shape and
    /// health. (Raised by restoreMagicWalls at the solo map's load: with the solo saves.)
    struct PendingWall {
        std::uint8_t x = 0;
        std::uint8_t y = 0;
        std::uint32_t replaced = 0;
        std::uint8_t oldMaterial = 0;
        std::uint8_t oldDirection = 0;
        std::uint8_t oldVariant = 0;
        std::uint8_t material = 0;
        std::uint8_t variant = 0;
        std::uint8_t direction = 0;
        std::uint8_t health = 0;
    };
    std::vector<PendingWall> pendingMagicWalls;
    /// A solo .plr's warrior charging as it was saved: the
    /// ability runs again for the first player (with the solo game).
    int pendingAbility = 0;
    /// The byte a .plr's GameData carries last (the
    /// briefing's lock screen, Gui_Brief_ShowLockScreen, sets it on a host with a window).
    std::uint8_t briefingByte = 0;
    /// The trades (trade.hpp: the exe's trade list, newest first; a map change ends them all),
    /// the next one's id, a quest's shops kept for each player index when it left
    /// them (g_questShopSessionByPlayer: ids, 0 none), and the things a trade holds out of the world
    /// (a shop's stock, the offers: made by newObjectByTypeID and in no list).
    std::list<TradeSession> trades;
    std::uint32_t nextTradeId = 1;
    std::array<std::uint32_t, 32> questShops{};
    std::map<std::uint32_t, ServerObject> tradeObjects;
    /// An extent given back for reuse (a trade's thing freed, objectFreeMem).
    void freeExtent(std::uint32_t extent);
    /// The script id the next new object gets (respawnAllow), and taking it.
    std::int32_t nextScriptId() const noexcept { return nextScriptId_; }
    std::int32_t newScriptId() noexcept { return nextScriptId_++; }
    /// Set by a game's start (1000000000) or a loaded game's (its .plr's).
    void setNextScriptId(std::int32_t id) noexcept { nextScriptId_ = id; }
    /// modifier.bin (null: none), for the weapons' and armour's durability (WeaponCreate /
    /// ArmorCreate) and the records' modifiers.
    const things::ModifierDb* modifierDb = nullptr;
    /// The map's client-only things (the client-only types: the server frees them at the load,
    /// servMapLoadPlaceObj; the clients load them from their own map), in the
    /// file's order: their records, for the save (the save writes the host client's).
    struct ClientOnly {
        std::uint16_t type = 0;
        std::string typeName;
        std::shared_ptr<const map::ObjectXfer> record;
    };
    std::vector<ClientOnly> clientOnly;
    /// The owners the load's records name (mapReadWriteObjData's list:
    /// each owned script id with its owner's, not under game flag
    /// 0x200000 / 0x400000), in the order read; set after a saved map's load
    /// (solo_save.hpp soloMapLoaded), then dropped.
    struct PendingOwner {
        std::int32_t owner = 0;
        std::uint32_t owned = 0;
    };
    std::vector<PendingOwner> pendingOwners;

    /// A gamedata number as this game reads it (Game_GetGamedataFloat /
    /// gamedataGetFloatTable: SOLO or ARENA lines by the flags; 0 past a table's
    /// end), from the file or the shipped numbers.
    float number(std::string_view name, std::size_t index = 0) const;
    /// The same by a key (game_data.hpp's GameDataKey: the name found once, for every frame's look-ups).
    float number(const things::GameDataKey& key, std::size_t index = 0) const { return things::gameNumber(gameData, key, gameFlags, index); }
    bool solo() const noexcept { return (gameFlags & (GameFlag::Solo | GameFlag::SoloOther)) != 0; }
    bool quest() const noexcept { return (gameFlags & GameFlag::Quest) != 0; }
    bool network() const noexcept { return (gameFlags & GameFlag::Network) != 0; }
    bool versus() const noexcept { return (gameFlags & GameFlag::Versus) != 0; }
    /// noxcpp: modeOwnsBook in force: on, in a network game that isn't a quest nor one waiting for
    /// its quest to begin (questChosen).
    bool modeBook() const noexcept { return modeOwnsBook && network() && !quest() && !questChosen; }

    /// Loads the map's objects, walls, waypoints and groups, made as a game with `gameFlags`
    /// and `gameData` makes them; the items' modifiers by `modifiers`' names (none without).
    /// The objects take script ids from `nextScriptId` on (the exe's counter, carried from map to
    /// map) and are put in the world at `frame` (createAt's born frame and timer). Throws on a
    /// malformed map.
    static World load(const map::MapFile& map, const things::ThingDb& things,
                      std::uint32_t gameFlags = GameFlag::Network | GameFlag::Arena, const things::GameData* gameData = nullptr,
                      const things::ModifierDb* modifiers = nullptr, const things::MonsterDb* monsterDb = nullptr,
                      std::int32_t nextScriptId = kFirstScriptId, std::uint32_t frame = 0, std::vector<std::uint32_t> reserved = {});
    /// noxcpp's map editor session (server/edit_session.hpp; never in a game the exe could host):
    /// objects added to the running world exactly as the map load reads and places them (each
    /// record's object, its inventory, a client-only one not placed, a door's wall), from a
    /// map's ObjectTOC and ObjectData; then the new things' pass takes them as any made in play.
    /// The extents of the top-level ones placed, in record order.
    std::vector<std::uint32_t> placeLiveRecords(const map::ObjectTOC& toc, const map::ObjectData& data, const things::ThingDb& things,
                                                std::uint32_t frame);

    /// The map's objects (not those held) in its file's order: the exe's object list as the map
    /// load leaves it (each read one createAt to the new list's head, unitsNewAddToList
    /// then moving them newest first to the list's head), for the passes that walk the list then
    /// (rewards.hpp's).
    std::vector<std::uint32_t> loadOrder;

    using ObjectMap = IndexedMap<std::uint32_t, ServerObject, ExtentTable>;
    ObjectMap& objects() noexcept { return objects_; }
    const ObjectMap& objects() const noexcept { return objects_; }
    /// Each object, in the extents' order as a walk of objects() takes them, by the extent table
    /// (no stepping through the map's nodes): for the frame's walks of every object. As the map's
    /// walk, what's put in meanwhile past the one being walked is walked too, and what's taken out
    /// before it's reached isn't; the one being walked mustn't be taken out.
    template <class Fn>
    void eachObject(Fn&& fn)
    {
        const ExtentTable& table = objects_.index();
        for (std::size_t w = 0; w < table.present.size(); ++w) {
            // (Each next one from the word as it stands now: after the call before it.)
            std::uint64_t from = ~std::uint64_t{0};
            for (std::uint64_t bits = table.present[w] & from; bits != 0; bits = table.present[w] & from) {
                const int b = std::countr_zero(bits);
                from = b == 63 ? 0 : ~std::uint64_t{0} << (b + 1);
                fn(*table.objects[w * 64 + static_cast<std::size_t>(b)]);
            }
        }
        for (auto it = objects_.lower_bound(ExtentTable::kLimit); it != objects_.end(); ++it) {
            fn(it->second);
        }
    }
    /// The objects with a player record (gone ones too), in the extents' order: what a walk of
    /// objects() for them finds, in its order. Gathered again only once a player's unit came or went.
    UnitList<ServerObject> playerUnits() { return UnitList<ServerObject>(players_.get(objects_, objects_.index().playersMoved)); }
    UnitList<const ServerObject> playerUnits() const
    {
        return UnitList<const ServerObject>(players_.get(objects_, objects_.index().playersMoved));
    }
    /// The same of the sentry globes (those with a Sentry record).
    UnitList<ServerObject> sentryUnits() { return UnitList<ServerObject>(sentries_.get(objects_, objects_.index().sentriesMoved)); }
    /// The same of the flagball balls (those with a Ball record).
    UnitList<ServerObject> ballUnits() { return UnitList<ServerObject>(balls_.get(objects_, objects_.index().ballsMoved)); }

    /// The map grid and the units update's lists (map_grid.hpp: the exe's).
    MapGrid& grid() noexcept { return grid_; }
    const MapGrid& grid() const noexcept { return grid_; }
    /// Its collision box about (x, y) as makes it (CollideP1 / P2; a shapeless
    /// thing's never set: the zeroes it was made with).
    static MapGrid::Rect collideBoxAt(const ServerObject& o, float x, float y);
    /// Linked at (x, y), its NewPos, its box about there, if it may be: on the map
    /// (not held, not DESTROYED: ours' pendingDelete or gone), not linked already.
    /// (A missile's box isn't made by the link, another call's being for the rest: `missileBox`, its
    /// last objectUnkUpdateCoords's, or about (x, y).)
    void gridLink(ServerObject& o, float x, float y, const MapGrid::Rect* missileBox = nullptr);
    /// moveUpdateSpecial: off the grid, and on again at its NewPos (x, y); a point
    /// off the grid deletes it instead (delayedDeleteObject).
    void gridRelink(ServerObject& o, float x, float y, const MapGrid::Rect* missileBox = nullptr);
    void gridRelink(ServerObject& o) { gridRelink(o, o.x, o.y); }
    /// Off the grid and the lists (servFinalizeDelObject's: picked up, freed).
    void gridRelease(ServerObject& o) { grid_.release(o); }
    /// unitAddToUpdatable (not a missile) / unitRemoveFromUpdatable.
    void addUpdatable(ServerObject& o);
    void removeUpdatable(ServerObject& o) { grid_.removeUpdatable(o); }
    bool updatable(const ServerObject& o) const noexcept { return grid_.updatable(o); }
    /// A thing put into the world (createAt's new list: server_objects_uninited),
    /// for the next new things' pass (physics.hpp queueNewObjects).
    void noteNew(const ServerObject& o) { fresh_.push_back(o.extent); }
    /// The new list as it stands (oldest first): those made or carried in since the last pass,
    /// not on the grid's lists yet (the exe's are on their owner's owned list at once).
    const std::vector<std::uint32_t>& newList() const noexcept { return fresh_; }
    /// The new list taken for the pass, newest first (the exe's walks it from its head).
    std::vector<std::uint32_t> takeNew();

    /// getUnitsInRect: those whose box strictly meets the rectangle, by the grid's
    /// order (map_grid.hpp).
    template <class Fn>
    void unitsInRect(float left, float top, float right, float bottom, Fn&& fn)
    {
        grid_.inRect(MapGrid::Rect{left, top, right, bottom}, fn);
    }
    /// The same, ended at the first `fn` answers true for (MapGrid::inRectUntil).
    template <class Fn>
    void unitsInRectUntil(float left, float top, float right, float bottom, Fn&& fn)
    {
        grid_.inRectUntil(MapGrid::Rect{left, top, right, bottom}, fn);
    }
    /// getUnitsInRectAdv: those, then the missiles in the rectangle's cells
    /// (by their point's cell, no box).
    template <class Fn>
    void unitsInRectAdv(float left, float top, float right, float bottom, Fn&& fn)
    {
        const MapGrid::Rect r{left, top, right, bottom};
        grid_.inRect(r, fn);
        grid_.missilesInRect(r, fn);
    }
    /// unitsGetInCircle: those of the square about (x, y) whose position is
    /// strictly within r.
    /// (The game's x87: r squared and the square's sides stored, chopped; each distance's
    /// squares and sum on the stack.)
    template <class Fn>
    void unitsInCircle(float x, float y, float r, Fn&& fn)
    {
        using util::X87;
        const float r2 = (X87(r) * r).f();
        grid_.inRect(circleSquare(x, y, r), [&](ServerObject& o) {
            const X87 dx = X87(x) - o.x;
            const X87 dy = X87(y) - o.y;
            if (dx * dx + dy * dy < r2) {
                fn(o);
            }
        });
    }
    /// getMissilesInCircle: the missiles in the square's cells within r (or on it).
    template <class Fn>
    void missilesInCircle(float x, float y, float r, Fn&& fn)
    {
        using util::X87;
        const float r2 = (X87(r) * r).f();
        grid_.missilesInRect(circleSquare(x, y, r), [&](ServerObject& o) {
            const X87 dx = X87(x) - o.x;
            const X87 dy = X87(y) - o.y;
            if (dx * dx + dy * dy <= r2) {
                fn(o);
            }
        });
    }
    /// The square about a circle the searches walk: each side one add, stored.
    static MapGrid::Rect circleSquare(float x, float y, float r)
    {
        using util::X87;
        return {(X87(x) - r).f(), (X87(y) - r).f(), (X87(r) + x).f(), (X87(r) + y).f()};
    }
    /// All in the extent list of the cell (x, y) is in.
    template <class Fn>
    void unitsInCell(float x, float y, Fn&& fn)
    {
        grid_.inCell(x, y, fn);
    }
    /// The objects with a player record in the players' order (Plr_GetFirstUnit's: by player
    /// index): during the units update's walk (Server::updateWorld) its list made at the walk's
    /// start (no player is made in it), else gathered now into `scratch`. For the scans that
    /// walk the players (the obelisks', the generators').
    const std::vector<ServerObject*>& playerObjects(std::vector<ServerObject*>& scratch)
    {
        if (walkPlayers) {
            return *walkPlayers;
        }
        const auto units = playerUnits();
        scratch.assign(units.begin(), units.end());
        std::sort(scratch.begin(), scratch.end(), [](const ServerObject* a, const ServerObject* b) { return a->player->index < b->player->index; });
        return scratch;
    }
    /// The walk's player list (Server::updateWorld sets it for the walk; null outside it).
    const std::vector<ServerObject*>* walkPlayers = nullptr;
    ServerObject* byExtent(std::uint32_t extent);
    /// One the last sweep deleted, kept till the next sweep frees it (the exe's deleted list),
    /// if any.
    ServerObject* dying(std::uint32_t extent);
    /// The units update's fix-up of its hurtBy before each one's turn: one
    /// DESTROYED (0x20: deleted, or freed) becomes its owner if that one isn't, else none.
    void settleHurtBy(ServerObject& o);
    /// By script id, as server_scriptValToObjectPtr looks (inventories too, never a
    /// destroyed one: flag 0x20, set as delayedDeleteObject marks it, ours
    /// pendingDelete). -1 and -2 (caller, trigger) are the script host's.
    ServerObject* byScriptId(std::int32_t id);
    /// By map number (the exe's Extent: the number its map or .plr record gave it, fileExtent), as
    /// netGetUnitByExtent looks: a thing on the exe's object list whose number it is. That list is
    /// the world's things (none held, no missile, none new since the last pass, none with 0x20: ours
    /// gone or pendingDelete), each pass's at its head (a load's in its file's order). The exe's
    /// numbers aren't unique: a map may give a held thing and a placed one the same, a thing brought
    /// from another map keeps that map's, a save holds both. The exe takes the list's first (the
    /// newest: a thing put down later takes a map object's place, OB-412); ours, by decision
    /// (VD-47), the one on it longest (the oldest; of one load's, the later record), the map's own.
    /// Ours are keys, so a load numbers such a thing afresh (placeRecords, Server::loadMapKeeping, a
    /// .plr's things), and those are kept by number for this. 0 names none. (A group's members, a
    /// mover's thing, a map object's net code: bit 15 and the number.)
    ServerObject* byMapNumber(std::uint32_t number);
    /// Its map number given after it was put in the world (kept for byMapNumber).
    void setMapNumber(ServerObject& o, std::uint32_t number);
    /// A client's code for a thing as packetDynamicUnitCode, then Obj_GetByNetCode, take it: with
    /// bit 15 a map object's, by its map number (byMapNumber); else the thing it's the code of.
    ServerObject* byPacketCode(std::uint32_t code);
    /// A client's code for a thing as Obj_GetByNetCode alone takes it: one with bit 15 names
    /// nothing (a map object's own NetCode is under 0x8000; its clients know it only by its map
    /// number with bit 15); else the thing it's the code of.
    ServerObject* byNetCode(std::uint32_t code);
    /// By script name (the map editor's), as getObjectByScrName: with no 0x20 test,
    /// so a deleted thing still in the exe's lists is found (one on the map, ACTIVE, or held by
    /// one; a held thing deleted left its holder at once) till the next tick's frees
    /// it: this frame and the next (World::dying_).
    ServerObject* byScriptName(std::string_view name);

    WallCell* wall(int x, int y);
    /// The cell's wall, made (and put at the head of the wall list, WallCell::serial) when it has
    /// none: Map_CreateWallAt.
    WallCell& createWall(int x, int y);
    /// The wall cell a door is the leaf of (from
    /// doorAttachWall), if any.
    std::optional<std::pair<int, int>> doorCell(std::uint32_t door) const;
    /// When the last "locked" notice went to anyone (one timer for every door, chest, ankh and
    /// player: DoorCollide, ChestCollide and AnkhCollide's
    /// refusals; none within 1501 ms of it; here frames).
    std::optional<std::uint32_t> lockNoticeAt;
    using WallMap = IndexedMap<std::pair<int, int>, WallCell, WallTable>;
    WallMap& walls() noexcept { return walls_; }
    const WallMap& walls() const noexcept { return walls_; }
    /// The secret walls' records (wallSecretBlock: each read
    /// put at the head, so the map's last first), by cell.
    std::vector<std::pair<int, int>>& secretWalls() noexcept { return secretWalls_; }
    const std::vector<std::pair<int, int>>& secretWalls() const noexcept { return secretWalls_; }
    /// The map's breakable walls (wallBreackableListAdd: the
    /// map's last first; not the Wall spell's), by cell.
    std::vector<std::pair<int, int>>& breakableWalls() noexcept { return breakableWalls_; }
    const std::vector<std::pair<int, int>>& breakableWalls() const noexcept { return breakableWalls_; }
    /// The WALL materials' flags (thing.bin's, by WallCell::material).
    std::vector<std::uint32_t>& wallDefFlags() noexcept { return wallDefFlags_; }
    /// A WALL material's health and random looks (mapWallGetHpByTile /
    /// wallDefRandomVariations), and the index of one by name (-1 none).
    std::uint8_t wallHealth(int material) const;
    std::uint8_t wallVariations(int material) const;
    int wallMaterial(std::string_view name) const;
    /// The Wall spell's walls, and what their end owes the players they were sent to (the bytes
    /// to send, by player slot).
    std::vector<MagicWall>& magicWalls() noexcept { return magicWalls_; }
    struct WallNotice {
        std::vector<std::uint8_t> bytes;
        std::uint32_t players = 0; ///< by slot
    };
    std::vector<WallNotice>& wallNotices() noexcept { return wallNotices_; }
    /// Whether no wall stands between two points, as mapTraceRay walks it (the
    /// wall shapes' diagonals, cell by cell); `hit` gets the first wall met when one does.
    bool traceRay(float x0, float y0, float x1, float y1, std::uint8_t flags, RayHit* hit = nullptr) const;
    /// The frame's secret wall motion (updateUnits's end): opening and closing
    /// a step each, and those that shut or open by themselves counting down.
    void stepSecretWalls();

    ServerWaypoint* waypoint(std::int32_t id);
    ServerWaypoint* waypointNamed(std::string_view name);
    /// Whether a waypoint's or group's own name `own` matches `name` (each one's part after a
    /// "scope:" the same; the scopes, when both have one, the same but for case).
    static bool namesMatch(std::string_view own, std::string_view name);
    /// The first waypoint whose name matches (namesMatch), in the exe's list order:
    /// the map's.
    ServerWaypoint* waypointMatching(std::string_view name);
    std::vector<ServerWaypoint>& waypoints() noexcept { return waypoints_; }
    /// The waypoints not yet in the map grid's lists put there, by id, where each is now
    /// (waypointMapRegister as each is made: a map's as it's read; one added since,
    /// at the next search).
    void registerWaypoints()
    {
        for (std::uint32_t n = grid_.waypointsRegistered(); n < waypoints_.size(); ++n) {
            grid_.registerWaypoint(n + 1, waypoints_[n].x, waypoints_[n].y);
        }
    }
    /// The frame's sounds (sound.hpp), sent and cleared after each frame.
    std::vector<SoundEvent>& sounds() noexcept { return sounds_; }
    /// The map's polygons, index 1 first (the map's Polygons section).
    std::vector<MapPolygon>& polygons() noexcept { return polygons_; }
    /// The polygon (x, y) is in (polygonIsPlayerInPolygon): `current` first, then
    /// the others in order; with `scripted` only those with an enter or leave script
    /// (a monster's). 1-based; 0 none.
    std::uint32_t polygonAt(int x, int y, std::uint32_t current, bool scripted = false);

    /// server_scriptGetGroup: the group of that id, newest first.
    ServerGroup* group(std::int32_t id);
    ServerGroup* groupNamed(std::string_view name);
    /// A group's kind, a group of groups' that of the group its first member names
    /// (none for an empty one or a missing group: the exe's reads nothing there).
    std::optional<ServerGroup::Kind> groupKind(const ServerGroup& g);
    /// The first group of `kind` (groupKind) whose name matches (namesMatch), newest
    /// first (the script's ObjectGroup, WaypointGroup, WallGroup).
    ServerGroup* groupMatching(std::string_view name, ServerGroup::Kind kind);
    std::vector<ServerGroup>& groups() noexcept { return groups_; }

    /// The floor (a FLOR index) at a point, -1 for none (tileNFromPoint: the
    /// 46-square cell's quarter, its tile or an edge blended over it).
    int floorAt(float x, float y) const;
    /// Whether the map has any floor tile (noxcpp's: a world made without one answers floorAt -1
    /// everywhere).
    bool hasFloor() const noexcept { return anyFloor_; }
    /// A point on a water floor a teleport can't reach (mapTileAllowTeleport).
    bool noTeleportAt(float x, float y) const;


    /// Messages the frame made for the clients, in order (deaths, the kill feed, respawns,
    /// inventory reports): the network side sends and clears them.
    struct Broadcast {
        std::vector<std::uint8_t> bytes;
        bool reliable = true;
        std::uint32_t to = 0; ///< a player's unit's extent: that client alone; 0 everyone
    };
    std::vector<Broadcast>& broadcasts() noexcept { return broadcasts_; }
    /// Objects that left the world this frame (picked up, deleted): the clients that know them
    /// are told (SrvNet_ReportDestroyObject).
    std::vector<std::uint32_t>& removed() noexcept { return removed_; }
    /// Players moved at a stroke this frame (Obj_SetPosition's): their clients
    /// forget what they knew (MSG_FORGET_DRAWABLES), and the network side sends all afresh.
    std::vector<std::uint32_t>& forgetting() noexcept { return forgetting_; }

    /// A new object of `type` at (x, y) (CreateObject); its extent and script id are fresh.
    ServerObject& spawn(const things::ThingDb& things, std::string_view type, float x, float y);
    /// A free extent for something new (newExtent: from kDynamicExtents up; freed ones again
    /// once those under 0x8000 are used).
    std::uint32_t newExtent();
    /// The extents of the things a map change keeps, while the new map's are made (World::load's
    /// `reserved`, cleared once they're in): newExtent never hands them out, nor does a loaded
    /// thing made in play keep one (the exe's things keep their net codes from map to map).
    std::vector<std::uint32_t> reservedExtents;
    /// A thing from another world (a player kept across a map change), under its own extent; the
    /// new ones made after it. `asNew`: on the new list (the new things' pass to come); a thing a
    /// map change keeps isn't (the exe's lists are the game's: it's on them already).
    void adopt(ServerObject o, bool asNew = true);
    /// A new object of `type` put into the world at (x, y) as createAt does: its
    /// flags cleared but the type's (0x35E9FEDB) and ACTIVE, made (born) and its timer
    /// this frame; in a network game not a quest, gold and items (0x3001110, no
    /// missile) NO_COLLIDE. Null for a type thing.bin lacks ("NULL").
    ServerObject* createAt(const things::ThingDb& things, std::string_view type, float x, float y, std::uint32_t frame);
    /// Removes the objects Delete marked (the frame's end). What a deleted one holds stays held by
    /// it till the next sweep (the exe frees it a tick later): then it drops about
    /// where it was (the free's dropAllItems, shaken by `dropRng`: the server's;
    /// null, one of its own), a creature's kept clothing freed (objectFreeMem). `marked`, when
    /// given, is every object marked deleted, by extent (the caller's walk found them): no walk
    /// of its own to find them; `markedAll`: none marked since it was gathered (else the sweep's
    /// own passes look again).
    void sweepDeleted(std::minstd_rand* dropRng = nullptr, const std::vector<std::uint32_t>* marked = nullptr, bool markedAll = false);

    /// noxcpp: what describing a fresh object of a type leaves it (the type's fields and its CREATE
    /// function's, world.cpp), kept by the type's index once made by `make` and copied into each new
    /// one of it: made afresh as thing.bin is reloaded or reindexed, or the game's flags, gamedata,
    /// modifier.bin or monster.bin change. (typeCacheOn: off, every one made afresh: the test that
    /// proves them the same.)
    template <class Make>
    const ServerObject& typeProto(const things::ThingDb& things, std::size_t index, Make&& make) const;
    bool typeCacheOn = true;

    /// The shape a cell's wall presents, -1 for none. `mode`: 0x40 windows
    /// count, 0x10 shut doors do (gates too with 0x80).
    int wallShape(int x, int y, std::uint8_t mode) const;
    /// The wall in a cell as Map_GetWallAtGrid finds it: an even cell's, not broken nor a
    /// door's; or null.
    WallCell* gridWall(int x, int y);

private:
    /// The map load's object records read and placed (World::load's, and placeLiveRecords'
    /// with `live`: not on the load order). The top-level ones placed.
    static std::vector<std::uint32_t> placeRecords(World& w, const map::ObjectTOC& toc, const map::ObjectData& data, const things::ThingDb& things,
                                                   const things::ModifierDb* modifiers, std::uint32_t gameFlags, std::uint32_t frame, bool live);
    /// The map load's elevators and shafts among `placed` (the top-level ones placed, in record
    /// order) linked to each other by extent, from each elevator's record's number for its shaft.
    static void linkElevators(World& w, const std::vector<std::uint32_t>& placed);
    /// The map load's teleport pads among `placed` each linked to its partner by extent, from its
    /// record's number for it (one way).
    static void linkTransporters(World& w, const std::vector<std::uint32_t>& placed);
    /// A door's leaf takes the cell half its shut swing reaches (XFerDoor -> doorAttachWall).
    void attachDoorWall(const ServerObject& o);
    /// How many of the cell's wall segments the ray crosses within its span,
    /// the nearer first in `hit` (x, y pairs).
    int rayCell(int x, int y, const float ray[4], float hit[4], std::uint8_t flags) const;

    ObjectMap objects_;
    /// The things put in the world under an extent that isn't their map number, by that number
    /// (byMapNumber's other candidates, in the order they came; one gone, or the extent another's
    /// since, is passed over there). Few: a load's clashes, the things a map change or a .plr brings.
    std::map<std::uint32_t, std::vector<std::uint32_t>> renumbered_;
    /// Notes `o` in renumbered_ when its map number isn't 0 nor its extent.
    void noteRenumbered(const ServerObject& o);
    /// playerUnits' (sentryUnits') list as last gathered, and the counts it was gathered at
    /// (ExtentTable's, the record watch's): gathered again when either has moved on. Not carried by
    /// a move (gathered afresh).
    template <class Watch, auto Member>
    struct UnitCache {
        std::shared_ptr<const std::vector<ServerObject*>> list;
        std::uint32_t moved = 0;
        std::uint32_t records = 0;
        UnitCache() = default;
        UnitCache(const UnitCache&) noexcept {}
        UnitCache& operator=(const UnitCache&) noexcept
        {
            list.reset();
            return *this;
        }
        const std::shared_ptr<const std::vector<ServerObject*>>& get(const ObjectMap& objects, std::uint32_t movedNow)
        {
            const std::uint32_t recordsNow = Watch::changes.load(std::memory_order_relaxed);
            if (!list || moved != movedNow || records != recordsNow) {
                auto fresh = std::make_shared<std::vector<ServerObject*>>();
                for (const auto& [extent, o] : objects) {
                    if (o.*Member) {
                        fresh->push_back(const_cast<ServerObject*>(&o));
                    }
                }
                list = std::move(fresh);
                moved = movedNow;
                records = recordsNow;
            }
            return list;
        }
    };
    mutable UnitCache<PlayerRecordWatch, &ServerObject::player> players_;
    mutable UnitCache<SentryRecordWatch, &ServerObject::sentry> sentries_;
    mutable UnitCache<BallRecordWatch, &ServerObject::ball> balls_;
    /// typeProto's types, and what they were made under.
    struct TypeCache {
        const things::ThingDb* things = nullptr;
        const things::ThingDef* thingData = nullptr;
        std::size_t thingCount = 0;
        std::uint32_t thingReindexes = 0;
        std::uint32_t gameFlags = 0;
        const things::GameData* gameData = nullptr;
        const things::ModifierDb* modifiers = nullptr;
        const things::MonsterDb* monsters = nullptr;
        std::vector<std::unique_ptr<const ServerObject>> protos; ///< by type index (null: not made yet)
    };
    mutable TypeCache typeCache_;
    MapGrid grid_;
    std::vector<std::uint32_t> fresh_; ///< the new list (noteNew), oldest first
    WallMap walls_;
    std::uint32_t nextWallSerial_ = 0; ///< WallCell::serial's next
    std::vector<std::pair<int, int>> secretWalls_;
    std::vector<std::pair<int, int>> breakableWalls_;
    std::vector<std::uint32_t> wallDefFlags_; ///< WALL flags by material
    std::vector<std::uint8_t> wallDefHealth_;
    std::vector<std::uint8_t> wallDefVariations_;
    std::vector<std::string> wallDefNames_;
    std::vector<MagicWall> magicWalls_;
    std::vector<WallNotice> wallNotices_;
    std::vector<Broadcast> broadcasts_;
    std::vector<std::uint32_t> removed_;
    std::vector<std::uint32_t> forgetting_;
    std::list<DurationSpell> durationSpells_;
    std::vector<std::uint32_t> pendingCasts_;
    std::list<ActiveAbility> activeAbilities_;
    std::vector<ServerWaypoint> waypoints_;
    std::vector<MapPolygon> polygons_;
    std::vector<SoundEvent> sounds_;
    std::uint32_t polygonRay_ = 0; ///< the point-in-polygon ray's end, by its bit 2
    std::vector<ServerGroup> groups_;
    /// The floor: 128 x 128 cells of 46, each two tiles (the odd and
    /// the even), each with the edges blended over it (their floor, the EDGE's rows and columns,
    /// its direction).
    struct FloorEdgeCell {
        std::int16_t floor = 0;
        std::uint8_t rows = 0;
        std::uint8_t cols = 0;
        std::uint8_t direction = 0;
    };
    struct FloorHalf {
        std::int16_t floor = -1;
        std::vector<FloorEdgeCell> edges;
    };
    std::vector<std::array<FloorHalf, 2>> floor_;  ///< x * 128 + y
    bool anyFloor_ = false;                        ///< (hasFloor: the map's floor has a tile)
    std::vector<bool> noTeleportFloors_;           ///< by FLOR index
    std::uint32_t nextExtent_ = kDynamicExtents;
    std::deque<std::uint32_t> freedExtents_; ///< deleted dynamic ones, oldest first
    std::int32_t nextScriptId_ = kFirstScriptId;
    /// The last sweep's deleted things the exe keeps on its deleted list (DeletedAt this frame:
    /// the sweep frees them at the next tick's end): out of objects_, but still linked in the
    /// grid (the searches meanwhile find them, 0x20 set: ours pendingDelete), each one's map node
    /// kept (so its address); `named`: still in the exe's lists by name (byScriptName). And every
    /// dynamic extent (given back at the next sweep).
    struct Dying {
        std::map<std::uint32_t, ServerObject>::node_type node;
        bool named = false;
    };
    std::vector<Dying> dying_;
    std::vector<std::uint32_t> lingeringExtents_;
    /// The last sweep's deleted holders whose things the exe drops as it frees them at the next
    /// tick's sweep (dropAllItems): each one's copy as it was,
    /// its things meanwhile still held by it (in the extents' order; the exe's is the order deleted).
    std::vector<ServerObject> freeing_;
};

/// A thing made from an object record (a map's, a .plr inventory's), as World::load reads each:
/// its type's numbers, then the record's header and what its type's xfer the server keeps. Its
/// extent and script id are the record's; it isn't in the world.
ServerObject objectFromRecord(World& world, const map::ObjectXfer& xfer, std::string_view typeName, const things::ThingDb& things,
                              const things::ModifierDb* modifiers);
/// A type's flags as the type reader leaves them (FLAGS, ENABLED, NO_COLLIDE without a COLLIDE).
std::uint32_t typeFlags(const things::ThingDef& t);
/// A client-only type (not a missile, IMMOBILE, not VISIBLE_ENABLE, NO_COLLIDE without SHADOW
/// in its typeFlags): the clients load its things from their own map; the
/// server frees them at the load (servMapLoadPlaceObj).
bool clientOnlyType(const things::ThingDef& t);
/// A new thing of `type` as its type makes it (its flags, xStatus...), not in the world.
ServerObject describeType(World& world, const things::ThingDb& things, std::string_view type);

/// SrvNet_SendInformString's message: MSG_INFORM 0xA9 kind 0x0F, {u8 sign, the string table id
/// the client looks up, NUL}. (Who it goes to, and when, is the caller's.)
inline std::vector<std::uint8_t> informStringBytes(std::string_view id, std::uint8_t sign = 0)
{
    std::vector<std::uint8_t> m;
    m.reserve(id.size() + 4);
    m.push_back(0xA9);
    m.push_back(0x0F);
    m.push_back(sign);
    m.insert(m.end(), id.begin(), id.end());
    m.push_back(0);
    return m;
}

/// The players' units `pick` takes (each with its PlayerControl), in the player list's order
/// (Plr_GetFirstUnit / Plr_GetNextUnit: by index; the world's order among equals).
template <class Pick>
std::vector<ServerObject*> playersInOrder(World& world, Pick pick)
{
    std::vector<ServerObject*> units;
    for (ServerObject* o : world.playerUnits()) {
        if (o->player && pick(*o)) {
            units.push_back(o);
        }
    }
    std::ranges::stable_sort(units, [](const ServerObject* a, const ServerObject* b) { return a->player->index < b->player->index; });
    return units;
}

/// netSendPointFx: MSG_FX `op` at (x, y) (the exe's to the players who see the point; everyone
/// here), the point in whole units, each __ftol'd and sent as a u16.
inline void pointFx(World& world, std::uint8_t op, float x, float y)
{
    const auto ix = static_cast<std::uint16_t>(static_cast<int>(x));
    const auto iy = static_cast<std::uint16_t>(static_cast<int>(y));
    world.broadcasts().push_back({{op, static_cast<std::uint8_t>(ix), static_cast<std::uint8_t>(ix >> 8), static_cast<std::uint8_t>(iy),
                                   static_cast<std::uint8_t>(iy >> 8)},
                                  false,
                                  0});
}

} // namespace nox::server
