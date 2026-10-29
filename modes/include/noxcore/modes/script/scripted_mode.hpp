// A script mode's server side: its folder's server script
// run in a sandboxed Luau VM (luau_vm.hpp) on the server's thread, as a ServerMode any host makes
// from the registry. The script returns a table of the hooks it wants; a hook it leaves out is
// never called, and the host's options get only the callbacks its hooks need:
//
//   local mode = {}
//   function mode.configure(ctx) return { map = "Estate" } end -- (ctx.options, ctx.seed, ctx.slot)
//   function mode.start() end                                   -- the game made
//   function mode.onJoin(player) end                            -- player: { peer, name, identified }
//   function mode.onLeave(player) end
//   function mode.onChat(player, text) return false end         -- true: taken, not relayed
//   function mode.onChoice(player, index) end                   -- an answer to nox.choice
//   function mode.tick(frame) end                               -- once per game frame
//   function mode.console(line) return { "reply" } end          -- a host console line it takes
//   function mode.save() return state end                       -- a live reload's state, kept...
//   function mode.restore(state) end                            -- ...and handed to the new code
//   function mode.stop() end                                    -- the game about to go
//   function mode.onUiMessage(player, value) end                -- what its UI sent (nox.send there)
//   -- the engine's, inside the frame (ids: things in the world, a player's unit among them):
//   function mode.mapLoaded(name) end                           -- each map's load (its name)
//   function mode.onKill(victim, killer, assist) end            -- a player's death (nil: none)
//   function mode.respawnPoint(unit) return x, y end            -- nil: the game's start
//   function mode.collide(thing, other) end                     -- a thing it watches touched
//   return mode
//
// The toolbox (the global `nox`, read-only): nox.log, nox.players, nox.player, nox.say, nox.sayAll,
// nox.choice, nox.table, nox.option, nox.frame, nox.random, nox.f32, nox.send, nox.hasUi;
// the world's (object, objects, units, inventory, watchCollide, take, drop, place,
// clearPointNear, endSpellEffects, minimap, sound), the engine's paths (heal, cast, enchant,
// enchanted, damage, unitsNear, countNear, effect, spellSound, create, floorAt, wallAt, cosf, sinf,
// monsterInfo, setHealth, touch, sightRange, textToAll; nearestFoes, applyForce, attack,
// direction, directionOf, directionVector, newMark, madeSince, spellInfo, setCharmable, enchantTime, gameNumber,
// traceRay, shoot, summon, abilitySound, setPosition, setVelocity, setOwner, setTeam, setNoCollide,
// damageAt, delete: each the engine's own function, its input checked; setHitRadius, setBurstScale:
// one missile's own size (ServerObject::ModSize); thingType, present, living,
// setCollide, backIntoPlay, nearSoulGate; and batches that save a frame's calls: position,
// keepPresent, keepLiving, circle, damageAtEach; tell: a line to one player; a player's own numbers
// and book: gold, setGold, mana, setMana, setPlayerHealth, strength, setStrength, spellLevel,
// spells, raiseSpell, grantSpell, abilityLevel, setAbility, fieldGuide, clearBook, revoke,
// sendRevokes; its quest fields: status, gameOverAt, setGameOverAt, exitGate; the quest's rewards:
// rewards, rewardAt; setOnOff, unitSay; the game: setServerName, questStage, exitTo, lock, unlock,
// allow; isEnemy, chests, giveItem, createTeam, joinTeam, removeTeam, gameplay; soundAt, gameRandom: the
// server's game stream), seeded streams
// (mt19937 and its
// stream:next(), shuffle as the C++'s util::shuffle, mul32), teams and scores (teams, teamScore,
// changeLessons, addDeath, anyonePlaying, limits) and the stock messages (inform, informString,
// flagStatus, timer, endMatch, gameOver): what an arena or flag mode needs. And
// `require("name")` for the folder's other scripts (server/name.luau). What a script tells
// players goes through the views (say, choice, table): a player whose client runs the mode's UI
// and draws views (mode.json's "ui": script_ui.hpp) gets them as data, everyone else as text
// (MSG_TEXT_MESSAGE lines, 250 characters at most). A view whose data is over a message's size
// goes as text too. The UI is sent only after the player's map loads, so what onJoin says goes as
// text: say a UI's views once nox.hasUi(peer) is true. A UI of API 2 (mode.json's "uiApi": 2) goes
// only to clients that name ClientFeature::ModeUi2, the mod's assets\ (mod_assets.hpp) sent before
// it; any other client gets the views as text, the UI never sent. A UI of API 3 goes only to those
// clients at client level 3 or above (ui_proto.hpp's kUiApi3Level). A mode.json with "stockClients":
// false turns stock GAME.EXE players away with a notice (GameHost::Options::stockRefusal).
//
// Reload: "mode reload" on the host's console runs the scripts afresh (their state lost),
// "mode reload keep" hands save()'s table to restore(); and between games, with nobody in, a
// changed script reloads by itself. A reload sends the UI again when it changed, and its assets
// again only when they changed.
#pragma once

#include "noxcore/modes/mode.hpp"
#include "noxcore/modes/script/luau_vm.hpp"
#include "noxcore/modes/script/manifest.hpp"
#include "noxcore/modes/script/mod_assets.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace nox::server {
struct ModeHooks;
}

namespace nox::modes::script {

/// A script mode's files as the host has them: a folder (mods\<id>\), or a built-in mode's
/// scripts compiled into the program (`source`).
struct ModPackage {
    Manifest manifest;
    std::filesystem::path folder;
    /// A built-in mode's files (by path, as read takes it), in place of the folder's: never
    /// reloaded by their time.
    std::function<std::optional<std::string>(std::string_view relPath)> source;
    /// One of its files by its path in the folder ('/' separated, nothing outside it), less a
    /// leading UTF-8 mark; none: not there, or bigger than 1 MB.
    std::optional<std::string> read(std::string_view relPath) const;
    /// The newest write time of its .luau files and mode.json, and of its assets\ (a reload's
    /// test; a built-in mode's: never newer).
    std::filesystem::file_time_type newest() const;
};

class ScriptedMode final : public ServerMode {
public:
    ScriptedMode(std::shared_ptr<const ModPackage> package, ServerContext ctx, LuauVm::Limits limits = {});
    ~ScriptedMode() override;

    /// The scripts run and their configure called; the map they name (none: the host's). Throws
    /// std::runtime_error with the script's error when they fail (the game isn't hosted).
    std::filesystem::path configure(server::GameHost::Options& o) override;
    void attach(server::GameHost& host) override;
    void tick(server::GameHost& host, std::uint32_t nowMs) override;
    void detach(server::GameHost& host) override;
    std::optional<std::vector<std::string>> console(server::GameHost& host, std::string_view line) override;
    /// Its generated maps (mode.json's mapPrefix, this slot's) deleted; how many went.
    int sweepOwn() override;
    /// Its builds under way let go (waited for at the program's end: Mode::finishBackground).
    void releaseBackground() override;

    /// The scripts read again and run afresh; `keepState`: save()'s table handed to restore().
    /// False: the error (the old code keeps running).
    bool reload(bool keepState, std::string& error);
    /// The script errors so far, the latest last (each line once in a while: a failing tick
    /// doesn't flood the log).
    const std::vector<std::string>& errors() const noexcept;

    // ---- owned by a native mode part-way into script:
    // the director stays the ServerMode, calls these, and forwards the host's calls above.

    /// Before attach: false leaves the server's engine hooks (ModeHooks) to the owner, which
    /// forwards to engineHooks() the ones the script should hear. (Default: installed.)
    void setInstallEngineHooks(bool install) noexcept;
    /// How nox.f32 rounds: towards zero (true) or to nearest (false), whatever the host. None (the
    /// default): the host's (its exact maths: towards zero), and towards zero with no host. For a
    /// script that runs where the C++ it replaced rounded otherwise (a level built on a worker, to
    /// nearest).
    void setF32Chop(std::optional<bool> chop) noexcept;
    /// The script's engine hooks (onKill, respawnPoint, collide, mapLoaded) while attached;
    /// null before.
    server::ModeHooks* engineHooks() noexcept;
    /// The script's mode-table function `name` called: `pushArgs` pushes its arguments and says
    /// how many, `readResults` takes its `nresults` results off the stack (and pops them). False:
    /// it has no such function, or it failed (reported; readResults not called). Not per frame
    /// for everything: a piece's call, as the C++ it replaced was.
    bool call(std::string_view name, int nresults, const std::function<int(lua_State*)>& pushArgs,
              const std::function<void(lua_State*)>& readResults);
    /// A rule looked up once by name (and again after each reload), for calls every frame: no
    /// lookup by name per call.
    struct Rule {
        int index = -1;
    };
    Rule rule(std::string_view name);
    /// Whether the rule is a function in the script as it's loaded now (false: none of that name,
    /// or no script yet; after a reload, the new code's answer): a set-up's check for a missing
    /// one.
    bool has(Rule rule) const;
    bool call(Rule rule, int nresults, const std::function<int(lua_State*)>& pushArgs,
              const std::function<void(lua_State*)>& readResults);
    /// Before configure: a function of the owner's in the script's `owner` table
    /// (owner.name(...)), for the owner's own state its rules still need. It returns how many
    /// results it pushed; a Lua error from it is the calling rule's.
    void addOwnerFunction(std::string name, std::function<int(lua_State*)> fn);

    struct Impl; ///< (scripted_mode.cpp's: the VM, the hooks, the toolbox's state)

private:
    bool reloadUnguarded(bool keepState, std::string& error);
    std::optional<std::vector<std::string>> consoleLine(std::string_view line);

    std::unique_ptr<Impl> impl_;
};

} // namespace nox::modes::script
