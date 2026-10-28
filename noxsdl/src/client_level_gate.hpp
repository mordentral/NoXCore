// A game that wants a newer NoX Core than this one (lobby/extended.hpp's client levels): its
// extended reply names a lowest client level above ours (lobby::kClientLevel). The game list marks
// it "Update needed", and choosing it shows lobby::kClientTooOldNotice in a message box instead of
// joining; a join that the host's level turned away shows the same box. By decision, the box
// names no site and links nowhere: players know where they got NoX Core.
// A game with no lowest level (0, or no extended reply: a stock host, an older noxcpp one) is
// never marked and is joined as before. No SDL here (the tests use it).
#pragma once

#include "noxcore/lobby/browser.hpp"
#include "noxcore/lobby/extended.hpp"

#include <cstdint>
#include <optional>

namespace noxsdl::client_level {

/// Whether a host's extended reply asks for a newer client than level `ours`.
inline bool updateNeeded(const std::optional<nox::lobby::ExtendedServer>& extended,
                         std::uint16_t ours = nox::lobby::kClientLevel) noexcept
{
    return extended && extended->minClientLevel > ours;
}

/// Whether the game list marks this game "Update needed".
inline bool updateNeeded(const nox::lobby::ServerEntry& entry, std::uint16_t ours = nox::lobby::kClientLevel) noexcept
{
    return updateNeeded(entry.extended, ours);
}

/// Choosing a game to join (double click, Enter, Join, the description box's Join): the notice
/// instead of the join when the list knows the game wants a newer client. `listed` is the list's
/// entry for the address chosen (null: not a listed game, e.g. one we host; joined as before).
inline bool noticeInsteadOfJoin(const nox::lobby::ServerEntry* listed, std::uint16_t ours = nox::lobby::kClientLevel) noexcept
{
    return listed && updateNeeded(*listed, ours);
}

/// A join that ended: the notice as a message box when it failed with no other end (not timed out,
/// lost or kicked) and the host's reply named a lowest level above ours (LiveSession::tooOld(), or
/// the host refusing the join once its reply came).
inline bool noticeAfterJoin(bool failed, bool otherEnd, const std::optional<nox::lobby::ExtendedServer>& extended,
                            std::uint16_t ours = nox::lobby::kClientLevel) noexcept
{
    return failed && !otherEnd && updateNeeded(extended, ours);
}

} // namespace noxsdl::client_level
