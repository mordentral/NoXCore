// noxcpp's map vote (no stock counterpart). At a match's end the
// players pick the next map from up to four for the game's mode (the map cycle's next among
// them); any player can also call one mid-game (once every 5 minutes), which changes the map only
// when one map has more than half of the players' votes. noxsdl players call it, see the maps and
// vote by MSG_NOXCPP_MAPVOTE (0xF3); stock ones by chat: "?MapVote" to call one, a map's number to
// vote (a noxsdl player's chat is only chat). No votes leaves the map cycle's
// choice; a tie goes to the first of the tied in the list.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace nox::server {

class MapVote {
public:
    enum class Kind : std::uint8_t { EndOfMatch = 0, Called = 1 };
    static constexpr std::size_t kMaxCandidates = 4;

    bool active() const noexcept { return active_; }
    Kind kind() const noexcept { return kind_; }
    const std::vector<std::string>& candidates() const noexcept { return candidates_; }
    /// The server frame it closes at.
    std::uint32_t endsAt() const noexcept { return endsAt_; }
    /// Who called it (a Called vote's: its peer), 0 for a match's end.
    std::uint32_t caller() const noexcept { return caller_; }

    void open(Kind kind, std::vector<std::string> candidates, std::uint32_t endsAt, std::uint32_t caller = 0);
    void close() noexcept;
    /// A voter's choice (0-based; a later one replaces it); false when it isn't a candidate or no
    /// vote is open.
    bool vote(std::uint32_t voter, int choice);
    /// A voter gone (its vote with it).
    void forget(std::uint32_t voter);
    std::optional<int> choiceOf(std::uint32_t voter) const;
    /// The votes for each candidate.
    std::vector<int> tally() const;
    /// The map chosen, as its kind decides: a match's end the most voted (a tie: the first of
    /// them), none without votes; a called vote the one with more than half of `voters`.
    std::optional<int> winner(std::size_t voters) const;

    /// Up to kMaxCandidates of `forMode` (the server's maps for the mode): the map cycle's next
    /// first when it's one of them, then others at random; never `current`, unless it's the only
    /// map there is.
    static std::vector<std::string> pick(const std::vector<std::string>& forMode, const std::optional<std::string>& cycleNext,
                                         const std::string& current, std::minstd_rand& rng);

private:
    bool active_ = false;
    Kind kind_ = Kind::EndOfMatch;
    std::vector<std::string> candidates_;
    std::uint32_t endsAt_ = 0;
    std::uint32_t caller_ = 0;
    std::map<std::uint32_t, int> votes_;
};

/// MSG_NOXCPP_MAPVOTE (0xF3), noxcpp's own, only between a noxcpp server and the clients that
/// identified themselves (lobby/extended.hpp): {u8 0xF3, u8 sub, u16 the rest's length, the rest}.
namespace mapvote {
inline constexpr std::uint8_t kOp = 0xF3;
// Server to client.
inline constexpr std::uint8_t kOpen = 0;  ///< u8 kind, u16 seconds left, u8 count, then count x {u8 length, name}
inline constexpr std::uint8_t kTally = 1; ///< u8 count, count x u8 votes, u8 voters, u8 its own choice (0xFF none)
inline constexpr std::uint8_t kClose = 2; ///< u8 kind, u8 the winner (0xFF none: the cycle's map, or the call failed)
/// u8 1 when a vote can be called here (the vote on, a network game, not a quest), else 0: at the
/// join and each time it changes (noxsdl's Vote Map follows it).
inline constexpr std::uint8_t kAvailable = 3;
// Client to server, from noxsdl (a stock client says "?MapVote" in chat to call one, and a map's
// number to vote).
inline constexpr std::uint8_t kVote = 0x10; ///< u8 the choice
inline constexpr std::uint8_t kCall = 0x11; ///< nothing: a vote called (the in-game menu's Vote Map)

std::vector<std::uint8_t> openMessage(MapVote::Kind kind, std::uint16_t secondsLeft, const std::vector<std::string>& candidates);
std::vector<std::uint8_t> tallyMessage(const std::vector<int>& tally, std::size_t voters, std::optional<int> mine);
std::vector<std::uint8_t> closeMessage(MapVote::Kind kind, std::optional<int> winner);
std::vector<std::uint8_t> availableMessage(bool available);
std::vector<std::uint8_t> voteMessage(int choice);
std::vector<std::uint8_t> callMessage();
/// The message's sub and the rest, or nullopt when it isn't one (or is cut short).
std::optional<std::pair<std::uint8_t, std::span<const std::uint8_t>>> parse(std::span<const std::uint8_t> m);
} // namespace mapvote

} // namespace nox::server
