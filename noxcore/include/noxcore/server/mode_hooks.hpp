// A NoX Core mode's say inside the server's frame (a built-in mode's, and the script modes').
// What a director can't do between ticks (a builtin's answer, an exit's touch, a death, a pause,
// a door's key, a pick-up, a hit, a cast...) is asked here, at one call site each. World::modeHooks
// is null by default: then every call site behaves exactly as without it (the game's own logic,
// 1:1). The interface is generic (no mode's rules in the core); each default answers "as the game
// does".
//
// Two kinds of hook:
// - "asked": the answer changes what the frame does (a hit cancelled, a cast refused). Asked in
//   the frame, at the moment the game decides.
// - "told": the mode hears of it. A told hook must not make or delete objects or change the
//   world (it's called from inside the frame's own walks): it records, and the mode acts on its
//   records between ticks.
// Hooks for events that come often (hits, deaths of creatures, drops) are only asked or told for
// objects that opted in (ServerObject::modeEvents), so a mode pays only for what it watches.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace nox::script {
class Vm;
}

namespace nox::server {

class HostClient;
class ScriptHost;
class Server;
class World;
struct Fight;
struct ServerObject;

/// Why the solo pause (Game_SetPaused(1)) is asked for.
enum class PauseReason : std::uint8_t {
    Other,      ///< the console's, a test's
    Timed,      ///< a timed pause (pauseFor: a solo map's load)
    LevelUp,    ///< the level-up moment
    BookFlight, ///< the host's client's book flight (an award flying to the bars)
    Chapter,    ///< a chapter screen (StartupScreen / DeathScreen), and its end
    Menu,       ///< the host's in-game menu, save menu, load menu or AutoLoad question
    Fade,       ///< the host's client's fade from black (MSG_FADE_BEGIN {1}: a map's start)
    Cinematic,  ///< a script's ImmediateUnBlind (its unpause)
};

/// Which part of a player's death is asked about.
enum class DeathStep : std::uint8_t {
    Dies,       ///< Plr_Die: a solo game's pending save dropped, and its things on the ground
    DeathState, ///< its dead state reached: the map's PlayerDeath functions run
};

/// The events an object opts into (ServerObject::modeEvents): with World::modeHooks set, the
/// hooks named are asked or told for objects with the bit, never for the rest.
namespace ModeEvent {
inline constexpr std::uint8_t Death = 0x01;     ///< objectKilled (its death, a non-player's)
inline constexpr std::uint8_t Destroyed = 0x02; ///< destroyed (its deletion swept)
inline constexpr std::uint8_t Damage = 0x04;    ///< damage asked (a hit on it)
inline constexpr std::uint8_t Damaged = 0x08;   ///< damaged told (the health it loses)
inline constexpr std::uint8_t Drop = 0x10;      ///< dropped told (an item put down)
inline constexpr std::uint8_t Use = 0x20;       ///< mayUse asked (an item used)
inline constexpr std::uint8_t Equip = 0x40;     ///< equipped told (an item put on or taken off)
} // namespace ModeEvent

struct ModeHooks {
    virtual ~ModeHooks() = default;

    /// Before a script builtin runs: a value to return in its place (the builtin's own then not
    /// run; the hook pops and pushes as the builtin would), or nullopt to run the game's own.
    /// ScriptHost::gameBuiltin runs the game's builtin from inside (for a builtin applied to
    /// several objects).
    virtual std::optional<int> builtin(ScriptHost& host, std::uint32_t id, script::Vm& vm)
    {
        (void)host;
        (void)id;
        (void)vm;
        return std::nullopt;
    }
    /// A player touching an exit that would take it (ExitCollide, past its checks): false holds
    /// it (nothing happens this touch).
    virtual bool exitTouched(Fight& fight, ServerObject& exit, ServerObject& player)
    {
        (void)fight;
        (void)exit;
        (void)player;
        return true;
    }
    /// A part of a player's death: false skips the game's part (`step`).
    virtual bool playerDeath(Fight& fight, ServerObject& player, DeathStep step)
    {
        (void)fight;
        (void)player;
        (void)step;
        return true;
    }
    /// Game_SetPaused(1) asked for `why`: false refuses it.
    virtual bool mayPause(PauseReason why)
    {
        (void)why;
        return true;
    }
    /// Game_SetPaused(0) asked for `why` (the pause the game would end): false keeps it paused.
    virtual bool mayUnpause(PauseReason why)
    {
        (void)why;
        return true;
    }
    /// The host's own client as the server should see it (server/host_client.hpp): `own` itself,
    /// or the mode's in its place (one that passes every call on to `own`, and tells others too).
    /// Asked as the host's client is set, as the hooks are, and at each map's load.
    virtual HostClient* hostClient(HostClient* own) { return own; }
    /// A map's load finished (Server::loadMapKeeping's end: the players placed, the script made,
    /// before the map's first frame).
    virtual void mapLoaded(Server& server) { (void)server; }
    /// DoorCollide's key lock when `toucher` holds no key of the door's kind (`keyType`,
    /// "SilverKey"...): another key to use (it's used up as the toucher's would be), or null.
    virtual ServerObject* sharedKey(Fight& fight, ServerObject& door, ServerObject& toucher, std::string_view keyType)
    {
        (void)fight;
        (void)door;
        (void)toucher;
        (void)keyType;
        return nullptr;
    }
    /// A player's click on an NPC to talk (MSG_DIALOG 1: past GameHost's checks, no save pending,
    /// not observing, the NPC found; before SrvNet_OnDialogStart's own): false refuses it (no
    /// dialog starts).
    virtual bool mayStartDialog(Fight& fight, ServerObject& player, ServerObject& npc)
    {
        (void)fight;
        (void)player;
        (void)npc;
        return true;
    }
    /// Something about to be picked up (Obj_TryPickup, past its own checks: a live taker with room,
    /// a thing on the ground): false leaves it there.
    virtual bool mayPickUp(World& world, ServerObject& taker, ServerObject& item)
    {
        (void)world;
        (void)taker;
        (void)item;
        return true;
    }
    /// Something picked up (Obj_TryPickup's success, its pickup handler run), from (x, y): told,
    /// after.
    virtual void pickedUp(World& world, ServerObject& taker, ServerObject& item, float x, float y)
    {
        (void)world;
        (void)taker;
        (void)item;
        (void)x;
        (void)y;
    }

    // ---- Deaths and respawns

    /// Told: a player's death scored (Plr_Die, right after the mode's scoring): its killer's top
    /// owner (the player or a monster; null: none) and the assisting player, as the stock scoring
    /// had them. Every player's, whatever its modeEvents.
    virtual void playerKilled(Fight& fight, ServerObject& victim, ServerObject* killer, ServerObject* assist)
    {
        (void)fight;
        (void)victim;
        (void)killer;
        (void)assist;
    }
    /// Told: a non-player's death (its health at 0: unitDamageClear, before its DIE runs), for
    /// those with ModeEvent::Death; `by` what hurt it last (null: nothing known).
    virtual void objectKilled(Fight& fight, ServerObject& victim, ServerObject* by)
    {
        (void)fight;
        (void)victim;
        (void)by;
    }
    /// Told: a solo game's kill experience given (awardKillXP: the player it went to, `earner`,
    /// after its own award, whatever it gained). A mode may give others theirs.
    virtual void killExperienceGiven(Fight& fight, ServerObject& victim, ServerObject& earner)
    {
        (void)fight;
        (void)victim;
        (void)earner;
    }
    /// Told: an object with ModeEvent::Destroyed swept away at the frame's end (still whole; its
    /// extent is free for reuse after this frame).
    virtual void destroyed(World& world, ServerObject& object)
    {
        (void)world;
        (void)object;
    }
    /// Asked: where a player comes back (Fight::respawn, in place of the soul gate's or
    /// Map_FindPlayerStart's spot, whose random draw is then not made); none: the game's.
    virtual std::optional<std::pair<float, float>> respawnPoint(Fight& fight, ServerObject& player)
    {
        (void)fight;
        (void)player;
        return std::nullopt;
    }

    // ---- Spells

    /// Asked: whether `caster` may cast `spell` (after every check of the game's has let it:
    /// checkPlrCantCastSpell's end): 0 it may, else the MSG_INFORM reason the player is told
    /// (0xD a flag carried, 0xE anti-magic...). Asked again at each step that checks (the start,
    /// the finish, a trap's building), so it must answer the same for the same state.
    virtual int cantCast(const World& world, const ServerObject& caster, std::uint32_t spell, bool trap)
    {
        (void)world;
        (void)caster;
        (void)spell;
        (void)trap;
        return 0;
    }
    /// Told: a player's spell cast (Plr_FinishSpellCast's success: its mana spent), at `target`
    /// (null: none) and the cursor (x, y). `made`: what the cast put on the world's new list (its
    /// missiles, a summoned creature..., and anything its kills made at once; not what comes on
    /// later frames), oldest first; valid only during the call.
    virtual void spellCast(World& world, ServerObject& caster, std::uint32_t spell, ServerObject* target, float x, float y,
                           std::span<const std::uint32_t> made)
    {
        (void)world;
        (void)caster;
        (void)spell;
        (void)target;
        (void)x;
        (void)y;
        (void)made;
    }

    // ---- Contacts, hits and items

    /// Asked, at once: an object whose COLLIDE function is "ModeCollide" (set by the mode) touching
    /// `other` (null: a wall). The game runs no COLLIDE of its own for it.
    virtual void collide(Fight& fight, ServerObject& object, ServerObject* other)
    {
        (void)fight;
        (void)object;
        (void)other;
    }
    /// Asked: a hit on `target` (with ModeEvent::Damage) before anything of the game's (its
    /// DAMAGE function, armour...): `amount` may be changed; false cancels the hit (the damage
    /// call returns 0, nothing done).
    virtual bool damage(Fight& fight, ServerObject& target, ServerObject* source, ServerObject* weapon, int& amount, int type)
    {
        (void)fight;
        (void)target;
        (void)source;
        (void)weapon;
        (void)amount;
        (void)type;
        return true;
    }
    /// Told: `target` (with ModeEvent::Damaged) about to lose `amount` health (unitDamageClear:
    /// after armour and the rest, at most what it has).
    virtual void damaged(Fight& fight, ServerObject& target, int amount)
    {
        (void)fight;
        (void)target;
        (void)amount;
    }
    /// Told: an item with ModeEvent::Drop put down (Obj_DropAt's success, by hand or at a death).
    virtual void dropped(World& world, ServerObject& holder, ServerObject& item)
    {
        (void)world;
        (void)holder;
        (void)item;
    }
    /// Asked: an item with ModeEvent::Use used (Obj_UseItem): false does nothing (the mode may
    /// have done its own).
    virtual bool mayUse(World& world, ServerObject& user, ServerObject& item)
    {
        (void)world;
        (void)user;
        (void)item;
        return true;
    }
    /// Told: a player's item with ModeEvent::Equip put on (`worn`: Obj_PlayerEquipWeapon's or
    /// Obj_PlayerEquipArmor's success, at its end) or taken off (playerDequipWeapon or the armour's
    /// dequip, at its end: by hand, for another, or knocked off by a two-hander or a shield).
    virtual void equipped(World& world, ServerObject& player, ServerObject& item, bool worn)
    {
        (void)world;
        (void)player;
        (void)item;
        (void)worn;
    }

    // ---- The map's script

    /// Asked: a script handler the mode took (ServerObject::ScriptBlock::mode: a monster's, a
    /// trigger's, a polygon's, a pickup's...) set off, in place of the map's NoxScript function:
    /// its result (what the handler returns to the game), none for nothing.
    virtual std::optional<std::uint32_t> scriptBlock(ScriptHost& host, std::string_view function, std::uint32_t caller, std::uint32_t trigger)
    {
        (void)host;
        (void)function;
        (void)caller;
        (void)trigger;
        return std::nullopt;
    }
};

} // namespace nox::server
