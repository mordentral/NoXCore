// The map vote's state and its MSG_NOXCPP_MAPVOTE messages; the rules are in map_vote.hpp.
#include "noxcore/server/map_vote.hpp"

#include "noxcore/util/text.hpp"
#include "noxcore/util/random.hpp"

#include <algorithm>
#include <cctype>

namespace nox::server {

void MapVote::open(Kind kind, std::vector<std::string> candidates, std::uint32_t endsAt, std::uint32_t caller)
{
    active_ = true;
    kind_ = kind;
    candidates_ = std::move(candidates);
    if (candidates_.size() > kMaxCandidates) {
        candidates_.resize(kMaxCandidates);
    }
    endsAt_ = endsAt;
    caller_ = caller;
    votes_.clear();
}

void MapVote::close() noexcept
{
    active_ = false;
    votes_.clear();
}

bool MapVote::vote(std::uint32_t voter, int choice)
{
    if (!active_ || choice < 0 || static_cast<std::size_t>(choice) >= candidates_.size()) {
        return false;
    }
    votes_[voter] = choice;
    return true;
}

void MapVote::forget(std::uint32_t voter)
{
    votes_.erase(voter);
}

std::optional<int> MapVote::choiceOf(std::uint32_t voter) const
{
    const auto it = votes_.find(voter);
    return it != votes_.end() ? std::optional<int>{it->second} : std::nullopt;
}

std::vector<int> MapVote::tally() const
{
    std::vector<int> t(candidates_.size(), 0);
    for (const auto& [voter, choice] : votes_) {
        ++t[static_cast<std::size_t>(choice)];
    }
    return t;
}

std::optional<int> MapVote::winner(std::size_t voters) const
{
    const std::vector<int> t = tally();
    int best = -1;
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (t[i] > 0 && (best < 0 || t[i] > t[static_cast<std::size_t>(best)])) {
            best = static_cast<int>(i); // (the first of the tied keeps it)
        }
    }
    if (best < 0) {
        return std::nullopt;
    }
    // A called vote needs a strict majority of all the voters, not just of those who voted.
    if (kind_ == Kind::Called && !(static_cast<std::size_t>(t[static_cast<std::size_t>(best)]) * 2 > voters)) {
        return std::nullopt;
    }
    return best;
}

std::vector<std::string> MapVote::pick(const std::vector<std::string>& forMode, const std::optional<std::string>& cycleNext,
                                       const std::string& current, std::minstd_rand& rng)
{
    // Map names compare without case.
    using util::equalsNoCase;
    std::vector<std::string> out;
    const auto has = [&](const std::string& name) { return std::ranges::any_of(out, [&](const std::string& o) { return equalsNoCase(o, name); }); };
    // The cycle's next map goes first, if it is for this mode and isn't the current map.
    if (cycleNext && !equalsNoCase(*cycleNext, current) && std::ranges::any_of(forMode, [&](const std::string& m) { return equalsNoCase(m, *cycleNext); })) {
        out.push_back(*cycleNext);
    }
    // Then the mode's other maps (not the current one, not already listed) in random order.
    std::vector<std::string> rest;
    for (const std::string& m : forMode) {
        if (!equalsNoCase(m, current) && !has(m)) {
            rest.push_back(m);
        }
    }
    nox::util::shuffle(rest.begin(), rest.end(), rng);
    for (const std::string& m : rest) {
        if (out.size() >= kMaxCandidates) {
            break;
        }
        out.push_back(m);
    }
    if (out.empty() && std::ranges::any_of(forMode, [&](const std::string& m) { return equalsNoCase(m, current); })) {
        out.push_back(current); // (the only map for the mode)
    }
    return out;
}

namespace mapvote {

namespace {

// {op, sub, u16 length of `rest` (little-endian), rest}.
std::vector<std::uint8_t> framed(std::uint8_t sub, const std::vector<std::uint8_t>& rest)
{
    std::vector<std::uint8_t> m = {kOp, sub, static_cast<std::uint8_t>(rest.size()), static_cast<std::uint8_t>(rest.size() >> 8)};
    m.insert(m.end(), rest.begin(), rest.end());
    return m;
}

} // namespace

std::vector<std::uint8_t> openMessage(MapVote::Kind kind, std::uint16_t secondsLeft, const std::vector<std::string>& candidates)
{
    std::vector<std::uint8_t> rest = {static_cast<std::uint8_t>(kind), static_cast<std::uint8_t>(secondsLeft),
                                      static_cast<std::uint8_t>(secondsLeft >> 8), static_cast<std::uint8_t>(candidates.size())};
    for (const std::string& c : candidates) {
        // Names longer than 255 bytes are cut to fit their u8 length.
        const std::size_t n = std::min<std::size_t>(c.size(), 255);
        rest.push_back(static_cast<std::uint8_t>(n));
        rest.insert(rest.end(), c.begin(), c.begin() + static_cast<std::ptrdiff_t>(n));
    }
    return framed(kOpen, rest);
}

std::vector<std::uint8_t> tallyMessage(const std::vector<int>& tally, std::size_t voters, std::optional<int> mine)
{
    std::vector<std::uint8_t> rest = {static_cast<std::uint8_t>(tally.size())};
    for (const int t : tally) {
        rest.push_back(static_cast<std::uint8_t>(std::min(t, 255)));
    }
    rest.push_back(static_cast<std::uint8_t>(std::min<std::size_t>(voters, 255)));
    rest.push_back(mine ? static_cast<std::uint8_t>(*mine) : std::uint8_t{0xFF});
    return framed(kTally, rest);
}

std::vector<std::uint8_t> closeMessage(MapVote::Kind kind, std::optional<int> winner)
{
    return framed(kClose, {static_cast<std::uint8_t>(kind), winner ? static_cast<std::uint8_t>(*winner) : std::uint8_t{0xFF}});
}

std::vector<std::uint8_t> availableMessage(bool available)
{
    return framed(kAvailable, {static_cast<std::uint8_t>(available ? 1 : 0)});
}

std::vector<std::uint8_t> voteMessage(int choice)
{
    return framed(kVote, {static_cast<std::uint8_t>(choice)});
}

std::vector<std::uint8_t> callMessage()
{
    return framed(kCall, {});
}

std::optional<std::pair<std::uint8_t, std::span<const std::uint8_t>>> parse(std::span<const std::uint8_t> m)
{
    if (m.size() < 4 || m[0] != kOp) {
        return std::nullopt;
    }
    const std::size_t length = m[2] | static_cast<std::size_t>(m[3]) << 8;
    if (m.size() < 4 + length) {
        return std::nullopt;
    }
    return std::pair{m[1], m.subspan(4, length)};
}

} // namespace mapvote

} // namespace nox::server
