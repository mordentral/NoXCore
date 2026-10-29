// nox.weather: the weather each NoX Core player's client is told
// to draw (ui_proto.hpp's kWeather; weather.hpp), kept per player only once the mode has called
// it. Sent when it changes (a player's at most once a frame), again when a player's map is loaded
// (its join and each map change: CLIENT_READY, the client having cleared its own), and none to
// everyone it was sent to when the mode unloads or reloads. Only a client at client level 1 or
// above that named ModeUi is ever sent one (reaches); stock clients, bots and held-out players never
// get a byte. A mode that never calls nox.weather keeps nothing and sends nothing. Nothing in the
// game changes. (Internal to the script modes' sources.)
#pragma once

#include "noxcore/modes/script/weather.hpp"

#include <cstdint>
#include <map>
#include <optional>

struct lua_State;

namespace nox::server {
class GameHost;
}

namespace nox::modes::script {

class Weather {
public:
    // (A player with no reliable room, GameHost::reliableRoom: its weather waits till its
    // acknowledgements catch up; the game's own messages keep their place.)

    /// Whether `peer` may be sent the weather: a player in the game, not a held-out stock one,
    /// whose client named ClientFeature::ModeUi and is at client level 1 or above (asked each
    /// time: a peer let in may still read 0).
    static bool reaches(const server::GameHost& host, std::uint8_t peer);

    /// Whether the mode has called nox.weather since it was loaded (else nothing here does a thing).
    bool used() const noexcept { return used_; }
    /// nox.weather's: `peer`'s weather (none: everyone's, and of everyone who comes later), reached
    /// over `fade` frames. `host` null: before the game, kept for whoever comes.
    void set(const server::GameHost* host, std::optional<std::uint8_t> peer, const weather::State& state, std::uint16_t fade);
    /// A player's map loaded (CLIENT_READY): its client cleared its weather, so its own is sent
    /// again, at once (no fade).
    void ready(const server::GameHost& host, std::uint8_t peer);
    /// A player gone.
    void left(std::uint8_t peer) { peers_.erase(peer); }
    /// Once a frame: each player's weather that changed sent.
    void flush(server::GameHost& host);
    /// The mode unloaded or reloaded: none sent at once to every player it was ever sent to,
    /// everything forgotten (unused again). `host` null: forgotten only.
    void reset(server::GameHost* host);

private:
    struct Peer {
        weather::State want;   ///< what the mode says it has
        std::uint16_t fade = 0; ///< over how many frames it's reached
        weather::State has;    ///< what its client has as far as we know (none at first, and after a map's load)
        bool instant = false;  ///< sent again after a map's load: no fade
        bool sentAny = false;  ///< sent one at least (reset tells it none)
    };
    Peer& peer(std::uint8_t p);

    bool used_ = false;
    bool dirty_ = false;     ///< a player's may differ from what its client has
    weather::State all_;     ///< nox.weather(nil, ...)'s last: for players who come later
    std::uint16_t allFade_ = 0;
    std::map<std::uint8_t, Peer> peers_;
};

/// nox.weather set in the `nox` table on top of L's stack.
void addWeatherToolbox(lua_State* L);

} // namespace nox::modes::script
