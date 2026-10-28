// A NoX Core mode run alongside a hosted game (a mode's ServerMode: modes/mode.hpp), as whoever
// hosts sees it: it sets the host's options up before the game is made, is given the game once
// made, is ticked between the game's ticks on the server's thread, and is let go before the game
// goes. The host (noxsdl's LocalHost, a test) knows only this; the mode's rules are its own.
#pragma once

#include "noxcore/server/game_host.hpp"

#include <cstdint>
#include <filesystem>

namespace nox::server {

class HostedMode {
public:
    virtual ~HostedMode() = default;
    /// The host's options made the mode's (before the GameHost is made); the map to host, or empty
    /// to keep the host's own.
    virtual std::filesystem::path configure(GameHost::Options& o) = 0;
    /// The game made: the mode's hooks on it.
    virtual void attach(GameHost& host) = 0;
    /// Between the game's ticks (the server's thread).
    virtual void tick(GameHost& host, std::uint32_t nowMs) = 0;
    /// The game about to go: the mode's hooks off it.
    virtual void detach(GameHost& host) { (void)host; }
};

} // namespace nox::server
