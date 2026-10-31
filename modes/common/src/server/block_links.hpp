// The modes' generic blocks' links (object_links.hpp's tables, for the kept lists, a horde,
// gatherables, satellites, the ready timers and the watchers): each ObjectRef field with what a map
// change does to it were the block carried as a part. Each mode that holds a block lists it in its
// own table: Keep where the mode empties it, or makes it again, as each of its maps loads (as the
// Tide and the Delve do), Nested where it's carried. (Internal to the modes' sources.)
#pragma once

#include "noxcore/server/object_links.hpp"

#include "gather.hpp"
#include "horde.hpp"
#include "kept.hpp"
#include "ready_timers.hpp"
#include "satellites.hpp"
#include "watchers.hpp"

namespace nox::server {

template <>
struct Links<KeptLists::List> {
    using S = KeptLists::List;
    static constexpr std::string_view name = "KeptLists::List";
    static constexpr auto fields = std::tuple{NOX_LINK(S, things, CarryDropLost)}; // (what it keeps: one left behind, gone)
};

template <>
struct Links<KeptLists::Holder> {
    using S = KeptLists::Holder;
    static constexpr std::string_view name = "KeptLists::Holder";
    static constexpr auto fields = std::tuple{NOX_LINK(S, holder, Carry), NOX_LINK(S, lists, Nested)};
};

template <>
struct Links<KeptLists> {
    using S = KeptLists;
    static constexpr std::string_view name = "KeptLists";
    static constexpr auto fields = std::tuple{NOX_LINK(S, holders_, Nested)};
};

template <>
struct Links<Horde::Member> {
    using S = Horde::Member;
    static constexpr std::string_view name = "Horde::Member";
    static constexpr auto fields = std::tuple{NOX_LINK(S, extent, Carry)}; // (one left behind: none, reaped as gone)
};

template <>
struct Links<Horde> {
    using S = Horde;
    static constexpr std::string_view name = "Horde";
    static constexpr auto fields = std::tuple{NOX_LINK(S, members_, Nested)};
};

template <>
struct Links<Gatherables::Item> {
    using S = Gatherables::Item;
    static constexpr std::string_view name = "Gatherables::Item";
    static constexpr auto fields = std::tuple{NOX_LINK(S, extent, Carry)};
};

template <>
struct Links<Gatherables> {
    using S = Gatherables;
    static constexpr std::string_view name = "Gatherables";
    static constexpr auto fields = std::tuple{NOX_LINK(S, items_, Nested)};
};

template <>
struct Links<Satellites::Group> {
    using S = Satellites::Group;
    static constexpr std::string_view name = "Satellites::Group";
    static constexpr auto fields = std::tuple{NOX_LINK(S, things, CarryDropLost)};
};

template <>
struct Links<Satellites> {
    using S = Satellites;
    static constexpr std::string_view name = "Satellites";
    static constexpr auto fields = std::tuple{NOX_LINK(S, groups_, CarryDropLost)}; // (by centre: one left behind, its group gone)
};

template <>
struct Links<ReadyTimers::Held> {
    using S = ReadyTimers::Held;
    static constexpr std::string_view name = "ReadyTimers::Held";
    static constexpr auto fields = std::tuple{NOX_LINK(S, holder, Carry)};
};

template <>
struct Links<ReadyTimers> {
    using S = ReadyTimers;
    static constexpr std::string_view name = "ReadyTimers";
    static constexpr auto fields = std::tuple{NOX_LINK(S, satellites_, Nested), NOX_LINK(S, kept_, Nested), NOX_LINK(S, waits_, Nested)};
};

template <>
struct Links<Watchers::Zone> {
    using S = Watchers::Zone;
    static constexpr std::string_view name = "Watchers::Zone";
    static constexpr auto fields = std::tuple{NOX_LINK(S, thing, Keep)}; // (its mode's: retarget as a map loads)
};

template <>
struct Links<Watchers::Group::Cached> {
    using S = Watchers::Group::Cached;
    static constexpr std::string_view name = "Watchers::Group::Cached";
    static constexpr auto fields = std::tuple{NOX_LINK(S, unit, Carry)}; // (a player's unit, with its join's number)
};

template <>
struct Links<Watchers::Group> {
    using S = Watchers::Group;
    static constexpr std::string_view name = "Watchers::Group";
    static constexpr auto fields = std::tuple{
        NOX_LINK(S, zones, Nested),
        NOX_LINK(S, keptByPeer, Nested),
        NOX_LINK(S, gates, Keep),   // (its mode's: exits() as a level's gates are made)
        NOX_LINK(S, through, Keep), // (with them: reset)
    };
};

template <>
struct Links<Watchers::Watched> {
    using S = Watchers::Watched;
    static constexpr std::string_view name = "Watchers::Watched";
    static constexpr auto fields = std::tuple{NOX_LINK(S, thing, Keep)}; // (its mode's: unwatchAll as a map loads)
};

template <>
struct Links<Watchers::List> {
    using S = Watchers::List;
    static constexpr std::string_view name = "Watchers::List";
    static constexpr auto fields = std::tuple{NOX_LINK(S, things, Nested)};
};

template <>
struct Links<Watchers> {
    using S = Watchers;
    static constexpr std::string_view name = "Watchers";
    static constexpr auto fields = std::tuple{NOX_LINK(S, lists_, Nested), NOX_LINK(S, groups_, Nested)};
    /// Every block struct with a table (the test walks them).
    using structs = LinkStructs<KeptLists::List, KeptLists::Holder, KeptLists, Horde::Member, Horde, Gatherables::Item, Gatherables, Satellites::Group,
                                Satellites, ReadyTimers::Held, ReadyTimers, Watchers::Zone, Watchers::Group::Cached, Watchers::Group, Watchers::Watched,
                                Watchers::List, Watchers>;
};

namespace detail {
static_assert(allTablesFit(Links<Watchers>::structs{}), "an object link's rule doesn't fit its member, or a member is listed twice (block_links.hpp)");
} // namespace detail

} // namespace nox::server
