// The casters' brains: what a wizard and a conjurer do differently from every player
// (PlayerBrain), and from each other.
//
// CasterBrain, what both do:
//   - fight at range: keep between kNearest and kFarthest of the enemy (backing off, closing
//     in, else stepping round them), casting an attack the class picks every so often, at
//     where they'll be, when they're in sight and there's mana for it (the spells we've been
//     given: BotView::spells);
//   - with a bow or crossbow and something in the quiver, shoot it between spells, standing and
//     facing where they'll be (the shot flies the way we face). A crossbow shoots as the
//     attack starts and then reloads; a taunt right after the shot ends the attack, as players
//     do to cut the reload short (when there are bolts left to shoot);
//   - step aside from a missile coming at them;
//   - a heal or an escape when hurt, buffs on themselves out of a fight;
//   - drink mana potions and go for them; low on mana, off to an obelisk (PlayerBrain::seekMana);
//   - out of spells and shots: a weapon if they have one, else away (to mana, if there's some).
// WizardBrain and ConjurerBrain choose the spells. A persona's Style tunes it: Protective keeps
// further off and heals and escapes sooner, Combative closes in, casts more often and curses
// more, a Summoner conjurer calls creatures to fight beside it.
//
// The spells are on a bar of three pages of five (Attack, Control, Defence), as a player sets
// up the game's bar: what a caster casts comes from the page it's on. In a fight it turns to
// the page the moment calls for (hurt: Defence; a thief running off with our flag, a warrior
// closing in, a fresh foe now and then: Control; else Attack), out of one it keeps Defence (its
// buffs, a wizard's teleports); a turn takes kPageTurn, as a player's key does. (Summoning is
// apart, as the game's creature panel is.)
#pragma once

#include "noxcore/bot/personas.hpp"
#include "noxcore/bot/player_brain.hpp"

#include <algorithm>
#include <array>

namespace nox::bot {

/// Spell ids (things::spellId's numbering) the casters use.
namespace Spell {
inline constexpr std::uint32_t Blink = 4; ///< to a random waypoint or player start (spellBlink1)
inline constexpr std::uint32_t Burn = 5;  ///< flames where it's cast
inline constexpr std::uint32_t Charm = 9; ///< SPELL_CHARM: a creature made ours
inline constexpr std::uint32_t Confuse = 12;
inline constexpr std::uint32_t CurePoison = 14;
inline constexpr std::uint32_t DeathRay = 16;
inline constexpr std::uint32_t DrainMana = 22;
inline constexpr std::uint32_t Fireball = 27;
inline constexpr std::uint32_t Fist = 29;
inline constexpr std::uint32_t ForceOfNature = 31;
inline constexpr std::uint32_t Fumble = 33; ///< they drop what's in their hands
inline constexpr std::uint32_t Glyph = 34;  ///< a trap: the bots never lay them
inline constexpr std::uint32_t Haste = 36;
inline constexpr std::uint32_t Infravision = 37;
inline constexpr std::uint32_t LesserHeal = 41;
inline constexpr std::uint32_t MagicMissile = 50;
inline constexpr std::uint32_t Shield = 51; ///< SPELL_SHIELD: the Force Field
inline constexpr std::uint32_t Meteor = 52;
inline constexpr std::uint32_t PixieSwarm = 58;
inline constexpr std::uint32_t Poison = 60;
inline constexpr std::uint32_t Pull = 65; ///< them toward us
inline constexpr std::uint32_t Push = 66; ///< them away from us
inline constexpr std::uint32_t Shock = 71;
inline constexpr std::uint32_t Slow = 72;
inline constexpr std::uint32_t Stun = 74;
inline constexpr std::uint32_t TeleportToTarget = 126; ///< to the cursor (castTTT)
inline constexpr std::uint32_t ToxicCloud = 128;
inline constexpr std::uint32_t Vampirism = 130; ///< our blows heal us
inline constexpr std::uint32_t Wall = 132;      ///< never cast by a bot (people find it a nuisance)
} // namespace Spell

/// A caster's bar pages (CasterBrain).
enum class Page : std::uint8_t { Attack, Control, Defence };
using SpellPages = std::array<std::array<std::uint32_t, 5>, 3>; ///< by Page; 0 an empty slot

class CasterBrain : public PlayerBrain {
public:
    /// The range a caster fights at: no nearer than this...
    static constexpr float kNearest = 110;
    /// ...and no further (its spells' reach is about a screen).
    static constexpr float kFarthest = 250;
    /// Frames between casts (the chant takes about this long).
    static constexpr std::uint32_t kCastEvery = 24;
    /// How far it shoots a bow or crossbow from.
    static constexpr float kShootRange = 300;
    /// Units a frame: thing.bin's SPEED / 32 (ArcherBolt 1000, ArcherArrow 800).
    static constexpr float kBoltSpeed = 1000.0f / 32;
    static constexpr float kArrowSpeed = 800.0f / 32;
    /// A staff's shot, about a fireball's speed (an estimate).
    static constexpr float kStaffShotSpeed = 16;
    /// After a crossbow shot cut short by a taunt, this long on the move (the reload's time)
    /// before the next.
    static constexpr std::uint32_t kMoveAfterShot = 12;
    /// A summoner's creature, at most this often (15 s), while there's room in its cage...
    static constexpr std::uint32_t kSummonEvery = 450;
    /// ...and this much mana left over after the creature's price.
    static constexpr std::uint16_t kSummonReserve = 30;
    /// Summonings never called: the Bomber's (a trap's) and the green wizard's.
    static constexpr std::uint32_t kSummonBomber = 0x4F;
    static constexpr std::uint32_t kSummonWizard = 0x71;

    /// A page turn takes this long before a spell of the new page can be cast (a key's press).
    static constexpr std::uint32_t kPageTurn = 6;
    /// In a fight, the page is chosen again at most this often (an emergency aside), so it isn't
    /// flicked back and forth.
    static constexpr std::uint32_t kPageHold = 30;

    /// How this caster fights (its persona's): see Style.
    void setStyle(Style style) noexcept { style_ = style; }
    Style style() const noexcept { return style_; }
    /// For tests and logs: the bar page in use.
    Page page() const noexcept { return page_; }
    /// For logs: the last spell cast (0 none yet), and how often each has been.
    std::uint32_t lastSpell() const noexcept { return lastSpell_; }
    const std::map<std::uint32_t, int>& castCounts() const noexcept { return castCounts_; }
    /// The class's bar.
    virtual const SpellPages& pages() const = 0;

protected:
    CasterBrain(Skill skill, std::uint32_t seed);

    // ---- the style's measures (Balanced: the k... constants)
    /// The range it fights at: Protective further off, Combative closer.
    float nearest() const noexcept
    {
        return style_ == Style::Protective ? 140.0f : style_ == Style::Combative ? 90.0f : kNearest;
    }
    float farthest() const noexcept
    {
        return std::min(300.0f, style_ == Style::Protective ? 270.0f : style_ == Style::Combative ? 220.0f : kFarthest); // (a screen: spells reach no further)
    }
    /// Frames between casts: Combative casts more often.
    std::uint32_t castEvery() const noexcept { return style_ == Style::Combative ? 18u : kCastEvery; }
    /// A curse's (Slow, Confuse, Stun...) `percent` chance: twice Combative, half Protective.
    int curse(int percent) const noexcept
    {
        return style_ == Style::Combative ? percent * 2 : style_ == Style::Protective ? percent / 2 : percent;
    }
    /// The part of health a heal or an escape comes below: sooner Protective, later Combative.
    float hurtLine(float part) const noexcept
    {
        const float styled = style_ == Style::Protective ? part + 0.15f : style_ == Style::Combative ? part - 0.05f : part;
        return std::clamp(styled, 0.1f, 0.95f);
    }
    /// A Summoner's creature, placed a step toward (tx, ty): any it knows that fits the cage's
    /// free cells and leaves kSummonReserve mana, the stronger a little likelier, the last few
    /// called much less so; at most every kSummonEvery. True if one was called.
    bool summon(const BotView& view, BotBody& body, float tx, float ty);

    /// A spell and where it goes: at a point, or (none) on ourselves.
    struct Cast {
        std::uint32_t spell = 0;
        std::optional<std::pair<float, float>> at;
    };

    // ---- what a class picks
    /// The Attack page's spell for `enemy`, `d` away (one we can cast now), or nullopt (then a
    /// shot, if we have a loaded bow).
    virtual std::optional<std::uint32_t> attack(const BotView& view, const SeenPlayer& enemy, float d) = 0;
    /// The Control page's spell that would do something to `enemy`, `d` away, now (one we have
    /// and can pay for, whatever page we're on: it decides whether to turn to it), or nullopt.
    virtual std::optional<std::uint32_t> control(const BotView& view, const SeenPlayer& enemy, float d) = 0;
    /// A spell now to save ourselves: a heal when hurt, an escape when close and hurt (nullopt:
    /// none). `enemy` is null out of a fight.
    virtual std::optional<Cast> rescue(const BotView& view, const SeenPlayer* enemy);
    /// A buff on ourselves out of a fight (nullopt: none wanted now).
    virtual std::optional<std::uint32_t> buff(const BotView& view);

    /// A debuff (Slow, Confuse, Stun, Poison) on `enemy` that would do something: not one they
    /// already have, nor one we put on them in the last kDebuffAgain frames (it may not show).
    /// Any other spell: true.
    bool worthOn(const BotView& view, const SeenPlayer& enemy, std::uint32_t spell) const;
    static constexpr std::uint32_t kDebuffAgain = 150; ///< 5 s

    /// We have `spell` (whatever the mana).
    static bool knows(const BotView& view, std::uint32_t spell);
    /// We have `spell` and the mana for it (wherever it is on the bar).
    static bool has(const BotView& view, std::uint32_t spell);
    /// We have `spell`, the mana for it, and it's on the page in use, turned to a moment ago
    /// (kPageTurn): it can be cast now.
    bool can(const BotView& view, std::uint32_t spell) const;
    /// How near where `spell` lands we'd be caught by it too (a blast, flames, a cloud; 0 for
    /// one that hits only its target): it isn't cast at a point that near us.
    static float splash(std::uint32_t spell);
    /// How long after it's cast where `spell` lands stays dangerous (a fist still to fall, a
    /// cloud lingering), 0 for none: the caster keeps out of it meanwhile.
    static std::uint32_t blastFrames(std::uint32_t spell);
    /// `spell` is on `page` of the bar.
    bool onPage(std::uint32_t spell, Page page) const;
    /// To `page` (if not on it); true once it's ready to cast from (kPageTurn after the turn).
    bool turnTo(const BotView& view, Page page);
    /// The page a fight with `enemy`, `d` away, calls for now: Defence when hurt with a way out
    /// there, Control on a thief with our flag, a warrior closing in, or (now and then) a foe
    /// that doesn't have its curses yet, else Attack. A page with nothing we can afford for
    /// them isn't chosen.
    Page choosePage(const BotView& view, const SeenPlayer& enemy, float d);
    /// Below this part of max health (the style's hurtLine of it) the fight turns to Defence.
    virtual float rescueAt() const = 0;
    /// The bow, crossbow or shooting staff in hand, if any.
    static const OwnItem* launcher(const BotView& view);
    /// One in hand and something to shoot from it (bolts or arrows, a staff's charges).
    static bool loaded(const BotView& view);

    void prepare(const BotView& view) override;
    /// The weapon in hand: a bow or crossbow with something to shoot first, else the best
    /// staff with charges, else a weapon to strike with; then armour.
    void tendGear(const BotView& view, BotBody& body) override;
    void fight(const BotView& view, BotBody& body, const SeenPlayer& enemy) override;
    bool idle(const BotView& view, BotBody& body) override;
    bool usesMana() const override;

    /// Casts `spell` at (x, y) (on ourselves if `self`). True if cast.
    bool cast(const BotView& view, BotBody& body, std::uint32_t spell, float x, float y, bool self = false);
    bool cast(const BotView& view, BotBody& body, const Cast& c);
    void hitWall(const BotView& view, BotBody& body, float wx, float wy) override;
    float wallStandOff(const BotView& view) const override;
    /// The blasts a caster breaks a magic wall with (far faster than blows), in that order...
    static constexpr std::array<std::uint32_t, 3> kWallBlasts{Spell::Fireball, Spell::Fist, Spell::Meteor};
    /// ...from this far (out of their splash).
    static constexpr float kWallBlastRange = 110;

private:
    /// Shoots the bow or crossbow at where they'll be (standing, facing it).
    void shoot(const BotView& view, BotBody& body, const SeenPlayer& enemy, float d);
    /// Without spells or shots: a weapon close, else keep away.
    void armed(const BotView& view, BotBody& body, const SeenPlayer& enemy, float d);

    std::uint32_t castAt_ = 0;     ///< our last cast's frame
    std::uint32_t dodgeUntil_ = 0; ///< stepping out of a missile's way until this frame
    std::uint8_t dodgeDirection_ = 0;
    int circleSide_ = 1;
    std::uint32_t circleFlipAt_ = 0;
    std::uint32_t swingAt_ = 0;
    std::optional<std::uint8_t> bolts_; ///< the equipped quiver's count, as last reported
    bool justShot_ = false;             ///< it went down this frame: a shot went
    std::optional<std::pair<float, float>> teleportedFrom_; ///< a teleport cast from here (the path's planned again once it takes)
    std::uint32_t moveAfterShotUntil_ = 0; ///< on the move after a taunt-cut shot until this frame
    std::map<std::uint32_t, std::uint32_t> buffedAt_; ///< each buff spell's last cast
    std::map<std::pair<std::uint16_t, std::uint32_t>, std::uint32_t> debuffedAt_; ///< (enemy, spell): last cast
    static constexpr std::uint32_t kBuffAgain = 300;  ///< 10 s before the same buff again
    std::uint32_t nextSwap_ = 0;           ///< no weapon change before this frame
    Style style_ = Style::Balanced;
    std::uint32_t summonAt_ = 0;           ///< our last creature called
    std::vector<std::uint32_t> recentSummons_; ///< the last few summonings, oldest first
    Page page_ = Page::Defence;            ///< the bar page in use
    std::uint32_t pageAt_ = 0;             ///< when it was turned to
    std::uint32_t pageChosenAt_ = 0;       ///< the fight's last choice of page
    std::uint32_t lastSpell_ = 0;
    std::map<std::uint32_t, int> castCounts_;
    /// Our last blast still to come down (or lingering) where it was cast, until when.
    struct Blast {
        float x = 0;
        float y = 0;
        float radius = 0;
        std::uint32_t until = 0;
    };
    std::optional<Blast> blast_;
};

/// A wizard. Attack: Magic Missiles and Fireballs at range, Death Ray when it has the mana, Burn
/// under one who's held, slowed or stunned, Shock up close. Control: Slow and Confuse, Fumble
/// on an armed warrior, Push on one at hand, Pull on a thief running off with our flag.
/// Defence: when hurt and pressed, Teleport to Target away from them (else Blink); Force Field
/// and Haste out of a fight; Drain Mana. Teleports along long ways when it has mana to spare, but
/// never at an enemy: not on the way to one, nor to land near one (by design: Teleport to
/// Target is for getting away and getting about, not for jumping on people).
/// Low on mana it drains it from an obelisk, from as far as the spell reaches, standing (the
/// spell ends if it moves). It never lays traps, nor casts Wall.
class WizardBrain : public CasterBrain {
public:
    WizardBrain(Skill skill, std::uint32_t seed);
    const SpellPages& pages() const override;
    static const SpellPages kPages;

    /// A teleport along the way goes at most this far, and only when the goal is further.
    static constexpr float kTeleportReach = 300;
    /// A teleport over what's between (a river, a window) when it cuts this much walking.
    static constexpr float kTeleportSaves = 200;
    /// A teleport along the way never lands nearer an enemy in sight (player or creature) than
    /// this, and none is cast on the way to one (a goal this near one).
    static constexpr float kTeleportKeepOff = 250;
    static constexpr float kTeleportNotAt = 80;

protected:
    std::optional<std::uint32_t> attack(const BotView& view, const SeenPlayer& enemy, float d) override;
    std::optional<std::uint32_t> control(const BotView& view, const SeenPlayer& enemy, float d) override;
    std::optional<Cast> rescue(const BotView& view, const SeenPlayer* enemy) override;
    std::optional<std::uint32_t> buff(const BotView& view) override;
    float rescueAt() const override { return 0.35f; }
    bool shortcut(const BotView& view, BotBody& body, float x, float y) override;
    bool drains(const BotView& view) const override;
    bool recharge(const BotView& view, BotBody& body, const BotView::ManaSource& s, float d) override;

private:
    /// The nearest enemy in sight (player or creature) to (x, y), how far; a big number if none.
    float nearestEnemy(const BotView& view, float x, float y) const;
    /// Somewhere on screen, in sight and safe, well away from `enemy`; nullopt if nowhere.
    std::optional<std::pair<float, float>> awayFrom(const BotView& view, const SeenPlayer& enemy) const;

    std::uint32_t nextTeleport_ = 0;
    std::uint32_t teleportLookAt_ = 0;
    std::uint32_t drainAt_ = 0;
};

/// A conjurer: a crossbow (or bow) when it has one with bolts, between spells. Attack: Fist of
/// Vengeance, Meteors, Pixie Swarms, Burn or a Toxic Cloud on one who can't walk out of it (no
/// Force of Nature: its ball bounces back off the walls onto its caster). Control: Stun, Slow and Poison, Charm on an enemy's creature. Defence: Lesser Heal when
/// hurt, Cure Poison when poisoned, Vampirism and Infravision (sees the invisible) out of a
/// fight. Goes for a crossbow and bolts when it has none. A Summoner calls creatures too.
class ConjurerBrain : public CasterBrain {
public:
    ConjurerBrain(Skill skill, std::uint32_t seed);
    const SpellPages& pages() const override;
    static const SpellPages kPages;

protected:
    std::optional<std::uint32_t> attack(const BotView& view, const SeenPlayer& enemy, float d) override;
    std::optional<std::uint32_t> control(const BotView& view, const SeenPlayer& enemy, float d) override;
    std::optional<Cast> rescue(const BotView& view, const SeenPlayer* enemy) override;
    std::optional<std::uint32_t> buff(const BotView& view) override;
    float rescueAt() const override { return 0.5f; }
    bool underGeared(const BotView& view) const override;
};

} // namespace nox::bot
