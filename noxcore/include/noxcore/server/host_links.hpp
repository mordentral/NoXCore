// The host's records that name things (object_links.hpp's tables, for GameHost's own, the map's
// script's and the map editor's Play From Here): each ObjectRef field with its rule for a map
// change, carried by GameHost::loadNextMap right after the switch (carryLinks with Server::carry).
// The host's name players' units, which a map change keeps under their keys; the script's are the
// map's (a ScriptHost is made with each map).
#pragma once

#include "noxcore/server/game_host.hpp"
#include "noxcore/server/object_links.hpp"
#include "noxcore/server/play_from_here.hpp"

namespace nox::server {

template <>
struct Links<HostedPlayer> {
    using S = HostedPlayer;
    static constexpr std::string_view name = "HostedPlayer";
    static constexpr auto fields = std::tuple{NOX_LINK(S, extent, Carry)}; // (its unit: a player's, always kept)
};

template <>
struct Links<GameHost::Vote> {
    using S = GameHost::Vote;
    static constexpr std::string_view name = "GameHost::Vote";
    // (A player's unit, kept under its key; one whose player left is ended by updateVotes as its
    // target's gone. Keep, as before: Carry would end one whose player left in the switch's own tick
    // by its key none, not by its old key in the new map.)
    static constexpr auto fields = std::tuple{NOX_LINK(S, target, Keep)};
};

template <>
struct Links<GameHost> {
    using S = GameHost;
    static constexpr std::string_view name = "GameHost";
    static constexpr auto fields = std::tuple{
        NOX_LINK(S, players_, Nested),
        NOX_LINK(S, votes_, Nested),
        NOX_LINK(S, echoTo_, Carry), // (a sysop's unit, for its command's run alone: none between commands)
        NOX_LINK(S, server_, Nested), // (its World and script: each map's own, below)
    };
    /// Every struct of the host's with a table (the test walks them).
    using structs = LinkStructs<HostedPlayer, GameHost::Vote, GameHost, ScriptTimer, ScriptHost::QueuedBlock, ScriptHost, Server, PlayFromHere>;
};

// The map's script (made with each map: nothing of it outlives a map change).
template <>
struct Links<ScriptTimer> {
    using S = ScriptTimer;
    static constexpr std::string_view name = "ScriptTimer";
    static constexpr auto fields = std::tuple{NOX_LINK(S, trigger, Keep), NOX_LINK(S, caller, Keep)}; // (a hot reload's timers keep theirs)
};

template <>
struct Links<ScriptHost::QueuedBlock> {
    using S = ScriptHost::QueuedBlock;
    static constexpr std::string_view name = "ScriptHost::QueuedBlock";
    static constexpr auto fields = std::tuple{NOX_LINK(S, caller, Keep), NOX_LINK(S, trigger, Keep)};
};

template <>
struct Links<ScriptHost> {
    using S = ScriptHost;
    static constexpr std::string_view name = "ScriptHost";
    static constexpr auto fields = std::tuple{NOX_LINK(S, timers_, Fresh), NOX_LINK(S, queued_, Fresh)}; // (the new map's script its own)
};

template <>
struct Links<Server> {
    using S = Server;
    static constexpr std::string_view name = "Server";
    // (The World: the map change's own walk, loadMapKeeping, and its kept things'; the script the
    // new map's.)
    static constexpr auto fields = std::tuple{NOX_LINK(S, world_, Fresh), NOX_LINK(S, script_, Fresh)};
};

// The map editor's Play From Here (noxsdl's: its play-test's own map alone).
template <>
struct Links<PlayFromHere> {
    using S = PlayFromHere;
    static constexpr std::string_view name = "PlayFromHere";
    static constexpr auto fields = std::tuple{NOX_LINK(S, extent_, Keep)};
};

namespace detail {
static_assert(allTablesFit(Links<GameHost>::structs{}), "an object link's rule doesn't fit its member, or a member is listed twice (host_links.hpp)");
} // namespace detail

} // namespace nox::server
