// The co-op campaign's records that name things (object_links.hpp's tables, for the CoopDirector):
// each ObjectRef field with what a map change does to it, carried right after each switch
// (CoopDirector::carryLinks, GameHost::Options::carryLinks). The party's units keep their keys; the
// rest Keep, as it always was (mapLoaded empties the map's records and makes the new map's).
// (Internal to the co-op's sources.)
#pragma once

#include "noxcore/coop/coop.hpp"
#include "noxcore/server/object_links.hpp"

namespace nox::server {

template <>
struct Links<coop::CoopDirector::Member> {
    using S = coop::CoopDirector::Member;
    static constexpr std::string_view name = "CoopDirector::Member";
    static constexpr auto fields = std::tuple{NOX_LINK(S, extent, Carry)}; // (its unit: a player's, kept; it leaves the party as it goes)
};

template <>
struct Links<coop::CoopDirector::Gather> {
    using S = coop::CoopDirector::Gather;
    static constexpr std::string_view name = "CoopDirector::Gather";
    // (The old map's exit, and who stood at it: mapLoaded ends the gathering.)
    static constexpr auto fields = std::tuple{NOX_LINK(S, exit, Clear), NOX_LINK(S, standing, Clear)};
};

template <>
struct Links<coop::CoopDirector::PartyCreature> {
    using S = coop::CoopDirector::PartyCreature;
    static constexpr std::string_view name = "CoopDirector::PartyCreature";
    static constexpr auto fields = std::tuple{NOX_LINK(S, extent, Keep), NOX_LINK(S, owner, Keep)}; // (looked for again as the map loads: creaturesDirty_)
};

template <>
struct Links<coop::CoopDirector> {
    using S = coop::CoopDirector;
    static constexpr std::string_view name = "CoopDirector";
    static constexpr auto fields = std::tuple{
        NOX_LINK(S, party_, Nested),
        NOX_LINK(S, baseHealth_, Keep),   // (mapLoaded empties these, the new map's made by its rescale)
        NOX_LINK(S, copies_, Keep),
        NOX_LINK(S, sceneViewers_, Keep),
        NOX_LINK(S, sceneTalker_, Keep),
        NOX_LINK(S, wipeBy_, Keep),
        NOX_LINK(S, gather_, Keep),       // (mapLoaded: none)
        NOX_LINK(S, mirrorNpc_, Keep),    // (the mirrored talk's: watchDialog closes it as the talk ends)
        NOX_LINK(S, mirrorTalker_, Keep),
        NOX_LINK(S, partyCreatures_, Keep),
    };
    /// Every struct of the co-op's with a table (the test walks them).
    using structs = LinkStructs<coop::CoopDirector::Member, coop::CoopDirector::Gather, coop::CoopDirector::PartyCreature, coop::CoopDirector>;
};

namespace detail {
static_assert(allTablesFit(Links<coop::CoopDirector>::structs{}), "an object link's rule doesn't fit its member, or a member is listed twice (coop_links.hpp)");
} // namespace detail

} // namespace nox::server
