// Bots: players driven by a brain instead of a person.
//
// A brain sees the game through a BotView and plays through a BotBody, which carries exactly
// what a player's input can say (the controls of MSG_PLAYER_INPUT, the cursor, abilities, item
// actions, chat). Neither knows how the bot is connected, so the same brain plays:
//   - over the network, as a client of any server, vanilla 1.2b included (net_bot.hpp: the body
//     is a HeadlessClient, the view is built from its World);
//   - inside a game we host, with no connection at all (bot_host.hpp: the host's server shows
//     the bot the game and takes its controls directly). Until noxcore runs the server,
//     emulated_host.hpp stands in for it.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nox::client {
class NavGrid;
}

namespace nox::things {
struct ThingDb;
struct ModifierDb;
struct GameData;
} // namespace nox::things

namespace nox::bot {

/// What an item is to a bot.
enum class ItemKind : std::uint8_t { Other, Weapon, Staff, Armor, HealthPotion, ManaPotion, CurePotion, Quiver };

/// What a type of thing is to a bot.
struct ItemInfo {
    ItemKind kind = ItemKind::Other;
    std::uint8_t classUse = 0x7; ///< modifier.bin CLASSUSE: bit 0 warrior, 1 wizard, 2 conjurer
    /// How good it is: a melee weapon's damage at 100 strength (DAMAGE_MIN + DAMAGE_COEFFICIENT
    /// x 100: WarHammer 150, GreatSword 104, Longsword 39; 0 for thrown and shot ones), armour's
    /// ARMOR_VALUE x 1000.
    float value = 0;
    std::uint8_t slot = 0;  ///< armour: which piece (helmet, boots, shield...), 1 up; 0 none
    /// A melee weapon's reach from our centre to a player's centre: its RANGE plus both
    /// players' radius (10 each: a blow hits what's within the wielder's radius + RANGE of its
    /// centre, measured to the target's edge); a WarHammer's 35 ahead plus its RANGE
    /// round the head. 0 if not a melee weapon.
    float reach = 0;
    bool blocks = false;    ///< a shield or a greatsword: turns projectiles from the front
    bool twoHanded = false; ///< a GreatSword or a WarHammer: no shield with it
    bool ranged = false;    ///< a bow or a crossbow: it shoots (with a quiver)
    bool crossbow = false;  ///< a crossbow: shoots at once, then reloads (a taunt cuts that short)
    /// A staff that shoots (CLASS WAND, a STAFF_ subclass: Fireball, Death Ray...) with its
    /// charges; the plain wooden staff only strikes.
    bool wand = false;
    bool missile = false;   ///< CLASS MISSILE: fireballs, magic missiles, arrows...
    /// SUBCLASS VISIBLE_OBELISK / INVISIBLE_OBELISK: Drain Mana drains it;
    /// `refills`: an ObeliskUpdate one, which refills a player standing by it.
    bool obelisk = false;
    bool refills = false;
    bool flag = false; ///< CLASS FLAG: a team's flag
    bool creature = false; ///< CLASS MONSTER, not IMMOBILE: a summon, a charmed or the map's creature
    /// The stamina a swing of it takes (weaponGetStaminaByType: a Longsword 70,
    /// a WarHammer 100); a swing with less doesn't happen (playerInputAttack).
    std::uint8_t swingStamina = 0;
};

/// The stamina a swing takes with a weapon of these SUBCLASS bits (the player's WeaponEquip),
/// 0 for bare hands (weaponGetStaminaByType).
std::uint8_t swingStamina(std::uint32_t weaponBits);

/// The gamedata.bin numbers the brains plan by, read once from the bots' own copy of the file (as
/// a client reads its own) as a network game reads it.
struct GameNumbers {
    float drainRange = 0;    ///< ManaDrainRange: how far Drain Mana reaches a source
    float harpoonFlight = 0; ///< MaxHarpoonFlightDistance: how far the harpoon flies
    static GameNumbers from(const things::GameData& gameData);
};

/// What things are by type id (thing.bin THNG index + 1): kinds from CLASS and SUBCLASS, who may
/// use the weapons and armour and how good they are from modifier.bin (without it: anyone, no
/// values); and the gamedata.bin numbers the brains plan by (GameNumbers; without the file: 0).
class ItemKinds {
public:
    ItemKinds() = default;
    explicit ItemKinds(const things::ThingDb& things, const things::ModifierDb* modifiers = nullptr,
                       const things::GameData* gameData = nullptr);
    const GameNumbers& numbers() const noexcept { return numbers_; }
    const ItemInfo& info(std::uint16_t typeId) const noexcept
    {
        static const ItemInfo none;
        return typeId > 0 && typeId <= infos_.size() ? infos_[typeId - 1] : none;
    }
    ItemKind kind(std::uint16_t typeId) const noexcept { return info(typeId).kind; }
    /// Whether a player of `playerClass` (0 warrior, 1 wizard, 2 conjurer) may wield or wear it:
    /// the server refuses otherwise (armor.c:ArmorEquipClassFail).
    bool usableBy(std::uint16_t typeId, std::uint8_t playerClass) const noexcept
    {
        return (info(typeId).classUse >> playerClass) & 1;
    }
    /// The team colour a material modifier stands for (MaterialTeamRed 1, MaterialTeamBlue 2...:
    /// the table FlagDraw reads), 0 for any other. A CTF flag's material is
    /// its team's colour, and its team the one of that colour. (Without modifier.bin:
    /// 0 for all.)
    std::uint8_t teamColorOf(std::uint8_t material) const noexcept { return teamColors_[material]; }

private:
    std::vector<ItemInfo> infos_;
    std::array<std::uint8_t, 256> teamColors_{};
    GameNumbers numbers_;
};

/// Another player as the bot sees them; or a creature (`creature`: a conjurer's summons, a
/// monster), which is fought as a player is.
struct SeenPlayer {
    std::uint16_t netCode = 0;
    std::string name;
    float x = 0;
    float y = 0;
    bool placed = false; ///< in sight: its position is known
    std::uint32_t team = 0; ///< 0 none
    std::uint8_t playerClass = 0;
    bool dead = false;
    bool observer = false;
    bool invisible = false;
    std::uint8_t anim = 0; ///< their animation (things::kPlayerStates: 27 SWORD_STRIKE...)
    bool hasFlag = false;  ///< carrying a flag (WeaponEquip bit 1): an enemy has ours
    std::uint32_t buffs = 0; ///< their enchantments (bit n = ENCHANT n: 3 confused, 4 slowed, 5 held...)
    bool poisoned = false;
    bool creature = false;   ///< not a player: a creature (no class, no flag; anim stays 0)
    std::uint16_t typeId = 0; ///< a creature's thing type
};

/// Player animations that are a blow coming (punches, the strikes and the thrust).
bool isStrike(std::uint8_t anim);
/// The Berserker Charge animation.
inline constexpr std::uint8_t kChargeAnim = 45;

/// An item on the ground.
struct SeenItem {
    std::uint16_t netCode = 0;
    std::uint16_t typeId = 0;
    ItemKind kind = ItemKind::Other;
    float x = 0;
    float y = 0;
    bool usable = true; ///< our class may wield or wear it
    float value = 0;    ///< ItemInfo::value
    std::uint8_t slot = 0;
};

/// An item the bot carries.
struct OwnItem {
    std::uint16_t netCode = 0;
    std::uint16_t typeId = 0;
    ItemKind kind = ItemKind::Other;
    bool equipped = false;
    bool usable = true; ///< our class may wield or wear it
    float value = 0;    ///< ItemInfo::value
    std::uint8_t slot = 0;
    bool blocks = false;
    float reach = 0; ///< ItemInfo::reach
    std::uint8_t swingStamina = 0; ///< ItemInfo::swingStamina
    bool twoHanded = false;        ///< ItemInfo::twoHanded
    bool ranged = false;           ///< ItemInfo::ranged
    bool crossbow = false;         ///< ItemInfo::crossbow
    /// A quiver's arrows or bolts, or a staff's charges, left (MSG_REPORT_CHARGES, sent with
    /// each shot); unknown until the first. The server takes an empty quiver away.
    std::optional<std::uint8_t> charges;
    bool wand = false; ///< ItemInfo::wand
};

/// A missile in flight (a fireball, a magic missile, an arrow), whoever's it is.
struct SeenMissile {
    std::uint16_t netCode = 0;
    std::uint16_t typeId = 0;
    float x = 0;
    float y = 0;
};

/// The game as one bot sees it, once a game frame.
struct BotView {
    std::uint32_t frame = 0;     ///< game frame (30 a second)
    GameNumbers numbers;         ///< gamedata.bin's (ItemKinds::numbers)
    std::uint32_t gameFlags = 0; ///< GAME_SETTINGS' flags (0x1000 quest, 0x800 solo...)
    bool inGame = false;         ///< in the world, not joining or loading a map
    std::uint16_t netCode = 0;
    std::uint8_t playerClass = 0; ///< 0 warrior, 1 wizard, 2 conjurer
    float x = 0;
    float y = 0;
    bool placed = false; ///< our position is known
    bool dead = false;
    bool observer = false;
    std::uint8_t anim = 0; ///< our animation (a swing under way can't be cut short)
    std::uint32_t team = 0;
    std::uint16_t health = 0;
    std::uint16_t maxHealth = 0;
    std::uint16_t mana = 0;
    std::uint16_t maxMana = 0;
    std::uint8_t stamina = 0; ///< a jump takes 90 (playerSubStamina in Plr_UpdateState)
    bool poisoned = false;
    std::array<bool, 6> abilityReady{}; ///< by ability (1 berserker charge ... 5 eye of the wolf)
    std::array<std::uint8_t, 6> abilityLevel{};
    std::vector<OwnItem> inventory;
    std::vector<SeenPlayer> players; ///< everyone else in the game
    /// The creatures we can see that aren't on our side (an enemy conjurer's summons, the map's
    /// monsters): not ours, not on our friends list, not of our team. Fought as players are,
    /// though a player near them comes first.
    std::vector<SeenPlayer> creatures;
    std::vector<SeenItem> items;     ///< items lying where we can see them
    /// The items the map lays out where they start (and respawn, in arena games): what a player
    /// who knows the map knows, whether or not they're there now. netCode 0. May be null.
    const std::vector<SeenItem>* mapItems = nullptr;
    /// Where the map keeps each team's flag (CTF bases): {team, x, y}. May be null.
    struct FlagBase {
        std::uint32_t team = 0;
        float x = 0;
        float y = 0;
    };
    const std::vector<FlagBase>* flagBases = nullptr;
    /// The map's obelisks: mana for casters. A `refills` one gives a player within 50 who it
    /// can see a mana a frame from its charge (0-50, back 1 every 15 frames: ObeliskUpdate
    /// updateObelisk); any can be drained with Drain Mana from up to ManaDrainRange (240). One with
    /// a team gives only to that team.
    struct ManaSource {
        float x = 0;
        float y = 0;
        std::uint32_t team = 0;
        bool refills = false;
        std::optional<std::uint8_t> charge; ///< as its light shows it; unknown until it changes
    };
    std::vector<ManaSource> manaSources;
    /// The map's elevator cars where they rest at the bottom, and whether one can be stepped onto
    /// now: its height (0-64, rising and falling on its own every few seconds: ElevatorUpdate
    /// updateElevator) at most 20, as MSG_DRAW_FRAME's height / 4 shows it (unknown: taken as down).
    struct RideCar {
        float x = 0;
        float y = 0;
        bool down = true;
    };
    std::vector<RideCar> elevatorCars;
    /// The flags lying where we can see them (enabled flag drawables: a carried one isn't),
    /// with their team. Ours away from its base is dropped: a touch sends it home. Theirs away
    /// from theirs: a touch takes it.
    std::vector<FlagBase> flags;
    bool carryingFlag = false; ///< we have the enemy's flag (WeaponEquip bit 1)
    /// What the squad wants of us in a team game (BotSquad assigns it; Free in free for all).
    /// Hunt: an enemy has our flag; after them (to where they're seen, else to their base, where
    /// they must take it). Escort: a teammate (`leader`) has their flag; with them, a step ahead
    /// on the way home, fighting whoever comes at them.
    enum class Role : std::uint8_t { Free, Guard, Attack, Follow, Hunt, Escort };
    Role role = Role::Free;
    std::uint16_t leader = 0; ///< Follow, Escort: the teammate we keep near (their netCode)
    /// Carrying their flag: the teammates the squad has running with us (their netCodes).
    std::vector<std::uint16_t> escorts;
    /// People having a hard time against the bots lately (their netCodes): a gentle bot
    /// (Skill::gentle) goes easy on them.
    std::vector<std::uint16_t> struggling;
    std::vector<SeenMissile> missiles;
    const client::NavGrid* nav = nullptr; ///< the map's walls (may be null or empty)
    /// The Wall spell's walls standing now, by wall-grid cell (23 units; world = cell x 23).
    /// Paths go round them; with no way round, they're broken (blows and blasts wear them down:
    /// they're breakable walls, damageToMap).
    std::vector<std::pair<int, int>> magicWalls;
    /// What a player sees: their screen round them, half its size each way (a 640 x 480 client:
    /// 320 x 240), less what walls hide. The bot sees no more than that.
    float viewHalfWidth = 320;
    float viewHalfHeight = 240;
    std::uint32_t buffs = 0; ///< our enchantments (bit n = ENCHANT n: 5 held, 25 frozen...)
    /// The spells we've been given (the spellbook's awards), with what they cost.
    struct KnownSpell {
        std::uint32_t id = 0;      ///< things::spellId's numbering (SPELL_MAGIC_MISSILE 50...)
        std::uint8_t level = 0;    ///< 1-3
        std::uint8_t manaCost = 0; ///< thing.bin's SPEL cost (0 for the ones paid as they go); a summoning's own price
        /// A summoning's creature's size in the cage's cells (1 small, 2 medium, 4 large); 0 for
        /// any other spell.
        std::uint8_t creatureSize = 0;
    };
    std::vector<KnownSpell> spells;
    /// The cells free in our creature cage (4, less 1, 2 or 4 for each small, medium or large
    /// creature we have): a summoning that doesn't fit isn't let happen (CreatureControlFailed).
    std::uint8_t summonRoom = 4;
    /// How many frames our input takes to reach the server (half our ping): where things will
    /// be by the time what we do now happens.
    float lagFrames = 0;
    /// Whoever runs the bot wants it out of play for now (nobody to play against): it goes to
    /// observer mode and stays there until this clears.
    bool standDown = false;
};

/// For logs: a part's name ("" for Free).
constexpr const char* roleName(BotView::Role role)
{
    constexpr const char* kNames[] = {"", "guard", "attack", "follow", "hunt", "escort"};
    return kNames[static_cast<std::size_t>(role)];
}

/// Warrior abilities (MSG_TRY_ABILITY ids, the ability table).
namespace Ability {
inline constexpr std::uint8_t BerserkerCharge = 1;
inline constexpr std::uint8_t WarCry = 2;
inline constexpr std::uint8_t Harpoon = 3;
inline constexpr std::uint8_t TreadLightly = 4;
inline constexpr std::uint8_t EyeOfTheWolf = 5;
} // namespace Ability

/// What a bot can do: a player's input. The held ones stay until changed, as a player's keys
/// do; the rest happen once.
class BotBody {
public:
    virtual ~BotBody() = default;
    /// Runs toward `direction` (0-255, Nox units: 0 east, 64 south), facing it; nullopt stops.
    /// Walking or running is the server's call, from how far the cursor is (past 100: running).
    virtual void setMove(std::optional<std::uint8_t> direction) = 0;
    /// Faces `direction` while standing (moving faces the way it goes).
    virtual void setFacing(std::uint8_t direction) = 0;
    /// The cursor's world point: where aimed abilities and spells go.
    virtual void setCursor(int x, int y) = 0;
    /// Holds the Action control: swings the weapon every frame it's held.
    virtual void setAction(bool held) = 0;
    /// Action once: leaves observer mode or respawns.
    virtual void pressAction() = 0;
    virtual void jump() = 0;
    virtual void useAbility(std::uint8_t ability) = 0;
    virtual void castSpells(std::span<const std::uint32_t> spells, bool onSelf) = 0;
    virtual void pickup(std::uint16_t netCode) = 0;
    virtual void useItem(std::uint16_t netCode) = 0;
    virtual void equip(std::uint16_t netCode) = 0;
    virtual void say(std::string_view text, bool team) = 0;
    /// The taunt (MSG_SOCIAL: the fist shake). It ends an attack under way (Plr_SetState), so
    /// players use it to cut a crossbow's reload short.
    virtual void taunt() = 0;
    virtual void joinTeam(std::uint32_t team) = 0;
    /// Asks to watch instead of play (the in-game menu's Observer Mode); Action brings it back.
    virtual void observe() = 0;
};

/// How well a bot plays.
struct Skill {
    int reactionFrames = 9;      ///< from seeing an enemy (or dying) to acting on it
    float aimError = 8;          ///< facing error, in Nox direction units (256 a turn)
    /// A shot's facing error (a bolt or a staff's shot flies the way we face, and a player is
    /// only ~20 wide: at 250 away one unit is 6 off).
    float shotError = 1;
    float retreatHealth = 0.35f; ///< below this part of max health it drinks a health potion
    bool useAbilities = true;
    int abilityChance = 60;      ///< percent: when an ability fits, how likely it's used that moment
    int dodgeChance = 60;        ///< percent: a blow or a charge seen coming is stepped out of
    int slowdown = 0;            ///< frames added between its blows and casts
    /// Goes easy on someone who's having a hard time against the bots (BotView::struggling):
    /// fights them as gently(). Easy bots only.
    bool gentle = false;

    /// This skill eased off: slower to react and to strike, worse aim, fewer abilities and
    /// dodges.
    Skill gently() const;

    static Skill easy();
    static Skill normal();
    static Skill hard();
};

/// Plays one bot. think() runs once a game frame and sets the body's controls.
class Brain {
public:
    virtual ~Brain() = default;
    virtual void think(const BotView& view, BotBody& body) = 0;
};

/// Nox's direction (0-255: 0 east, 64 south) from one point toward another.
std::uint8_t directionTo(float fromX, float fromY, float toX, float toY);

/// A bot's name: "[BOT] " and a random one from a list (at most 24 characters in all, the name
/// field's wchar[25]), none in `taken` (compared ignoring case), so people know who's real and
/// no two bots in a game share one. With the list used up, "[BOT] Bot <n>".
std::string botName(std::mt19937& rng, const std::set<std::string>& taken);

} // namespace nox::bot
