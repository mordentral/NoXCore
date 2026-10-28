// Bots in a game we host: no connection, no join. The host's server puts each bot's player in
// the game, shows it the game as that player sees it and takes its input directly, so a bot
// costs a brain and a player, not a client and a socket. Until noxcore runs the server,
// EmulatedHost (emulated_host.hpp) implements this.
//
// BotRoster keeps the count: up to a configured number of bots, one fewer for each person
// playing (each person who joins takes a bot's place), within the game's player limit.
#pragma once

#include "noxcore/bot/bot.hpp"
#include "noxcore/bot/personas.hpp"
#include "noxcore/client/headless_client.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace nox::bot {

/// What a hosting server offers its bots.
class BotHost {
public:
    virtual ~BotHost() = default;
    /// Puts a bot's player in the game as if it had joined; its net code, or nullopt when full.
    virtual std::optional<std::uint16_t> addBot(const client::PlayerProfile& profile) = 0;
    /// Takes it out again, as a player leaving.
    virtual void removeBot(std::uint16_t netCode) = 0;
    /// The game as that player sees it now.
    virtual BotView view(std::uint16_t netCode) const = 0;
    /// That player's input, applied on the host's next frame.
    virtual BotBody& body(std::uint16_t netCode) = 0;
    /// People in the game (bots not counted).
    virtual std::size_t humans() const = 0;
    virtual std::size_t playerLimit() const = 0;
    /// Every player's name, people's and bots'.
    virtual std::set<std::string> names() const = 0;
};

/// A brain for a class (0 warrior, 1 wizard, 2 conjurer: WarriorBrain, WizardBrain,
/// ConjurerBrain, all PlayerBrains), nullptr for any other. A caster fights in `style` (a
/// persona's; wizards don't summon).
std::unique_ptr<Brain> makeBrain(std::uint8_t playerClass, Skill skill, std::uint32_t seed, Style style = Style::Balanced);
/// How well a persona of `tier` plays.
Skill skillFor(Tier tier);

class BotRoster {
public:
    struct Bot {
        std::uint16_t netCode = 0;
        std::string name;
        std::unique_ptr<Brain> brain;
    };

    BotRoster(BotHost& host, std::uint32_t seed);
    ~BotRoster();

    /// How many bots to keep at most (with nobody else playing).
    void setTarget(std::size_t count) { target_ = count; }
    std::size_t target() const noexcept { return target_; }
    void setSkill(Skill skill) { skill_ = skill; }

    /// Once a game frame, before the host's: a bot in or out when the count is off (one at a
    /// time, `kChangeFrames` apart, so a crowd joining doesn't churn), then every bot thinks.
    void frame(std::uint32_t frame);
    /// Takes every bot out (the game ending, hosting stopping).
    void clear();

    const std::vector<Bot>& bots() const noexcept { return bots_; }

    /// The bots wanted: `target` less one per person, never past the player limit.
    static std::size_t wanted(std::size_t target, std::size_t humans, std::size_t limit);
    static constexpr std::uint32_t kChangeFrames = 30;

private:
    BotHost& host_;
    std::mt19937 rng_;
    Skill skill_;
    std::size_t target_ = 0;
    std::vector<Bot> bots_;
    std::uint32_t changedAt_ = 0;
    bool changed_ = false;
};

} // namespace nox::bot
