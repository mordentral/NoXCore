// The script modes' C++ records that name things (object_links.hpp's tables). A script mode keeps
// the things it works with in its Luau state (numbers the engine doesn't see); what its C++ side
// keeps past the frame is the draw scales owed (mod_draw_scale.hpp), each let go as it's flushed
// (a thing gone, or not known to the player, dropped). The told events (mod_events.hpp) are the
// frame's: keys, not links. (Internal to the script modes' sources.)
#pragma once

#include "noxcore/server/object_links.hpp"

#include "mod_draw_scale.hpp"
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
struct Links<modes::script::Parts> {
    using S = modes::script::Parts;
    static constexpr std::string_view name = "Parts";
    static constexpr auto fields = std::tuple{NOX_LINK(S, drawScales, Keep)};
    /// Every struct of the script modes' with a table (the test walks them).
    using structs = LinkStructs<modes::script::DrawScales::Owed, modes::script::DrawScales, modes::script::Parts>;
};

namespace detail {
static_assert(allTablesFit(Links<modes::script::Parts>::structs{}), "an object link's rule doesn't fit its member, or a member is listed twice (script_links.hpp)");
} // namespace detail

} // namespace nox::server
