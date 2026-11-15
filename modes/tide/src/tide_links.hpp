// Undead Tide's records that name things (object_links.hpp's tables, for the TideDirector): each
// ObjectRef field with what a map change does to it, carried right after each switch
// (TideDirector::carryLinks, GameHost::Options::carryLinks). Most are Keep: the old map's things are
// forgotten as the next map comes in (beginArena empties the arena's records, watchLobby makes the
// lobby's zones afresh). A runner's unit and the charmed armour are carried (a player's unit and
// what it carries keep their keys; what stayed behind names nothing), and the lobby's things are
// cleared, so each load of the lobby finds them again (runLobby, watchLobby). (Internal to the
// Tide's sources.)
#pragma once

#include "noxcore/server/tide.hpp"

#include "block_links.hpp"

namespace nox::server {

template <>
struct Links<TideDirector::Runner> {
    using S = TideDirector::Runner;
    static constexpr std::string_view name = "TideDirector::Runner";
    // (Its player's unit as last seen, set each frame while it's in the arena; carried with its
    // player, none for one who left: D2295.)
    static constexpr auto fields = std::tuple{NOX_LINK(S, unit, Carry)};
};

template <>
struct Links<TideDirector::Asked> {
    using S = TideDirector::Asked;
    static constexpr std::string_view name = "TideDirector::Asked";
    static constexpr auto fields = std::tuple{NOX_LINK(S, unit, Keep)}; // (the lobby's: put away as the run begins, closeAsks)
};

template <>
struct Links<TideDirector::Chest> {
    using S = TideDirector::Chest;
    static constexpr std::string_view name = "TideDirector::Chest";
    static constexpr auto fields = std::tuple{NOX_LINK(S, extent, Keep)};
};

template <>
struct Links<TideDirector> {
    using S = TideDirector;
    static constexpr std::string_view name = "TideDirector";
    static constexpr auto fields = std::tuple{
        NOX_LINK(S, runners_, Nested),
        NOX_LINK(S, shrine_, Keep),       // (beginArena: none)
        NOX_LINK(S, board_, Clear),       // (the lobby's: found again as it loads, runLobby; D2296)
        NOX_LINK(S, signs_, Clear),
        NOX_LINK(S, portal_, Clear),
        NOX_LINK(S, askedAtGate_, Nested),
        NOX_LINK(S, boardWaiting_, Keep), // (watchLobby: emptied)
        NOX_LINK(S, watch_, Keep),        // (the lobby's zones: watchLobby makes them afresh)
        NOX_LINK(S, prepared_, Keep),     // (beginArena empties these)
        NOX_LINK(S, gemField_, Keep),
        NOX_LINK(S, pickupField_, Keep),
        NOX_LINK(S, glows_, Keep),
        NOX_LINK(S, chests_, Keep),
        NOX_LINK(S, openedChests_, Keep),
        NOX_LINK(S, bodies_, Keep),
        NOX_LINK(S, timers_, Keep),
        NOX_LINK(S, horde_, Keep),
        NOX_LINK(S, boss_, Keep),
        NOX_LINK(S, charms_, CarryDropLost), // (carried armour's under its key now; armour left behind dropped: D2294)
        NOX_LINK(S, told_, Keep),         // (by peer: a newcomer's told afresh)
    };
    /// Every struct of the Tide's with a table (the test walks them).
    using structs = LinkStructs<TideDirector::Runner, TideDirector::Asked, TideDirector::Chest, TideDirector>;
};

namespace detail {
static_assert(allTablesFit(Links<TideDirector>::structs{}), "an object link's rule doesn't fit its member, or a member is listed twice (tide_links.hpp)");
} // namespace detail

} // namespace nox::server
