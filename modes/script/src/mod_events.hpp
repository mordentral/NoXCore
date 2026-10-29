// The engine's events, to a script's hooks. The told ones
// (onObjectKilled, onDestroyed, onDamaged, onPickup, onDrop, onEquip, onSpellCast) are recorded
// as plain facts when the engine tells them (a told hook mustn't change the world: mode_hooks.hpp)
// and delivered in order after the frame's update, before tick(frame), in the same frame. The
// asked ones (onDamage, onExit) run at once; inside them the toolbox's world-changing functions
// are refused (Parts::asked). A hook the script doesn't have is never recorded or called.
// (Internal to the script modes' sources.)
#pragma once

#include "noxcore/server/world.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

struct lua_State;

namespace nox::modes::script {

class Events {
public:
    /// Events recorded a frame at most (more are counted and reported, not delivered).
    static constexpr std::size_t kPerFrame = 4096;

    struct Has {
        bool onObjectKilled = false, onDestroyed = false, onDamage = false, onDamaged = false, onPickup = false,
             onDrop = false, onEquip = false, onSpellCast = false, onExit = false;
    };
    /// Which of them the script's mode table (on top of L's stack) has (each load).
    void look(lua_State* L);
    const Has& has() const noexcept { return has_; }

    // ---- told: recorded (the caller has left out held-out players' units)
    void killed(const server::ServerObject& victim, const server::ServerObject* by);
    void destroyed(const server::ServerObject& o);
    void damaged(const server::ServerObject& target, int amount);
    void pickedUp(const server::ServerObject& taker, const server::ServerObject& item, float x, float y);
    void dropped(const server::ServerObject& holder, const server::ServerObject& item);
    void equipped(const server::ServerObject& unit, const server::ServerObject& item, bool worn);
    /// `made`: what the cast made (onSpellCast's sixth argument; kept only when the hook is there).
    void spellCast(const server::ServerObject& caster, std::uint32_t spell, const server::ServerObject* target, float x, float y,
                   std::span<const std::uint32_t> made);

    // ---- asked: the script's answer, at once
    /// onDamage(target, source, weapon, amount, kind) -> amount (nil: kept; 0: the hit cancelled).
    /// False: cancel it.
    bool damage(lua_State* L, const server::ServerObject& target, const server::ServerObject* source,
                const server::ServerObject* weapon, int& amount, int type);
    /// onExit(exit, player) -> false holds the player at the exit.
    bool exitTouched(lua_State* L, const server::ServerObject& exit, const server::ServerObject& player);

    /// The recorded events to the script's hooks, in order (each its own protected call).
    void deliver(lua_State* L);
    /// The recorded events dropped (the game's end, a reload).
    void clear() noexcept;
    /// Nothing recorded (a frame's delivery has nothing to do).
    bool idle() const noexcept { return queue_.empty() && overflow_ == 0; }

private:
    enum class Kind : std::uint8_t { Killed, Destroyed, Damaged, PickedUp, Dropped, Equipped, SpellCast };
    struct Event {
        Kind kind;
        std::uint32_t a = 0, b = 0; ///< the ids it names (0: none)
        float x = 0, y = 0;
        std::int32_t n = 0;          ///< an amount, a spell, worn
        server::Name type;           ///< (a death's or a deletion's thing type)
        std::uint32_t madeFrom = 0, madeCount = 0; ///< a cast's: what it made, in made_
    };
    bool room();
    /// An asked hook's call with Parts::asked set (the world's changes refused in it).
    bool ask(lua_State* L, const char* hook, int nargs, int nresults);

    Has has_;
    std::vector<Event> queue_;
    std::vector<std::uint32_t> made_; ///< the queued casts' made ids, each a run of them
    std::size_t overflow_ = 0;
};

} // namespace nox::modes::script
