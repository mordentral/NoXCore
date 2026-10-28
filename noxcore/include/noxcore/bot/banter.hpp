// What a bot says when a kill involves a person: a taunt over someone it killed, a whine at
// someone who killed it. Now and then, not every kill (BotSquad decides when). And on a chat
// map, waiting for the host to start the game: a nudge now and then.
#pragma once

#include <cstddef>
#include <deque>
#include <random>
#include <string>
#include <string_view>

namespace nox::bot {

class Banter {
public:
    enum class Kind { Taunt, Whine, Waiting };

    /// A line of `kind` at `person` (their name, which some lines use), none of the last few
    /// said again.
    std::string line(Kind kind, std::string_view person, std::mt19937& rng);

    /// The lines of each kind ("{name}" for the person's name).
    static std::size_t count(Kind kind);
    static std::string_view raw(Kind kind, std::size_t i);

private:
    static constexpr std::size_t kRecent = 20;
    std::deque<std::string_view> recent_;
};

} // namespace nox::bot
