// What every class's brain shares: playing Nox as a player does, whatever the class.
//
//   - dead or observing: respawns after a moment (Action); told to stand down, observes;
//   - follows everyone's and every missile's movement (to lead and dodge them);
//   - sees as a player sees (its screen, not through walls) and remembers a target that goes
//     out of sight for a little while;
//   - walks the map's paths (round walls, objects and the Wall spell's walls; over lava it can
//     jump; on teleporters and elevators), steps out of hazards;
//   - drinks potions when hurt or poisoned (mana ones too for a class that uses mana), puts on
//     gear better than what it wears (and a quiver with a bow), seeks out obelisks when low on
//     mana;
//   - picks items up, runs to where the map keeps gear it wants, takes its part in a team game
//     (guard, attack, follow), else roams.
// What a class does differently is a handful of hooks: how it fights (fight), what reach it
// fights at (prepare), anything before picking a target (react: a warrior's block), what's
// worth having (gearWorth), how it equips (tendGear), and what it does out of a fight (idle: a
// caster's buffs). WarriorBrain and CasterBrain (WizardBrain, ConjurerBrain) are the classes.
#pragma once

#include "noxcore/bot/bot.hpp"
#include "noxcore/client/navigation.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <optional>
#include <random>
#include <set>
#include <utility>
#include <vector>

namespace nox::bot {

/// Follows a path round the map's walls toward a goal, planning again when the goal moves or
/// progress stops.
class Navigator {
public:
    /// The direction to move this frame to get to (gx, gy), or nullopt when there (within
    /// `arrive`) or there's no way.
    std::optional<std::uint8_t> steer(const BotView& view, float gx, float gy, float arrive = 12);
    void reset()
    {
        path_.clear();
        unreachable_ = false;
    }
    /// No progress for a while despite moving: the caller may give up on the goal.
    bool stuck() const noexcept { return stuckFrames_ > 90; }
    /// The last plan found no way to the goal (walled off, or off the map's floor); steer() then
    /// takes us as near it as can be got to.
    bool unreachable() const noexcept { return unreachable_; }
    /// The longest stretch of hazard (flames, lava, spikes) the way ahead crosses, in world units:
    /// 0 when it keeps clear.
    float hazardAhead(const BotView& view) const;
    /// The furthest point of the way ahead, at most `maxDistance` from us, that we can see and
    /// could stand on (a teleport's landing); nullopt if none (no plan, or nothing past us).
    std::optional<std::pair<float, float>> farthestInSight(const BotView& view, float maxDistance) const;
    /// How far the planned way still runs from us (0 with no plan).
    float remaining(const BotView& view) const;
    /// The length of a way of waypoints from `from`, leg by leg.
    static float pathLength(std::pair<float, float> from, std::span<const std::pair<int, int>> path);
    /// Held up by a ride: on its pad waiting to be carried, or beside an elevator whose car is
    /// up (a fight with whoever's hitting us comes before it).
    bool waiting() const noexcept { return waitingSince_.has_value() || carWaitSince_.has_value(); }

private:
    std::vector<std::pair<int, int>> path_;
    bool unreachable_ = false;
    std::pair<float, float> goal_{-1e9f, -1e9f};
    std::uint32_t plannedAt_ = 0;
    std::pair<float, float> progressFrom_{};
    int stuckFrames_ = 0;
    int side_ = 1; ///< which way it last stepped aside when stuck
    std::optional<std::uint32_t> waitingSince_; ///< standing on a ride's pad since
    std::optional<client::NavGrid::Link> waitingOn_; ///< that ride (a step off a pentagram keeps it)
    /// Beside an elevator whose car is up (too high to step onto) since: waiting for it to come
    /// down rather than walking into it; after kCarPatience it's planned round for a while.
    std::optional<std::uint32_t> carWaitSince_;
    static constexpr std::uint32_t kCarPatience = 300; ///< 10 s (a cycle is ~3 s: ElevatorUpdate)
    /// Cells we ran into without getting through (the grid had them open), until when: paths
    /// plan round them for a while (kBumpFrames).
    std::map<std::pair<int, int>, std::uint32_t> bumped_;
    std::set<std::pair<int, int>> avoid_; ///< bumped_'s cells, as findPath takes them
    static constexpr std::uint32_t kBumpFrames = 900; ///< 30 s
    /// Hemmed in (walls ahead and to both sides the grid didn't know: a corner walled on three
    /// sides): at kBackOutAt frames stuck, the cells to either side are planned round too and it
    /// backs out the way it came for kBackOutFrames, then plans a new way.
    std::optional<std::uint32_t> backOutUntil_;
    std::uint8_t backOutDirection_ = 0;
    static constexpr int kBackOutAt = 55;
    static constexpr std::uint32_t kBackOutFrames = 20;

public:
    /// For tests: cells planned round since we ran into them.
    std::size_t bumps() const noexcept { return bumped_.size(); }
};

class PlayerBrain : public Brain {
public:
    void think(const BotView& view, BotBody& body) final;

    /// For tests and logs: who it's after (0 none).
    std::uint16_t target() const noexcept { return target_; }
    /// For tests and logs: the map item spot it's running to (BotView::mapItems index).
    std::optional<std::size_t> goalSpot() const noexcept { return goalSpot_; }
    /// For tests and logs: the mana source it's going to (BotView::manaSources index).
    std::optional<std::size_t> manaGoal() const noexcept { return manaGoal_; }
    /// For tests and logs: cells it's planning round since it ran into them.
    std::size_t bumps() const noexcept { return nav_.bumps(); }
    /// For logs: what it was at on its last think ("fight", "flag home", "break wall"...).
    const char* doing() const noexcept { return wallBreakAt_ != 0 && wallBreakAt_ == frame_ ? "break wall" : doing_; }

    /// The distance it fights at now (a weapon's reach, a caster's casting range).
    float reach() const noexcept { return reach_; }

    /// A target out of sight is chased to where it was last seen for this long (3 s), then
    /// forgotten.
    static constexpr std::uint32_t kMemoryFrames = 90;
    /// GAME_SETTINGS' flag of a chat map (no game yet: the host picks the next).
    static constexpr std::uint32_t kChatMode = 0x80;
    /// On a chat map a bot wanders no further than this from where it came in.
    static constexpr float kLoungeRadius = 100;
    /// A carrier waits for its escort when it's this far behind, at most kEscortPatience (3 s),
    /// then not again for kEscortWaitAgain (10 s).
    static constexpr float kEscortGap = 250;
    static constexpr std::uint32_t kEscortPatience = 90;
    static constexpr std::uint32_t kEscortWaitAgain = 300;
    /// How far a running jump carries (an estimate: the Jump control pushes twice the player's
    /// speed along the facing for the JUMP animation, Plr_UpdateState state 0x0C), and the
    /// stamina it takes (90).
    static constexpr float kJumpReach = 100;
    static constexpr std::uint8_t kJumpStamina = 90;
    static constexpr std::uint32_t kJumpEvery = 30; ///< at most a jump this often
    /// A hazard on the way to an item wider than this isn't jumped: the item's left be.
    static constexpr float kJumpableHazard = 56;
    /// Items are picked up from this far (the server's reach, less a margin).
    static constexpr float kPickupReach = 34;

protected:
    friend class Navigator; // (its hazard checks are the same as ours)
    PlayerBrain(Skill skill, std::uint32_t seed);

    // ---- what a class decides
    /// Each frame it's in play, first: sets reach_ (the distance it fights at).
    virtual void prepare(const BotView& view) = 0;
    /// Before choosing a target (a warrior turning to block a missile). True: done this frame.
    virtual bool react(const BotView& view, BotBody& body);
    /// The fight with `enemy` this frame.
    virtual void fight(const BotView& view, BotBody& body, const SeenPlayer& enemy) = 0;
    /// Out of a fight, before looking for items and goals (a caster's buffs). True: done.
    virtual bool idle(const BotView& view, BotBody& body);
    /// What gear (weapons, armour, staffs) is worth to this class: by default, usable gear
    /// that beats what we carry of its kind (on its armour piece) by how much. Potions are the
    /// base's (worth).
    virtual float gearWorth(const BotView& view, const SeenItem& item) const;
    /// Poorly armed: gear before fights with anyone far off.
    virtual bool underGeared(const BotView& view) const;
    /// Puts on what's better (after the potions): by default the best weapon by value, then
    /// the armour piece that beats most what's worn there, each tried once.
    virtual void tendGear(const BotView& view, BotBody& body);
    /// Whether it casts with mana (drinks and values mana potions, seeks out obelisks when low).
    virtual bool usesMana() const;
    /// Before walking toward (x, y) (the path to it planned): a faster way there (a wizard's
    /// teleport). True: done this frame.
    virtual bool shortcut(const BotView& view, BotBody& body, float x, float y);
    /// It can drain mana from afar (a wizard's Drain Mana): any obelisk will do, from range.
    virtual bool drains(const BotView& view) const;
    /// On the way to mana source `s`, `d` away: true once there and taking its mana (by
    /// default: within an obelisk's 50 and in sight of it, standing).
    virtual bool recharge(const BotView& view, BotBody& body, const BotView::ManaSource& s, float d);
    /// Within wallStandOff of a magic wall at (wx, wy), facing it: a blow at it (by default a
    /// swing or a punch, as often as the weapon's blows come round; a caster blasts it).
    virtual void hitWall(const BotView& view, BotBody& body, float wx, float wy);
    /// How near a magic wall we break it from: at hand (kWallReach) for a blow; a caster with a
    /// blast to spare (Fireball, Fist of Vengeance, Meteor: they wear a wall down far faster than
    /// blows) from off, out of the blast's way.
    virtual float wallStandOff(const BotView& view) const;
    static constexpr float kWallReach = 34;

    // ---- shared helpers
    /// Thinks so far (one a game frame we send input for), for "the next think".
    std::uint32_t thinks() const noexcept { return thinks_; }
    /// A class tells the base it attacked (a blow, a cast, a shot): a fight that does neither
    /// this nor moving for a while is a stall, broken by going at them.
    void attacked(const BotView& view) { attackedAt_ = view.frame; }
    static float distance(float ax, float ay, float bx, float by);
    /// A clear line from us to (x, y): no wall (`sight`: only walls block it) and no solid
    /// object (`safe`: no hazard on it either).
    static bool clearBetween(const BotView& view, float x, float y, bool sight = false, bool safe = false);
    /// What a player in our place would see: on our screen and not behind a wall.
    static bool sees(const BotView& view, float x, float y);
    /// The cursor ahead of us along `direction`, far enough that the server runs.
    static void runCursor(const BotView& view, BotBody& body, std::uint8_t direction);
    static bool hazardAt(const BotView& view, float x, float y);
    static bool blockedAt(const BotView& view, float x, float y);
    static std::pair<float, float> unitOf(std::uint8_t direction);
    /// The usable weapon not yet tried whose value beats `wielded` by most (`twoHandedOnly`:
    /// only two-handers); nullptr if none.
    const OwnItem* weaponUpgrade(const BotView& view, float wielded, bool twoHandedOnly) const;
    /// The usable armour not yet tried that beats most what's worn on its piece (`noShields`:
    /// no shields); nullptr if none.
    const OwnItem* armorUpgrade(const BotView& view, bool noShields) const;
    /// Puts `item` on, and never tries it again (the server says no to what doesn't fit).
    void equipOnce(const BotView& view, BotBody& body, const OwnItem& item);
    /// Of 16 points round (cx, cy) at radius `r`, the one nearest us with no wall, hazard or void;
    /// nullopt if none.
    static std::optional<std::pair<float, float>> openOnRing(const BotView& view, float cx, float cy, float r);
    /// No wall, hazard or void every 12 units out from us along `direction`, up to `upTo`.
    static bool openRun(const BotView& view, std::uint8_t direction, float upTo);
    /// A way (from `toward` turned by `turn`, then the other side) with no wall, hazard or
    /// void for 72 units; nullopt if neither.
    static std::optional<std::uint8_t> safeWay(const BotView& view, std::uint8_t toward, int turn);

    /// Where something was and how fast it's going, from frame to frame.
    struct Track {
        float x = 0;
        float y = 0;
        std::uint32_t frame = 0;
        float vx = 0; ///< units a frame
        float vy = 0;
        bool measured = false; ///< a velocity has been seen
    };
    static void follow(std::map<std::uint16_t, Track>& tracks, std::uint16_t netCode, float x, float y, std::uint32_t frame);
    /// Where the enemy will be in `frames` frames, if they keep going.
    std::pair<float, float> lead(const SeenPlayer& enemy, float frames) const;
    /// A missile that will pass within `radius` of us within `frames` frames (by its tracked
    /// velocity); nullptr if none.
    const SeenMissile* incoming(const BotView& view, float frames, float radius) const;

    /// Going `direction`, with a hazard just ahead and safe ground within a jump: jump it, or,
    /// short of the stamina for that, stop at its edge until there is (true: stopped us).
    bool hop(const BotView& view, BotBody& body, std::uint8_t direction);
    /// Standing in a hazard: out of it by the nearest way. True when it moved us.
    bool escapeHazard(const BotView& view, BotBody& body);
    /// Where to stand to pick `item` up (safe ground within reach of it if it lies in a
    /// hazard); nullopt if there's none.
    std::optional<std::pair<float, float>> pickupSpot(const BotView& view, const SeenItem& item) const;
    void moveTo(const BotView& view, BotBody& body, float x, float y, float arrive = 12);
    /// No way on toward (gx, gy) (or stuck), and a magic wall that way within 250: to it and
    /// break it. True while doing that.
    bool breakMagicWall(const BotView& view, BotBody& body, float gx, float gy);
    /// No way there, or stuck, for giving a goal up: not while breaking a magic wall in the way
    /// (the way's closed only until it falls).
    bool noWay(const BotView& view) const { return nav_.unreachable() && view.frame - wallBreakAt_ > 30; }
    bool stuckFor(const BotView& view) const { return nav_.stuck() && view.frame - wallBreakAt_ > 30; }
    /// Toward the enemy along the paths (to `arrive`); straight at them if close and in sight
    /// but the grid has no way; given up on (ignored a while) if there's no way at all.
    void approach(const BotView& view, BotBody& body, const SeenPlayer& enemy, float arrive);
    std::uint8_t aimAt(const BotView& view, float x, float y);
    bool chance(int percent);

    bool isEnemy(const BotView& view, const SeenPlayer& p) const;
    /// Players seen fall are bodies (not targets) until they're elsewhere or moving again.
    void noteBodies(const BotView& view);
    const SeenPlayer* pickTarget(const BotView& view);
    /// How much an item is worth going for (0: not at all): potions while there's room (mana
    /// ones for a class that uses mana), gear by gearWorth; nothing out in lava.
    float worth(const BotView& view, const SeenItem& item) const;
    /// The item to go for, worth at least `atLeast` (keeps to the one it's after).
    const SeenItem* chooseItem(const BotView& view, float atLeast, float maxDistance = kItemReach);
    /// Items are gone for from this far...
    static constexpr float kItemReach = 450;
    /// ...but an attacker with its gear, off for their flag, goes only for what's on its way
    /// (a potion a little further when hurt).
    static constexpr float kMissionItemReach = 120;
    static constexpr float kMissionPotionReach = 250;
    void fetch(const BotView& view, BotBody& body, const SeenItem& item);
    /// Off to where the map lays out an item worth at least `atLeast` to us: one of the few
    /// best (so bots spread out), kept to until reached or seen empty. True while on the way.
    bool gearRun(const BotView& view, BotBody& body, float atLeast);
    /// The squad's part for us in a team game (BotView::role). True while doing it.
    bool teamGoal(const BotView& view, BotBody& body);
    /// Escorting: the teammate with their flag we're with, if they're in view and still have it.
    const SeenPlayer* escorted(const BotView& view) const;
    /// Our team's flag base (own) or the enemy's, from the map.
    std::optional<std::pair<float, float>> flagBase(const BotView& view, bool own) const;
    /// Our flag (own) or theirs lying away from its base, where we can see it; null if none.
    const BotView::FlagBase* looseFlag(const BotView& view, bool own) const;
    /// A flag dropped within kFlagDash (ours first): to it before a fight (whoever we fight
    /// nearer and at hand, or with our flag, excepted). True while going.
    bool flagDash(const BotView& view, BotBody& body, const SeenPlayer* enemy);
    /// Onto a dropped flag (ours: home it goes; theirs: it's ours to carry). False, and flags
    /// left alone for kFlagSkip, when there's no way onto it.
    bool toFlag(const BotView& view, BotBody& body, const BotView::FlagBase& flag);
    /// Carrying their flag: home with it; with ours gone from its base (seen from near it),
    /// held nearby off the base, where the enemies aren't, until ours is back; with our escort
    /// well behind and nobody about, a moment's wait for them. False if home isn't known.
    bool carryHome(const BotView& view, BotBody& body);
    /// The frame's play (think() notes the movement round it).
    void play(const BotView& view, BotBody& body);
    /// On a chat map (no game yet, the host to pick one): no fighting; standing about, now and
    /// then a few steps somewhere within kLoungeRadius of where we came in.
    void lounge(const BotView& view, BotBody& body);
    /// Out of play (standing down, dead, observing): still, and after nothing.
    void outOfPlay(BotBody& body);
    /// After the frame's play: standing with a shield (which comes up, and turns what comes at
    /// it), not blocking a missile, out of a fight, with a teammate within kBuffReach casting:
    /// a slow walk to them keeps the shield down so their buff gets in.
    void letBuffsIn(const BotView& view, BotBody& body);
    void tend(const BotView& view, BotBody& body);
    /// tendGear's armour (the piece that beats most what's worn there) and a quiver with a bow,
    /// each tried once. True if it equipped something.
    bool tendArmor(const BotView& view, BotBody& body);
    /// Low on mana (a class that uses it): off to the nearest obelisk with some, and there
    /// until full again or it's drained. True while doing that.
    bool seekMana(const BotView& view, BotBody& body);
    /// Nobody to fight: items, mana, gear runs, the team's goal, else roaming.
    void look(const BotView& view, BotBody& body);
    /// In a fight that has stalled (a target within 250, and for kFightStall neither a step nor
    /// an attack): at them, swinging, for a moment. True while doing that.
    bool unstall(const BotView& view, BotBody& body, const SeenPlayer& enemy);
    /// Running back and forth in one spot out of a fight for a while (a way the paths can't
    /// settle on): whatever it was going for is given up for a while, and it goes elsewhere.
    void watchForLoops(const BotView& view);

    /// Loop watch: a position sampled this often, over kLoopSamples of them (8 s).
    static constexpr std::uint32_t kLoopSampleEvery = 15;
    static constexpr std::size_t kLoopSamples = 16;
    /// Run at least this far in that time while staying within kLoopRadius: a loop.
    static constexpr float kLoopTravel = 220;
    static constexpr float kLoopRadius = 70;

    /// Below this part of max mana a caster goes for mana; it stays until this full.
    static constexpr float kLowMana = 0.3f;
    static constexpr float kFullMana = 0.95f;
    /// An obelisk gives a player within this a mana a frame (ObeliskUpdate: 2500 squared).
    static constexpr float kObeliskReach = 50;
    /// Obelisks within this of a drained one are its group (left with it unless they show charge).
    static constexpr float kObeliskGroup = 120;
    /// How far it looks for an obelisk with charge (the map, near enough).
    static constexpr float kManaSearch = 4000;
    /// At an obelisk, under kManaDryGain mana in each kManaDry (an obelisk gives a point a frame;
    /// a caster's own mana comes back a point every 1-2 s), counted kManaSettle after arriving:
    /// it's dry, on to another.
    static constexpr std::uint32_t kManaDry = 45;
    static constexpr std::uint16_t kManaDryGain = 5;
    static constexpr std::uint32_t kManaSettle = 20;
    /// Carrying the flag, only an enemy this near (in the way) is fought...
    static constexpr float kFlagFightReach = 70;
    /// ...or, hurt in the last kHurtMemory frames, one this near (who's hurting us).
    static constexpr float kFlagDefendReach = 250;
    static constexpr std::uint32_t kHurtMemory = 60;
    /// A guard fights anyone this near it, or this near its flag, or hurting it; others it
    /// leaves (back to the flag).
    static constexpr float kGuardReach = 350;
    static constexpr float kGuardBase = 500;
    /// Hurt with nobody in sight: the nearest enemy this near is who's doing it.
    static constexpr float kHurtSearch = 400;
    /// A dropped flag this near is gone for before a fight.
    static constexpr float kFlagDash = 300;
    /// A dropped flag there's no way onto is left this long (10 s).
    static constexpr std::uint32_t kFlagSkip = 300;
    /// A teammate casting this near may be buffing us: our shield's kept down (letBuffsIn).
    static constexpr float kBuffReach = 300;
    /// Carrying their flag, a health potion's drunk below this part of max health (sooner).
    static constexpr float kCarrierDrinks = 0.6f;
    /// A carrier waiting for ours to come back holds this far from its base.
    static constexpr float kHoldRadius = 150;
    /// An escort runs this far ahead of the carrier, homeward...
    static constexpr float kEscortLead = 60;
    /// ...and fights only enemies this near them (or at hand, or hurting it).
    static constexpr float kEscortGuard = 250;
    /// A creature is chosen as a target as if this much further off than it is.
    static constexpr float kCreatureHandicap = 80;

    // ---- state
    Skill skill_;          ///< as it plays this frame (eased off, fighting someone struggling)
    Skill baseSkill_;      ///< as it plays
    bool easing_ = false;  ///< going easy on this frame's target
    std::mt19937 rng_;
    Navigator nav_;
    float reach_ = 32;                 ///< this frame's fighting distance (prepare sets it)
    std::uint16_t target_ = 0;
    std::optional<SeenPlayer> lastSeen_; ///< the target where we last saw them
    std::uint32_t lastSeenAt_ = 0;
    std::uint16_t noticed_ = 0;        ///< an enemy seen but not yet reacted to
    std::uint32_t noticedAt_ = 0;
    std::uint32_t reachedAt_ = 0;      ///< last frame the target was within reach (or was picked)
    std::uint16_t lastHealth_ = 0;     ///< health last frame: less now means we're being hit
    std::uint16_t ignored_ = 0;        ///< an enemy there's no way to, left alone until ignoredUntil_
    std::uint32_t ignoredUntil_ = 0;
    std::uint32_t respawnAt_ = 0;
    std::uint32_t observeAskedAt_ = 0; ///< standing down: when we last asked to observe
    std::uint32_t nextItemAction_ = 0; ///< potions and equipping, at most so often
    std::set<std::uint16_t> triedEquip_;
    std::uint16_t fetching_ = 0;       ///< an item it's going for
    std::uint32_t fetchSince_ = 0;
    int pickupTries_ = 0;
    std::map<std::uint16_t, std::uint32_t> skipItems_; ///< items it couldn't get to, until when
    std::optional<std::size_t> goalSpot_;              ///< the map item spot it's running to
    std::uint32_t goalSince_ = 0;
    std::map<std::size_t, std::uint32_t> checkedSpots_; ///< spots found empty, until when
    static constexpr std::uint32_t kSpotRecheck = 900;  ///< 30 s: an arena item's respawn
    static constexpr std::uint32_t kGoalTimeout = 900;  ///< a run given up after this long
    std::optional<std::pair<float, float>> roam_;
    std::uint32_t roamUntil_ = 0;
    std::map<std::uint16_t, Track> players_;  ///< the others' movement
    std::map<std::uint16_t, Track> missiles_; ///< missiles' movement
    std::uint32_t nextJump_ = 0;              ///< no jumping before this frame
    std::optional<std::size_t> manaGoal_;     ///< the BotView::manaSources one it's going to
    std::uint32_t manaSince_ = 0;
    std::optional<std::uint32_t> manaArrivedAt_;
    std::uint32_t manaWindowAt_ = 0;          ///< at a source: the current measure of what's coming began...
    std::uint16_t manaWindowFrom_ = 0;        ///< ...with this much mana
    std::uint16_t lastMana_ = 0;
    std::uint32_t manaGainAt_ = 0;            ///< the last frame our mana went up
    std::map<std::size_t, std::uint32_t> skipSources_; ///< sources found empty or out of reach, until when
    std::vector<std::pair<float, float>> loopSamples_; ///< where we were, every kLoopSampleEvery frames
    std::map<std::uint16_t, std::pair<float, float>> bodies_; ///< the fallen, where they lie
    std::uint32_t wallBlowAt_ = 0;            ///< our last blow at a magic wall
    std::uint32_t wallBreakAt_ = 0;           ///< the last frame we were at breaking one
    std::uint32_t hurtAt_ = 0;                ///< the last frame our health went down
    std::optional<std::pair<float, float>> patrol_; ///< a guard's next spot by the flag
    std::uint32_t patrolPauseUntil_ = 0;            ///< pausing at the last one until then
    static constexpr float kPatrolRadius = 160;     ///< a guard's beat: this far round its flag
    std::uint32_t loopSampledAt_ = 0;
    std::uint32_t thinks_ = 0;
    std::uint32_t attackedAt_ = 0;            ///< our last blow, cast or shot
    std::pair<float, float> fightAnchor_{};   ///< where we stood when a fight last saw us move
    std::uint32_t fightAnchorAt_ = 0;
    std::uint32_t unstickUntil_ = 0;          ///< breaking a fight stall: at them until this frame
    static constexpr std::uint32_t kFightStall = 90; ///< 3 s neither moving nor attacking, a target near
    std::uint32_t teamGoalPausedUntil_ = 0;            ///< the team's goal left for roaming until then (it looped)
    const char* doing_ = "";                           ///< doing()
    bool moving_ = false;                              ///< our held movement is on (as last set)
    std::optional<std::pair<float, float>> loungeHome_; ///< a chat map: where we came in
    std::optional<std::pair<float, float>> loungeSpot_; ///< ...and the spot we're strolling to
    std::uint32_t loungeUntil_ = 0;                    ///< ...or standing about until
    bool blocked_ = false;                             ///< this frame we turned to block a missile
    std::uint32_t frame_ = 0;                          ///< the last think's frame
    std::uint32_t flagSkipUntil_ = 0;                  ///< dropped flags left alone until then (no way onto one)
    std::optional<std::pair<float, float>> holdSpot_;  ///< a carrier's spot by home while ours is away
    std::uint32_t holdChosenAt_ = 0;
    std::uint32_t escortWaitFrom_ = 0;                 ///< a carrier waiting for its escort since (0: not)
    std::uint32_t noEscortWaitUntil_ = 0;

public:
    /// For tests and logs: how many times it found itself in a loop and broke out.
    int loopsBroken() const noexcept { return loopsBroken_; }
    /// For tests and logs: how many fight stalls it broke.
    int stallsBroken() const noexcept { return stallsBroken_; }

private:
    int loopsBroken_ = 0;
    int stallsBroken_ = 0;
};

} // namespace nox::bot
