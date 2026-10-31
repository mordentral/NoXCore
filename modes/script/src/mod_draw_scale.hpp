// nox.setDrawScale: a thing drawn larger or smaller by NoX Core clients (ui_proto.hpp's kDrawScale).
// The scale lives on the thing (ServerObject::ModSize's drawScale, out of line; nothing on the
// server reads it). A client is told when the scale is set, if it knows the thing then, and when
// it's first sent the thing (GameHost::Options::onScaledSeen: its record new to it, after it came
// into view or its client forgot it); never per frame. Only a client at client level 5 or above
// that named ModeUi is ever sent one (reaches); stock clients, older NoX Core clients, bots and
// held-out players never get a byte and draw the thing as its type. A mode that never calls
// nox.setDrawScale keeps and sends nothing. (Internal to the script modes' sources.)
#pragma once

#include "noxcore/server/object_key.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>

struct lua_State;

namespace nox::server {
class GameHost;
class World;
struct ServerObject;
} // namespace nox::server

namespace nox::modes::script {

class DrawScales {
    /// (script_links.hpp: what's owed names things.)
    template <class>
    friend struct server::Links;

public:
    /// The most messages kept owed (a player with no reliable room: GameHost::reliableRoom); past
    /// it a new one is let go (its client draws that thing as its type).
    static constexpr std::size_t kMaxOwed = 1024;

    /// Whether `peer` may be sent a draw scale: a player in the game, not a held-out stock one,
    /// whose client named ClientFeature::ModeUi and is at uimsg::kDrawScaleLevel or above.
    static bool reaches(const server::GameHost& host, std::uint8_t peer);

    /// Whether the mode has called nox.setDrawScale since it was loaded.
    bool used() const noexcept { return used_; }
    /// Whether messages wait for room.
    bool owing() const noexcept { return !owed_.empty(); }
    /// nox.setDrawScale's: `o`'s scale set (1: let go), each player whose client knows `o` now
    /// told. `host` null: kept on the thing only.
    void set(server::GameHost* host, server::ServerObject& o, float scale);
    /// The host's onScaledSeen: `peer`'s client was just sent `o` (it has a draw scale).
    void seen(server::GameHost& host, std::uint8_t peer, const server::ServerObject& o);
    /// Once a frame while owing: what's owed sent as room allows (a thing gone, or no longer known
    /// to the player, let go; a thing's scale as it is now).
    void flush(server::GameHost& host);
    /// A player gone: what it was owed let go.
    void left(std::uint8_t peer);
    /// The mode unloaded or reloaded: everything forgotten (the things keep their scales; a new
    /// map's things are new).
    void reset() { *this = DrawScales{}; }

private:
    struct Owed {
        std::uint8_t peer = 0;
        server::ObjectRef extent;
        std::uint16_t type = 0;
    };
    /// `o`'s scale sent to `peer` now, or owed when it has no room.
    void tell(server::GameHost& host, std::uint8_t peer, const server::ServerObject& o);

    bool used_ = false;
    std::deque<Owed> owed_;
};

/// nox.setDrawScale set in the `nox` table on top of L's stack.
void addDrawScaleToolbox(lua_State* L);

} // namespace nox::modes::script
