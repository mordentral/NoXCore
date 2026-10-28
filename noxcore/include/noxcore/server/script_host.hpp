// The map script's builtins on the server's world (script::Host), and its timers.
//
// Every builtin pops exactly its arguments and pushes its result as the game's does (the table
// in script_host.cpp, counted from each builtin in GAME.EXE). All 211 of the game's are
// implemented; a fallback (pop, push 0, counted in unimplemented()) is kept for safety.
// What the scripts do that a client would see (prints, chat, sounds, effects, music) comes out as
// events for the network side to send.
#pragma once

#include "noxcore/map/sections.hpp"
#include "noxcore/script/script_file.hpp"
#include "noxcore/script/vm.hpp"
#include "noxcore/server/world.hpp"

#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace nox::server {

struct ScriptEvent {
    enum class Kind : std::uint8_t {
        Print,        ///< text (a string-table id) to the caller, a player
        PrintToAll,   ///< text to everyone
        Chat,         ///< object says text (a bubble)
        DestroyChat,  ///< object's bubble goes; object 0: every bubble
        Audio,        ///< sound text at (x, y)
        ObjectSound,  ///< sound number value from object
        WallSound,    ///< text is "open" or "close", at (x, y), for wall material value
        Effect,       ///< effect text from (x, y) to (x2, y2)
        Music,        ///< music value, volume value2
    };
    Kind kind = Kind::Print;
    std::uint32_t frame = 0;
    std::string text;
    std::uint32_t object = 0; ///< an extent
    float x = 0;
    float y = 0;
    float x2 = 0;
    float y2 = 0;
    std::int32_t value = 0;
    std::int32_t value2 = 0;
};

/// A script timer (the exe's "activator"): calls `function` at `frame`, with `arg` pushed if
/// the function takes one.
struct ScriptTimer {
    std::uint32_t frame = 0;
    std::uint32_t function = 0;
    std::uint32_t arg = 0;
    std::uint32_t id = 0;
    std::uint32_t trigger = 0; ///< extents (none, when a builtin sets one)
    std::uint32_t caller = 0;
    /// A saved map's timer, read back: its objects' script ids till resolveTimerObjects.
    std::int32_t triggerId = 0;
    std::int32_t callerId = 0;
};

/// Builtin `id`'s argument count and whether it pushes a result, as the host's table has them
/// (the compiler's builtin signatures are checked against it).
struct BuiltinArity {
    int args = 0;
    bool returns = false;
};
BuiltinArity builtinArity(std::uint32_t id);

/// Where the map scripts' stopped calls are logged (ScriptHost::stopLog's lines): a program's
/// log (a dedicated server's stderr, noxsdl's SDL_Log). None set: they're only kept in each host. Set it
/// once at start-up, before any server runs.
void setScriptLog(std::function<void(const std::string&)> sink);

class ScriptHost : public script::Host {
public:
    ScriptHost(World& world, const things::ThingDb& things, const script::ScriptFile& file, std::uint32_t seed = 1);

    script::Vm& vm() noexcept { return vm_; }

    /// scriptRunFirst (map load): globals 0-3 are -2, -1, 1, 0 (the language's
    /// trigger, self, true and false), then GLOBAL (function 1) runs its initialisers.
    void runGlobalInit();
    /// Only the language's four (scriptRunFirst under switch-to-solo: GLOBAL not run).
    void setLanguageGlobals();
    /// Every function whose name begins with `prefix` (MapInitialize, MapEntry:,
    /// PlayerDeath: Plr_UpdateState), in file order.
    void runEntryPoints(std::string_view prefix);
    /// The timers due by `frame`, oldest first, those they start included.
    void runTimers(std::uint32_t frame);
    /// Drops the timers that would run with a deleted object, and its handlers put off.
    void forgetObject(std::uint32_t extent);

    /// The frame builtins see (timers are set from it).
    void setFrame(std::uint32_t frame) noexcept { frame_ = frame; }
    std::uint32_t frame() const noexcept { return frame_; }

    /// A builtin call: a NoX Core mode's answer when it has one (World::modeHooks: ModeHooks::builtin),
    /// else the game's own (gameBuiltin).
    int builtin(std::uint32_t id, script::Vm& vm) override;
    /// The game's own builtin `id` (what builtin() runs with no mode's answer).
    int gameBuiltin(std::uint32_t id, script::Vm& vm);
    /// The cinematic builtins (script_builtins_screen.cpp: UnBlind, Blind, WideScreen,
    /// StartupScreen, DeathScreen, MusicPush/PopEvent, EndGame, ImmediateBlind, ChatTimer).
    int screenBuiltin(std::uint32_t id, script::Vm& vm);
    /// The trigger's (self's) object-variable words, by word offset (function 0's layout): 0
    /// until written. GAME.EXE never allocates them, so it faults where these are used
    /// (OB-210); NoX Core keeps them per object, for its own scripts. Never saved (nor are
    /// they in the exe's ScriptData).
    std::uint32_t objectVar(std::int32_t word) override;
    void setObjectVar(std::int32_t word, std::uint32_t value) override;
    /// Every object's object-variable words, by extent (only objects that had one written).
    const std::map<std::uint32_t, std::vector<std::uint32_t>>& objectVars() const noexcept { return objectVars_; }

    /// The events since the last call (the newest kMaxEvents at most: a caller that never takes
    /// them, a GameHost's, doesn't keep them all).
    std::vector<ScriptEvent> takeEvents() { return std::exchange(events_, {}); }
    static constexpr std::size_t kMaxEvents = 4096;
    const std::list<ScriptTimer>& timers() const noexcept { return timers_; }
    /// Calls to builtins not implemented yet, by id. (Every one of the game's 211 is now: this stays
    /// empty but for an id past the table.)
    const std::map<std::uint32_t, std::uint32_t>& unimplemented() const noexcept { return unimplemented_; }

    /// Calls `function` with these objects (extents; 0 none) as caller and trigger
    /// (script_callByIndex, as the timers, the dialogs, the entry points and the console's call
    /// do: the stack keeps what the function leaves, and the strings it makes stay).
    bool call(std::uint32_t function, std::uint32_t caller = 0, std::uint32_t trigger = 0);
    /// A handler's call (scriptCallByEventBlock): none for one that has run once already or
    /// names no function (the exe's NULL; a once-only one is marked run before it's called).
    /// With a script mid-way (the stack not empty: a builtin set this off) it's put off, at most
    /// 32 waiting, and gives 0. Else it's called, what it returns kept (0 for nothing), the
    /// stack emptied and every string made at run time dropped; then it's off the waiting list
    /// and the first one still waiting is called the same way. What the last call of that chain
    /// kept is the result (the exe's one shared word).
    std::optional<std::uint32_t> callBlock(ServerObject::ScriptBlock& block, std::uint32_t caller, std::uint32_t trigger);
    /// The handlers put off, oldest first (handler, caller, trigger).
    struct QueuedBlock {
        ServerObject::ScriptBlock* block = nullptr;
        std::uint32_t caller = 0;
        std::uint32_t trigger = 0;
    };
    static constexpr std::size_t kMaxQueuedBlocks = 32;
    const std::vector<QueuedBlock>& queuedBlocks() const noexcept { return queued_; }
    /// Whether its function is in the script (the block's index isn't -1).
    bool resolves(const ServerObject::ScriptBlock& block) const
    {
        return !block.function.empty() && vm_.file().find(block.function).has_value();
    }
    /// The function of that name, if the script has it.
    std::optional<std::uint32_t> function(std::string_view name) const
    {
        const auto f = vm_.file().find(name);
        return f ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(*f)) : std::nullopt;
    }
    /// Calls the VM's budget stopped (a script that would have hung the game).
    std::uint32_t stopped() const noexcept { return stopped_; }
    /// A server frame begins: the VM's frame budget afresh (script::Vm::kMaxFrameSteps).
    void newFrame() noexcept { vm_.newFrame(); }
    /// The lines logged for stopped calls (each also to setScriptLog's sink): one per function
    /// and reason on this map, not one per frame. A call stopped at its start because the frame's
    /// budget was already spent is counted in stopped() but not logged (the call that spent it is).
    const std::vector<std::string>& stopLog() const noexcept { return stopLog_; }
    /// The save's ScriptData (server_mapRWScriptData's write side, version 1): with a
    /// compiled script loaded and not game flag 0x400000 (the host's flag 1 is ours
    /// always), GLOBAL's locals and the timers (script_activatorsSave: version 1, the
    /// frame, the count, each frame / function / arg / trigger's and caller's script ids).
    map::ScriptData saveData(std::uint32_t frame);
    /// A saved map's ScriptData read back (server_mapRWScriptData's read side, script_activatorsLoad):
    /// GLOBAL's locals, then the timers at the list's tail, each at `frame` less the save's frame
    /// plus its own, its objects' script ids kept for resolveTimerObjects. False where the exe's
    /// read fails (a version past 1; the timers' version not 1); a short section fails here too
    /// (the exe's reads go on into the next section's bytes). (Wiring, switch-to-solo: read
    /// during the map's sections; then scriptRunFirst sets GLOBAL's first four
    /// (-2, -1, 1, 0) but doesn't run GLOBAL resolves the timers' objects.)
    bool loadData(const map::ScriptData& data, std::uint32_t frame);
    /// script_activatorResolveObjs: the loaded timers' script ids as objects (none
    /// where no object has the id).
    void resolveTimerObjects();
    /// The last timer id given out: the timers' numbering, the game's (never reset by a map change).
    std::uint32_t nextTimerId() const noexcept { return nextTimerId_; }
    void setNextTimerId(std::uint32_t id) noexcept { nextTimerId_ = id; }

    // ---- S5: the creature and object builtins (script_builtins_creatures.cpp) ----
    /// modifier.bin, for the items those builtins give and take (null: anyone may use anything).
    void setModifiers(const things::ModifierDb* modifiers) noexcept { modifiers_ = modifiers; }

private:
    /// The map editor's hot reload (script_reload.hpp, NoX Core's only: never in a game) remaps
    /// the timers between frames.
    friend class ScriptReloadAccess;

    /// One of the creature / object builtins run (its result as builtin()'s), or -1 when `id`
    /// isn't one of them.
    int creatureBuiltin(std::uint32_t id, script::Vm& vm);
    const things::ModifierDb* modifiers_ = nullptr;
    std::minstd_rand creatureRng_{1};     ///< what their fights, spells and drops draw
    std::uint32_t pickupFrame_ = 0;       ///< Pickup's co-op counts' frame
    int pickupsThisFrame_ = 0;            ///< (g_coopPickupsThisFrame)
    bool carryingTooMuchShown_ = false;   ///< (g_coopCarryingTooMuchShown)
    // ---- (end of S5's) ----

    ServerObject* object(std::int32_t scriptValue);
    void addEvent(ScriptEvent e);
    std::uint32_t startTimer(std::uint32_t frames, std::uint32_t function, std::uint32_t arg);
    void wallOp(int op, std::uint32_t handle);
    void setOn(ServerObject& o, bool on);
    /// Effect(name, x1, y1, x2, y2): "MSG_FX_" + name's message, the prefab's offset
    /// (ox, oy) added.
    void effect(std::string_view name, float x1, float y1, float x2, float y2, int ox, int oy);
    std::string lookupName(std::string_view name) const;

    // Dialog, the journal (script_builtins_dialog.cpp): SetDialog, CancelDialog,
    // StoryPic, TellStory, StartDialog, GetAnswer, IsTalking, JournalEntry / Delete / Edit.
    int dialogBuiltin(std::uint32_t id, script::Vm& vm);

    World& world_;
    const things::ThingDb& things_;
    script::Vm vm_;
    std::uint32_t frame_ = 0;
    std::list<ScriptTimer> timers_;
    std::uint32_t nextTimerId_ = 1;
    std::vector<ScriptEvent> events_;
    std::map<std::uint32_t, std::uint32_t> unimplemented_;
    std::uint32_t wideStrings_ = 0; ///< Unused5e's table count (from the script's load)
    std::map<std::uint32_t, std::vector<std::uint32_t>> objectVars_; ///< by extent
    std::vector<QueuedBlock> queued_; ///< the handlers put off (callBlock)
    std::uint32_t blockResult_ = 0;   ///< what the last handler call kept (callBlock)
    bool wallSounds_ = true;
    std::uint32_t stopped_ = 0;
    std::vector<std::string> stopLog_;
    std::vector<std::uint8_t> stopLogged_; ///< by function: a bit per script::Vm::Stop already logged
    /// A top-level call that the budget stopped: logged once per function and reason.
    void logStop(std::uint32_t function);
    std::mt19937 rng_;
    std::minstd_rand wallRng_{1}; ///< a broken wall's debris
};

} // namespace nox::server
