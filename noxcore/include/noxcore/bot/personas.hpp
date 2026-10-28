// The bots' roster: 75 people of the Land of Nox, 25 of each class (5 easy, 10 medium, 10 hard),
// each always of their class, with a life before the war, a temper and (casters) a way of
// fighting. The hardest carry the names of the campaign's great ones. A squad picks a free one
// of the class asked for within the difficulty range asked for; the same person comes back
// under the same name, the same class and the same ways.
#pragma once

#include <cstdint>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>

namespace nox::bot {

enum class Tier : std::uint8_t { Easy, Medium, Hard };

/// How a person carries themselves (how they talk, and how they take what's done to them).
enum class Temper : std::uint8_t { Friendly, Cheerful, Calm, Proud, Gruff, Mean };

/// How a caster fights: Balanced; Protective (shields and heals early, keeps further off);
/// Combative (closer, casts more often, more curses); Summoner (conjurers: creatures to fight).
enum class Style : std::uint8_t { Balanced, Protective, Combative, Summoner };

struct Persona {
    std::string_view name;       ///< without "[BOT] " (at most 18 characters: the game's 24)
    std::uint8_t playerClass = 0; ///< 0 warrior, 1 wizard, 2 conjurer
    Tier tier = Tier::Medium;
    Temper temper = Temper::Calm;
    Style style = Style::Balanced;
    std::string_view before;  ///< their life before the war, in their world's words
    std::string_view history; ///< a line of who they are now
};

/// All 75.
std::span<const Persona> personas();
/// One of the legendary eight (the campaign's own: Jandor, Horrendous, Gearhart, Jack Mower,
/// Horvath, Hecubah, Aldwyn, Theogrin).
bool isLegendary(const Persona& p);
/// The name it plays under: "[BOT] " and its name, and a legendary one marked with an asterisk
/// ("[BOT] Jandor*") so people can tell.
std::string playerNameOf(const Persona& p);
/// The persona named so (with or without "[BOT] "), if any.
const Persona* personaNamed(std::string_view name);
/// A free one (its name not in `taken`, compared as players' names: "[BOT] " + name) of
/// `playerClass` within [minTier, maxTier], at random; nullptr if all are taken.
const Persona* pickPersona(std::uint8_t playerClass, Tier minTier, Tier maxTier, const std::set<std::string>& taken,
                           std::mt19937& rng);

std::string_view tierName(Tier tier);
std::optional<Tier> tierNamed(std::string_view name); ///< "easy", "medium" ("normal"), "hard"

} // namespace nox::bot
