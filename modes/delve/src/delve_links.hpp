// Delving Mode's records that name things (object_links.hpp's tables, for the DelveDirector): each
// ObjectRef field with what a map change does to it, carried right after each switch
// (DelveDirector::carryLinks, GameHost::Options::carryLinks). Each is Keep, as the Delve has always
// had them: a level's records are the level's (beginLevel and the level's change empty them, the
// lobby's things are found again as it loads), and the players' units keep their keys. (Internal to
// the Delve's sources.)
#pragma once

#include "noxcore/server/delve.hpp"

#include "block_links.hpp"

namespace nox::server {

template <>
struct Links<DelveDirector::Celebration> {
    using S = DelveDirector::Celebration;
    static constexpr std::string_view name = "DelveDirector::Celebration";
    static constexpr auto fields = std::tuple{NOX_LINK(S, glow, Keep)};
};

template <>
struct Links<DelveDirector::Barrel> {
    using S = DelveDirector::Barrel;
    static constexpr std::string_view name = "DelveDirector::Barrel";
    static constexpr auto fields = std::tuple{NOX_LINK(S, extent, Keep)};
};

template <>
struct Links<DelveDirector::Elite> {
    using S = DelveDirector::Elite;
    static constexpr std::string_view name = "DelveDirector::Elite";
    static constexpr auto fields = std::tuple{NOX_LINK(S, extent, Keep)};
};

template <>
struct Links<DelveDirector::ObjectiveRun> {
    using S = DelveDirector::ObjectiveRun;
    static constexpr std::string_view name = "DelveDirector::ObjectiveRun";
    static constexpr auto fields = std::tuple{NOX_LINK(S, targets, Keep), NOX_LINK(S, quarry, Keep)};
};

template <>
struct Links<DelveDirector> {
    using S = DelveDirector;
    static constexpr std::string_view name = "DelveDirector";
    static constexpr auto fields = std::tuple{
        NOX_LINK(S, watch_, Keep),          // (its zones made again each level, its lists emptied: watchLevel, watchLobby)
        NOX_LINK(S, lobbySigns_, Keep),     // (the lobby's: found again as it loads)
        NOX_LINK(S, guardianWatched_, Keep), // (the level's change: none)
        NOX_LINK(S, boss_, Keep),
        NOX_LINK(S, gates_, Keep),
        NOX_LINK(S, gateSigns_, Keep),
        NOX_LINK(S, healed_, Keep),         // (players' units, by key: the level's)
        NOX_LINK(S, offers_, Keep),         // (by player: the campfire's, taken back from one who walks off or goes)
        NOX_LINK(S, celebrations_, Keep),
        NOX_LINK(S, barrels_, Keep),        // (beginLevel empties these)
        NOX_LINK(S, elites_, Keep),
        NOX_LINK(S, blessOffers_, Keep),    // (by player: the altar's)
        NOX_LINK(S, objective_, Keep),
        NOX_LINK(S, keptHealth_, Keep),
    };
    /// Every struct of the Delve's with a table (the test walks them).
    using structs = LinkStructs<DelveDirector::Celebration, DelveDirector::Barrel, DelveDirector::Elite, DelveDirector::ObjectiveRun, DelveDirector>;
};

namespace detail {
static_assert(allTablesFit(Links<DelveDirector>::structs{}), "an object link's rule doesn't fit its member, or a member is listed twice (delve_links.hpp)");
} // namespace detail

} // namespace nox::server
