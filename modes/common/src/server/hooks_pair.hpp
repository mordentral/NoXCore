// Two modes' say inside the server's frame (mode_hooks.hpp) as one: a native mode's (a director's)
// and the script it runs beside it (a mod's ScriptedMode::engineHooks, not installed by itself). The
// server holds one ModeHooks; this one asks both, each hook by the rule that keeps each mode's own
// behaviour when the other has nothing to say (every default answers "as the game does"):
//   - asked yes / no (an exit's touch, a death's part, a pause, a talk, a pickup, a use, a hit): the
//     script first, then the native mode, a no from either the answer (the script's no keeps the
//     native mode from starting what it would on a yes: a gathering at an exit);
//   - asked for an answer (a builtin's value, a respawn point, a cast's price or refusal, a script
//     block, a map's LuauCall, a shared key, a player's text): the native mode's, else the
//     script's;
//   - told: the native mode, then the script;
//   - the host's client: the script's view of the native mode's.
// Every ModeHooks hook is forwarded here: a hook added to ModeHooks is added here too.
// (Internal to noxmodes' server sources.)
#pragma once

#include "noxcore/server/mode_hooks.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace nox::server::detail {

class HooksPair final : public ModeHooks {
public:
    HooksPair(ModeHooks& native, ModeHooks& script) : native_(native), script_(script) {}

    std::optional<int> builtin(ScriptHost& host, std::uint32_t id, script::Vm& vm) override
    {
        if (const auto r = native_.builtin(host, id, vm)) {
            return r;
        }
        return script_.builtin(host, id, vm);
    }
    bool exitTouched(Fight& fight, ServerObject& exit, ServerObject& player) override
    {
        return script_.exitTouched(fight, exit, player) && native_.exitTouched(fight, exit, player);
    }
    bool playerDeath(Fight& fight, ServerObject& player, DeathStep step) override
    {
        return script_.playerDeath(fight, player, step) && native_.playerDeath(fight, player, step);
    }
    bool mayPause(PauseReason why) override { return script_.mayPause(why) && native_.mayPause(why); }
    bool mayUnpause(PauseReason why) override { return script_.mayUnpause(why) && native_.mayUnpause(why); }
    HostClient* hostClient(HostClient* own) override { return script_.hostClient(native_.hostClient(own)); }
    void mapLoaded(Server& server) override
    {
        native_.mapLoaded(server);
        script_.mapLoaded(server);
    }
    ServerObject* sharedKey(Fight& fight, ServerObject& door, ServerObject& toucher, std::string_view keyType) override
    {
        if (ServerObject* k = native_.sharedKey(fight, door, toucher, keyType)) {
            return k;
        }
        return script_.sharedKey(fight, door, toucher, keyType);
    }
    bool mayStartDialog(Fight& fight, ServerObject& player, ServerObject& npc) override
    {
        return script_.mayStartDialog(fight, player, npc) && native_.mayStartDialog(fight, player, npc);
    }
    std::optional<std::u16string> playerText(const World& world, const ServerObject& player, std::string_view label) override
    {
        if (auto t = native_.playerText(world, player, label)) {
            return t;
        }
        return script_.playerText(world, player, label);
    }
    bool mayPickUp(World& world, ServerObject& taker, ServerObject& item) override
    {
        return script_.mayPickUp(world, taker, item) && native_.mayPickUp(world, taker, item);
    }
    void pickedUp(World& world, ServerObject& taker, ServerObject& item, float x, float y) override
    {
        native_.pickedUp(world, taker, item, x, y);
        script_.pickedUp(world, taker, item, x, y);
    }
    void playerKilled(Fight& fight, ServerObject& victim, ServerObject* killer, ServerObject* assist) override
    {
        native_.playerKilled(fight, victim, killer, assist);
        script_.playerKilled(fight, victim, killer, assist);
    }
    void objectKilled(Fight& fight, ServerObject& victim, ServerObject* by) override
    {
        native_.objectKilled(fight, victim, by);
        script_.objectKilled(fight, victim, by);
    }
    void killExperienceGiven(Fight& fight, ServerObject& victim, ServerObject& earner) override
    {
        native_.killExperienceGiven(fight, victim, earner);
        script_.killExperienceGiven(fight, victim, earner);
    }
    void destroyed(World& world, ServerObject& object) override
    {
        native_.destroyed(world, object);
        script_.destroyed(world, object);
    }
    std::optional<std::pair<float, float>> respawnPoint(Fight& fight, ServerObject& player) override
    {
        if (const auto at = native_.respawnPoint(fight, player)) {
            return at;
        }
        return script_.respawnPoint(fight, player);
    }
    int cantCast(const World& world, const ServerObject& caster, std::uint32_t spell, bool trap) override
    {
        if (const int why = native_.cantCast(world, caster, spell, trap)) {
            return why;
        }
        return script_.cantCast(world, caster, spell, trap);
    }
    void spellCast(World& world, ServerObject& caster, std::uint32_t spell, ServerObject* target, float x, float y,
                   std::span<const std::uint32_t> made) override
    {
        native_.spellCast(world, caster, spell, target, x, y, made);
        script_.spellCast(world, caster, spell, target, x, y, made);
    }
    void collide(Fight& fight, ServerObject& object, ServerObject* other) override
    {
        native_.collide(fight, object, other);
        script_.collide(fight, object, other);
    }
    bool damage(Fight& fight, ServerObject& target, ServerObject* source, ServerObject* weapon, int& amount, int type, HitCause cause,
                int& wearAs) override
    {
        return script_.damage(fight, target, source, weapon, amount, type, cause, wearAs) &&
               native_.damage(fight, target, source, weapon, amount, type, cause, wearAs);
    }
    bool reflect(Fight& fight, ServerObject& target, ServerObject& missile, bool front) override
    {
        return native_.reflect(fight, target, missile, front) || script_.reflect(fight, target, missile, front);
    }
    void strike(Fight& fight, ServerObject& victim, ServerObject* attacker, ServerObject& weapon) override
    {
        native_.strike(fight, victim, attacker, weapon);
        script_.strike(fight, victim, attacker, weapon);
    }
    std::optional<CastPrice> castPrice(World& world, const ServerObject& caster, std::uint32_t spell, int mana, bool trap) override
    {
        if (const auto p = native_.castPrice(world, caster, spell, mana, trap)) {
            return p;
        }
        return script_.castPrice(world, caster, spell, mana, trap);
    }
    void damaged(Fight& fight, ServerObject& target, int amount) override
    {
        native_.damaged(fight, target, amount);
        script_.damaged(fight, target, amount);
    }
    void dropped(World& world, ServerObject& holder, ServerObject& item) override
    {
        native_.dropped(world, holder, item);
        script_.dropped(world, holder, item);
    }
    bool mayUse(World& world, ServerObject& user, ServerObject& item) override
    {
        return script_.mayUse(world, user, item) && native_.mayUse(world, user, item);
    }
    void equipped(World& world, ServerObject& player, ServerObject& item, bool worn) override
    {
        native_.equipped(world, player, item, worn);
        script_.equipped(world, player, item, worn);
    }
    void wornRead(World& world, ServerObject& player, std::span<const std::uint32_t> worn) override
    {
        native_.wornRead(world, player, worn);
        script_.wornRead(world, player, worn);
    }
    void abilityNews(World& world, ServerObject& unit, const AbilityNews& news) override
    {
        native_.abilityNews(world, unit, news);
        script_.abilityNews(world, unit, news);
    }
    std::optional<std::uint32_t> scriptBlock(ScriptHost& host, std::string_view function, std::uint32_t caller, std::uint32_t trigger) override
    {
        if (const auto r = native_.scriptBlock(host, function, caller, trigger)) {
            return r;
        }
        return script_.scriptBlock(host, function, caller, trigger);
    }
    std::optional<std::uint32_t> mapCall(ScriptHost& host, const MapCall& call) override
    {
        if (const auto r = native_.mapCall(host, call)) {
            return r;
        }
        return script_.mapCall(host, call);
    }

private:
    ModeHooks& native_;
    ModeHooks& script_;
};

} // namespace nox::server::detail
