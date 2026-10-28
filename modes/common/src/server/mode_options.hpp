// What the Delve's and the Tide's mode entries share (delve_mode.cpp, tide_mode.cpp): their
// options as the dedicated server's flags always read them, a server's own maps, the records store a
// host hands them. (Internal to the modes' sources.)
#pragma once

#include "noxcore/modes/mode.hpp"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace nox::modes::detail {

/// An option's text as a string (for std::strtoul, std::atof and the like).
inline std::string text(std::string_view s)
{
    return std::string(s);
}

/// A text setting of the dedicated server's own flag, not shown in Host Custom.
inline OptionSpec textOption(std::string key, std::string label, std::string flag)
{
    return {.key = std::move(key), .label = std::move(label), .kind = OptionSpec::Kind::Text, .legacyFlag = std::move(flag), .hostPanel = false};
}

/// The first run's seed: the "seed" setting (a whole number), else the host's.
inline std::uint32_t seedOption(const ServerContext& ctx)
{
    const auto seed = option(ctx.options, "seed");
    return seed ? static_cast<std::uint32_t>(std::strtoul(text(*seed).c_str(), nullptr, 10)) : ctx.seed;
}

/// A pool's server N's maps: its own, <prefix>N and its lobby <prefix>LN (a host of its own keeps
/// the names it has).
inline void poolMaps(const ServerContext& ctx, std::string_view prefix, std::string& mapName, std::string& lobbyName)
{
    if (ctx.slot > 0) {
        mapName = std::string(prefix) + std::to_string(ctx.slot);
        lobbyName = std::string(prefix) + "L" + std::to_string(ctx.slot);
    }
}

/// The mode's records store, only from a host that keeps them (the bot manager's pool); none: no
/// records kept.
template <class Records>
std::shared_ptr<Records> recordsOf(const ServerContext& ctx)
{
    return ctx.records ? std::static_pointer_cast<Records>(ctx.records) : nullptr;
}

} // namespace nox::modes::detail
