// The script modes' C++ records that name things (object_links.hpp's tables). A script mode keeps
// the things it works with in its Luau state (numbers the engine doesn't see); what its C++ side
// keeps past the frame is the draw scales owed (mod_draw_scale.hpp), each let go as it's flushed
// (a thing gone, or not known to the player, dropped), and the leaps under way (mod_leap.hpp: let
// go at a map's load). The told events (mod_events.hpp) are the frame's: keys, not links.
// (Internal to the script modes' sources.)
#pragma once

#include "noxcore/server/object_links.hpp"

#include "mod_draw_scale.hpp"
#include "mod_leap.hpp"
#include "mod_parts.hpp"

namespace nox::server {

template <>
struct Links<modes::script::DrawScales::Owed> {
    using S = modes::script::DrawScales::Owed;
    static constexpr std::string_view name = "DrawScales::Owed";
    static constexpr auto fields = std::tuple{NOX_LINK(S, extent, Keep)};
};

template <>
struct Links<modes::script::DrawScales> {
    using S = modes::script::DrawScales;
    static constexpr std::string_view name = "DrawScales";
    static constexpr auto fields = std::tuple{NOX_LINK(S, owed_, Keep)}; // (flush lets go of one whose thing is gone; reset with the mode)
};

template <>
struct Links<modes::script::Leaps::Leap> {
    using S = modes::script::Leaps::Leap;
    static constexpr std::string_view name = "Leaps::Leap";
    static constexpr auto fields = std::tuple{NOX_LINK(S, unit, Keep)};
};

template <>
struct Links<modes::script::Leaps> {
    using S = modes::script::Leaps;
    static constexpr std::string_view name = "Leaps";
    static constexpr auto fields = std::tuple{NOX_LINK(S, leaps_, Keep)}; // (a map's load lets them all go: Leaps::reset)
};

template <>
struct Links<modes::script::Spins::Spin> {
    using S = modes::script::Spins::Spin;
    static constexpr std::string_view name = "Spins::Spin";
    static constexpr auto fields = std::tuple{NOX_LINK(S, unit, Keep)};
};

template <>
struct Links<modes::script::Spins> {
    using S = modes::script::Spins;
    static constexpr std::string_view name = "Spins";
    static constexpr auto fields = std::tuple{NOX_LINK(S, spins_, Keep)}; // (a map's load lets them all go: Spins::reset)
};

template <>
struct Links<modes::script::Parts> {
    using S = modes::script::Parts;
    static constexpr std::string_view name = "Parts";
    static constexpr auto fields = std::tuple{NOX_LINK(S, drawScales, Keep), NOX_LINK(S, leaps, Keep), NOX_LINK(S, spins, Keep)};
    /// Every struct of the script modes' with a table (the test walks them).
    using structs = LinkStructs<modes::script::DrawScales::Owed, modes::script::DrawScales, modes::script::Leaps::Leap,
                                modes::script::Leaps, modes::script::Spins::Spin, modes::script::Spins, modes::script::Parts>;
};

namespace detail {
static_assert(allTablesFit(Links<modes::script::Parts>::structs{}), "an object link's rule doesn't fit its member, or a member is listed twice (script_links.hpp)");
} // namespace detail

} // namespace nox::server
