// nox.setBusy, nox.busy and nox.leap. Busy: a unit kept from acting for some frames, till it dies
// (World::modeBusy, which the engine's player input, spells, abilities and creature AI look at;
// a dead or gone unit's entry let go each frame, sweepBusy). A leap: the unit carried along a
// straight line over its frames by the mode (Leaps::step, once a frame, after the world's frame),
// never through a wall (the line traced at its start: a wall or ground no one stands on cuts it
// short), still the while (busy, its moves and turns refused, a creature's AI paused; bodies
// neither push it nor are pushed by it, World::passesBodies, the physics' push alone: it's met,
// struck, hit and found as ever, no flag of the game's changed); NoX Core clients of client level
// 8 that named ModeUi draw it raised (ui_proto.hpp's kLeap, one message a leap), every other sees
// it move along the ground. Down, the mode's onLand(unit, x, y, cut) runs. Nothing on the server
// reads a height. (Internal to the script modes' sources.)
#pragma once

#include "noxcore/server/object_key.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

struct lua_State;

namespace nox::server {
class GameHost;
class World;
struct ServerObject;
} // namespace nox::server

namespace nox::modes::script {

class Leaps {
    /// (script_links.hpp: a leap names its unit.)
    template <class>
    friend struct server::Links;

public:
    /// The most leaps under way at once (more: nox.leap gives nil).
    static constexpr std::size_t kMax = 64;
    /// The step a leap's line is traced in (world units).
    static constexpr float kTraceStep = 8;

    struct Landing {
        server::ObjectKey unit = 0;
        float x = 0, y = 0;
        bool cut = false; ///< a wall or unstandable ground stopped it short of where it was aimed
    };
    struct Path {
        float x = 0, y = 0; ///< where it comes down
        bool cut = false;
    };

    /// Where `unit` comes down leaping at (x, y): the line traced from it in kTraceStep steps
    /// (walls and closed doors stop it, windows don't, as a jump goes over them; each step on
    /// ground one stands on), the last step its body fits at (no wall within its radius).
    static Path pathOf(const server::World& world, const server::ServerObject& unit, float x, float y);

    /// A leap begun: `unit` (a creature or a player, alive, not held, frozen, jumping, charging or
    /// leaping) carried to pathOf's spot over `frames`, `height` high for the clients; still till
    /// it's down. None when it can't (or kMax are under way). `host` null: no client told.
    std::optional<Path> start(server::World& world, server::GameHost* host, server::ServerObject& unit, float x, float y, int frames,
                              int height);
    bool active() const noexcept { return !leaps_.empty(); }
    /// Once a frame (after the world's): each leap's unit put at its share of the way; those down
    /// (no longer still) returned in order. One whose unit is gone or dead is let go, no landing.
    std::vector<Landing> step(server::World& world);
    /// Every leap let go where its unit is, its stillness ended (the mode's end). `world` null:
    /// forgotten only (a map's load: the new World's busy units are none, and nothing of the
    /// game's was changed on the unit).
    void reset(server::World* world);

private:
    struct Leap {
        server::ObjectRef unit;
        float fromX = 0, fromY = 0, toX = 0, toY = 0;
        std::uint32_t start = 0; ///< the frame it began after
        int frames = 1;
        bool cut = false;
    };
    /// `unit`'s stillness ended (its busy entry let go when nothing's left of it).
    static void end(server::World& world, server::ObjectKey unit);

    std::vector<Leap> leaps_;
};

/// nox.setSpin's spins under way (ui_proto.hpp's kSpin): each told to the level 8 ModeUi clients
/// that know its thing when it's set, and (once a frame, step) to one that comes to know it later,
/// with its frames left; a spin past its frames, or whose thing is gone, let go.
class Spins {
    /// (script_links.hpp: a spin names its thing.)
    template <class>
    friend struct server::Links;

public:
    static constexpr std::size_t kMax = 64;
    /// `o` spun `rate16` (turns a second x 16) for `frames` (either 0: stopped, every client that
    /// knows it told so); past kMax a new one isn't kept (it's still sent now).
    void set(server::GameHost* host, server::World& world, const server::ServerObject& o, std::int16_t rate16, int frames);
    /// Once a frame while active: the spent and the gone let go, the ones a client has come to
    /// know since told to it.
    void step(server::GameHost& host);
    bool active() const noexcept { return !spins_.empty(); }
    void reset() { spins_.clear(); }

private:
    struct Spin {
        server::ObjectRef unit;
        std::int16_t rate16 = 0;
        std::uint32_t until = 0; ///< (frameNow at its end)
        std::uint32_t told = 0;  ///< the players' slots told (HostedPlayer::slot's bits)
    };
    /// `s` to each client it reaches that knows its thing and hasn't been told.
    static void tellNew(server::GameHost& host, server::World& world, Spin& s);

    std::vector<Spin> spins_;
};

/// `unit` busy for `frames` more (0: no longer; a leap's stillness stays); false when
/// World::kMaxModeBusy units are busy. The world's spent entries are let go on the way.
bool setBusy(server::World& world, const server::ServerObject& unit, int frames);
/// The world's busy entries spent (past their frames) or whose unit is dead or gone let go.
void sweepBusy(server::World& world);

/// nox.setBusy, nox.busy, nox.leap set in the `nox` table on top of L's stack.
void addLeapToolbox(lua_State* L);

} // namespace nox::modes::script
