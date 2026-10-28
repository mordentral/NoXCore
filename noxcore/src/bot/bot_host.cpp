// Hosted bots (bot_host.hpp): the brain for each class, the skill for each tier, and the roster
// that keeps a game we host filled with bots up to a target.
#include "noxcore/bot/bot_host.hpp"

#include "noxcore/bot/caster_brain.hpp"
#include "noxcore/bot/warrior_brain.hpp"

#include <algorithm>

namespace nox::bot {

std::unique_ptr<Brain> makeBrain(std::uint8_t playerClass, Skill skill, std::uint32_t seed, Style style)
{
    switch (playerClass) {
    case 0:
        return std::make_unique<WarriorBrain>(skill, seed);
    case 1: {
        auto brain = std::make_unique<WizardBrain>(skill, seed);
        brain->setStyle(style == Style::Summoner ? Style::Balanced : style); // (wizards don't summon)
        return brain;
    }
    case 2: {
        auto brain = std::make_unique<ConjurerBrain>(skill, seed);
        brain->setStyle(style);
        return brain;
    }
    default:
        return nullptr;
    }
}

Skill skillFor(Tier tier)
{
    switch (tier) {
    case Tier::Easy: return Skill::easy();
    case Tier::Medium: return Skill::normal();
    case Tier::Hard: return Skill::hard();
    }
    return Skill::normal();
}

BotRoster::BotRoster(BotHost& host, std::uint32_t seed)
    : host_(host)
    , rng_(seed)
{
}

BotRoster::~BotRoster()
{
    clear();
}

std::size_t BotRoster::wanted(std::size_t target, std::size_t humans, std::size_t limit)
{
    // Bots fill up to the target with the people, never past the player limit.
    const std::size_t room = limit > humans ? limit - humans : 0;
    const std::size_t fill = target > humans ? target - humans : 0;
    return std::min(fill, room);
}

void BotRoster::frame(std::uint32_t frame)
{
    const std::size_t want = wanted(target_, host_.humans(), host_.playerLimit());
    // At most one bot in or out each kChangeFrames.
    if (!changed_ || frame - changedAt_ >= kChangeFrames) {
        if (bots_.size() < want) {
            client::PlayerProfile profile;
            profile.name = botName(rng_, host_.names());
            profile.playerClass = 0; // warriors first
            const auto seed = rng_();
            if (const auto netCode = host_.addBot(profile)) {
                bots_.push_back({*netCode, profile.name, makeBrain(profile.playerClass, skill_, seed)});
            }
            changedAt_ = frame;
            changed_ = true;
        } else if (bots_.size() > want) {
            // The newest goes first: the ones playing longest keep their scores.
            host_.removeBot(bots_.back().netCode);
            bots_.pop_back();
            changedAt_ = frame;
            changed_ = true;
        }
    }
    for (auto& bot : bots_) {
        if (bot.brain) {
            bot.brain->think(host_.view(bot.netCode), host_.body(bot.netCode));
        }
    }
}

void BotRoster::clear()
{
    for (const auto& bot : bots_) {
        host_.removeBot(bot.netCode);
    }
    bots_.clear();
}

} // namespace nox::bot
