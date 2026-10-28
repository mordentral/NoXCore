// The warrior's brain: what a warrior does differently from every player (PlayerBrain).
//
//   - fights in melee: closes in and swings (Action) once within its weapon's reach, facing
//     them with the skill's aim error, only with the stamina the swing takes; between blows
//     rounds them at the edge of reach; steps out of a blow or a charge it sees coming;
//   - its abilities: Berserker Charge down a clear line (led), the Harpoon to drag them in, War
//     Cry when a caster is near (it stops their spells within 300: warriorWarcry);
//   - turns a shield or a greatsword to a missile coming at it;
//   - keeps a GreatSword or WarHammer in hand once it has one (they take both hands), and goes
//     for them first.
// Ability ranges come from gamedata.bin (MaxHarpoonFlightDistance: BotView::numbers) and the exe; the
// readiness from the server (MSG_REPORT_ABILITY_STATE), so cooldowns needn't be tracked.
#pragma once

#include "noxcore/bot/player_brain.hpp"

namespace nox::bot {

class WarriorBrain : public PlayerBrain {
public:
    WarriorBrain(Skill skill, std::uint32_t seed);

    /// Bare-handed reach, centre to centre: 20 past the wielder's radius and both
    /// radii. A weapon's comes from modifier.bin's RANGE (ItemInfo::reach: a Longsword 65).
    static constexpr float kBareReach = 40;
    /// We close to this much inside our reach: they move, and our input takes a while to land.
    static constexpr float kReachMargin = 8;
    /// How fast a Berserker Charge goes (units a frame), for leading the target: an estimate.
    /// The charge pushes 6 x the player's base speed along the facing each frame
    /// (playerAttack), which friction holds to some speed.
    static constexpr float kChargeSpeed = 20;
    /// Frames from pressing Action until our blow has landed: we stand facing the target that
    /// long (an estimate: the strike animations run about half a second).
    static constexpr std::uint32_t kSwingHold = 12;

protected:
    void prepare(const BotView& view) override;
    bool react(const BotView& view, BotBody& body) override;
    void fight(const BotView& view, BotBody& body, const SeenPlayer& enemy) override;
    bool underGeared(const BotView& view) const override;
    void tendGear(const BotView& view, BotBody& body) override;

private:
    /// A missile coming at us while we hold a shield or greatsword: stop and face it so it's
    /// turned (server_handler_PlayerDamage). True while blocking.
    bool block(const BotView& view, BotBody& body);
    float weaponValue(const BotView& view) const; ///< the wielded weapon's value, 0 bare-handed
    /// The stamina a swing of what's in hand takes (0 bare-handed).
    std::uint8_t swingCost(const BotView& view) const;
    bool useAbilities(const BotView& view, BotBody& body, const SeenPlayer& enemy, float distance, bool clear);
    /// Close quarters: a blow when the weapon's ready and they're in reach, else round them at
    /// the edge of reach.
    void melee(const BotView& view, BotBody& body, const SeenPlayer& enemy, float d);

    std::uint32_t chargeAimAt_ = 0; ///< the think it turned to charge (the charge goes where it faces)
    std::uint32_t abilityAt_ = 0;   ///< the last ability, so they're spaced out
    std::uint32_t abilityCheckAt_ = 0; ///< the last look at whether one fits
    std::uint32_t swingAt_ = 0;     ///< our last blow's frame
    /// Turned to strike on this think (thinks()); the blow goes the next think. Counted in
    /// thinks, not server frames: those come in jumps as big as the server's update interval.
    std::uint32_t strikeAimAt_ = 0;
    std::uint8_t strikeFacing_ = 0;
    int circleSide_ = 1;            ///< which way round the enemy
    std::uint32_t circleFlipAt_ = 0;
    std::uint32_t dodgeUntil_ = 0;  ///< stepping out of a blow until this frame
    std::uint8_t dodgeDirection_ = 0;
    bool enemyWasAttacking_ = false; ///< (a blow is dodged once, when it starts)
    std::uint32_t blockUntil_ = 0;   ///< blocking a missile until this frame
    std::uint16_t blocking_ = 0;     ///< the missile it's turned to
};

} // namespace nox::bot
