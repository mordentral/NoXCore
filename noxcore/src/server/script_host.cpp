// The map script's host on the server: the general builtins (walls, timers, objects, waypoints,
// owners, prints, effects, quest status), the timers and their save. The creature, dialog and
// cinematic builtins live in script_builtins_creatures.cpp, _dialog.cpp and _screen.cpp.
#include "noxcore/server/script_host.hpp"

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/byte_writer.hpp"
#include "noxcore/net/messages.hpp"
#include "noxcore/script/opcodes.hpp"
#include "noxcore/server/combat.hpp"
#include "noxcore/server/host_client.hpp"
#include "noxcore/server/items.hpp"
#include "noxcore/server/mode_hooks.hpp"
#include "noxcore/server/monsters.hpp"
#include "noxcore/server/objects.hpp"
#include "noxcore/server/physics.hpp"
#include "noxcore/server/modes.hpp"
#include "noxcore/server/player.hpp"
#include "noxcore/server/quest_status.hpp"
#include "noxcore/server/script_names.hpp"
#include "noxcore/server/solo.hpp"
#include "noxcore/server/sound.hpp"
#include "noxcore/server/spells.hpp"
#include "noxcore/server/teleport.hpp"
#include "noxcore/server/trade.hpp"
#include "noxcore/server/walls.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/things/spell_ids.hpp"
#include "noxcore/util/text.hpp"
#include "noxcore/util/random.hpp"
#include "noxcore/util/x87.hpp"

#include <algorithm>
#include <cctype>
#include <array>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <optional>

namespace nox::server {

namespace {

struct Signature {
    std::uint8_t args = 0;
    bool returns = false;
};

// Each builtin's arguments and whether it pushes a result, counted from GAME.EXE (the pops and
// pushes in each function of the builtins' table; the timers push their id, and
// StartupScreen and DeathScreen pop their argument, elsewhere).
// Ten builtins to a row; the number after each row is its first builtin id.
constexpr std::array<Signature, script::kBuiltinCount> kSignatures{{
    {2, true}, {1, false}, {1, false}, {1, false}, {1, false}, {1, false}, {1, false}, {1, false}, {1, false}, {2, true}, // 0
    {2, true}, {2, false}, {2, false}, {2, false}, {2, false}, {1, false}, {1, false}, {1, false}, {1, false}, {1, false}, // 10
    {1, false}, {1, false}, {1, false}, {1, false}, {1, false}, {1, false}, {1, false}, {1, false}, {1, false}, {1, false}, // 20
    {1, false}, {1, false}, {1, false}, {1, false}, {2, false}, {1, false}, {1, false}, {2, false}, {0, false}, {1, false}, // 30
    {1, false}, {1, true}, {1, true}, {1, true}, {2, true}, {2, true}, {3, true}, {3, true}, {1, true}, {1, true}, // 40
    {2, true}, {4, false}, {4, false}, {3, true}, {3, false}, {2, true}, {2, false}, {3, false}, {3, false}, {0, true}, // 50
    {1, true}, {1, true}, {1, true}, {1, true}, {1, true}, {1, true}, {1, true}, {3, false}, {3, false}, {2, false}, // 60
    {2, false}, {3, false}, {4, false}, {1, true}, {1, true}, {2, true}, {1, true}, {2, true}, {2, true}, {2, true}, // 70
    {0, false}, {2, true}, {2, false}, {1, true}, {1, true}, {2, false}, {4, true}, {2, true}, {2, false}, {2, false}, // 80
    {2, false}, {2, false}, {2, false}, {2, false}, {1, true}, {1, true}, {2, false}, {3, false}, {3, false}, {1, true}, // 90
    {5, false}, {2, false}, {2, false}, {2, false}, {2, false}, {2, true}, {2, true}, {2, true}, {2, true}, {1, false}, // 100
    {1, true}, {1, true}, {1, true}, {1, true}, {3, false}, {3, false}, {2, false}, {1, false}, {0, false}, {2, false}, // 110
    {2, false}, {1, true}, {1, true}, {1, false}, {1, true}, {1, true}, {4, false}, {1, false}, {2, false}, {2, false}, // 120
    {2, false}, {3, false}, {4, false}, {4, false}, {5, false}, {0, false}, {0, false}, {1, false}, {1, true}, {6, false}, // 130
    {6, false}, {1, false}, {1, false}, {1, false}, {1, false}, {2, false}, {2, false}, {2, false}, {2, false}, {3, false}, // 140
    {3, false}, {3, false}, {3, false}, {2, false}, {2, false}, {2, false}, {2, false}, {3, false}, {2, false}, {3, false}, // 150
    {2, false}, {2, false}, {2, false}, {2, false}, {3, false}, {3, false}, {2, false}, {2, false}, {2, true}, {1, true}, // 160
    {2, false}, {1, true}, {2, false}, {2, true}, {0, false}, {2, false}, {1, false}, {0, true}, {0, true}, {0, true}, // 170
    {1, false}, {1, false}, {1, false}, {1, false}, {1, true}, {1, true}, {1, false}, {1, false}, {2, false}, {1, false}, // 180
    {3, false}, {2, false}, {4, false}, {0, true}, {1, false}, {2, false}, {0, false}, {1, true}, {1, false}, {1, false}, // 190
    {1, false}, {1, false}, {0, false}, {0, false}, {0, false}, {1, true}, {1, true}, {1, false}, {0, false}, {2, false}, // 200
    {1, true}, // 210
}};

// Builtin ids (their index in the builtins' table), named for the ones handled here.
enum Builtin : std::uint32_t {
    Wall = 0, WallOpen, WallGroupOpen, WallClose, WallGroupClose, WallToggle, WallGroupToggle, WallBreak,
    WallGroupBreak, SecondTimer, FrameTimer, Move, GroupMove,
    ObjectOn = 15, ObjectGroupOn, WaypointOn, WaypointGroupOn, ObjectOff, ObjectGroupOff, WaypointOff,
    WaypointGroupOff, ObjectToggle, ObjectGroupToggle, WaypointToggle, WaypointGroupToggle, Delete, GroupDelete,
    Unused1f = 31, Unused20,
    AudioEvent = 34, Print, PrintToAll, Chat, StopScript, UnlockDoor, LockDoor, IsObjectOn, IsWaypointOn, IsLocked,
    RandomFloat, Random, SecondTimerWithArg, FrameTimerWithArg, IntToString, FloatToString, CreateObject,
    CreateMover = 53, GroupCreateMover,
    GetHost = 59, Object, GetObjectX, GetWaypointX, GetObjectY, GetWaypointY, GetObjectZ, GetDirection, MoveObject,
    MoveWaypoint,
    GetHolder = 76,
    Unused50 = 80,
    Distance = 86,
    Unused58 = 88, Unused59, Unused5a, Unused5b, Unused5c, Unused5d, Unused5e, GetCharacterData,
    CancelTimer = 99, Effect, SetOwner, GroupSetOwner, SetOwners, GroupSetOwners,
    ClearOwner = 109, Waypoint, WaypointGroup, ObjectGroup, WallGroup,
    Unused74 = 116,
    DestroyChat = 117, DestroyEveryChat, SetQuestStatus, SetQuestStatusFloat, GetQuestStatus, GetQuestStatusFloat,
    ResetQuestStatus, IsTrigger, IsCaller,
    GiveXp = 172,
    Music = 175,
    GetTrigger = 178, GetCaller, MakeFriendly, MakeEnemy, BecomePet, BecomeEnemy, IsFriendly, IsPet,
    NoWallSound = 189, SetCallback, DeleteObjectTimer, TrapSpells,
    ClearMessages = 194,
    IsGameBall = 205, IsCrown,
    ChangeScore = 209, GetScore,
};

/// Sounds the exe plays by number (aud).
constexpr std::int32_t kSoundUnlock = 0xEA;
constexpr std::int32_t kSoundLock = 0xE9;
constexpr std::int32_t kSoundElevatorOn = 0xEB;
constexpr std::int32_t kSoundElevatorOff = 0xEC;

/// Move / GroupMove (server_scriptMoveTo): a monster by its action stack, anything
/// else by the movers.
void moveObject(World& world, const things::ThingDb& things, std::uint32_t frame, ServerObject& o, std::int32_t waypoint)
{
    if (o.classes & things::ObjectClass::Monster) {
        if (const ServerWaypoint* wp = world.waypoint(waypoint); wp && !(o.flags & ObjFlag::Dead)) {
            std::minstd_rand rng; // (the stack's pushes and pops draw nothing)
            Fight fight{world, rng, frame, &things, nullptr};
            scriptMove(fight, o, *wp);
        }
        return;
    }
    scriptMoveTo(world, o, waypoint);
}

} // namespace

BuiltinArity builtinArity(std::uint32_t id)
{
    return id < kSignatures.size() ? BuiltinArity{kSignatures[id].args, kSignatures[id].returns} : BuiltinArity{};
}

ScriptHost::ScriptHost(World& world, const things::ThingDb& things, const script::ScriptFile& file, std::uint32_t seed)
    : world_(world)
    , things_(things)
    , vm_(file, *this)
    , rng_(seed)
{
}

void ScriptHost::setLanguageGlobals()
{
    auto globals = vm_.globals();
    const std::array<std::uint32_t, 4> preset{0xFFFFFFFEu, 0xFFFFFFFFu, 1, 0};  // -2 the trigger, -1 the caller (see object()), true, false
    for (std::size_t i = 0; i < preset.size() && i < globals.size(); ++i) {
        globals[i] = preset[i];
    }
}

void ScriptHost::runGlobalInit()
{
    setLanguageGlobals();
    if (vm_.file().functions.size() > 1) {
        call(1);
    }
}

void ScriptHost::runEntryPoints(std::string_view prefix)
{
    const auto& functions = vm_.file().functions;
    for (std::size_t i = 0; i < functions.size(); ++i) {
        if (functions[i].name.starts_with(prefix)) {
            call(static_cast<std::uint32_t>(i));
        }
    }
}

bool ScriptHost::call(std::uint32_t function, std::uint32_t caller, std::uint32_t trigger)
{
    const bool ran = vm_.call(function, caller, trigger);
    if (!ran) {
        ++stopped_;
        // (A nested call's stop unwinds the call it's in, which is logged by its own name.)
        if (!vm_.running()) {
            logStop(function);
        }
    }
    return ran;
}

namespace {

std::mutex g_scriptLogMutex;
std::function<void(const std::string&)> g_scriptLog;

} // namespace

void setScriptLog(std::function<void(const std::string&)> sink)
{
    const std::lock_guard lock(g_scriptLogMutex);
    g_scriptLog = std::move(sink);
}

void ScriptHost::logStop(std::uint32_t function)
{
    using Stop = script::Vm::Stop;
    const Stop why = vm_.lastStop();
    // A call the frame's spent budget stopped before it ran anything: the one that spent it says so.
    if (why == Stop::None || (why == Stop::FrameSteps && vm_.lastSteps() == 0)) {
        return;
    }
    const auto& functions = vm_.file().functions;
    if (function >= functions.size()) {
        return;
    }
    if (stopLogged_.size() < functions.size()) {
        stopLogged_.resize(functions.size(), 0);
    }
    const auto bit = static_cast<std::uint8_t>(1u << static_cast<unsigned>(why));
    if (stopLogged_[function] & bit) {
        return;
    }
    stopLogged_[function] |= bit;
    std::string line = "script: map " + (world_.mapName.empty() ? std::string("(unnamed)") : world_.mapName) + ": " +
                       functions[function].name + " stopped: ";
    switch (why) {
    case Stop::Steps:
        line += "it ran " + std::to_string(script::Vm::kMaxSteps) + " instructions in one call (a loop that doesn't end?)";
        break;
    case Stop::FrameSteps:
        line += "the frame's " + std::to_string(script::Vm::kMaxFrameSteps) +
                " script instructions ran out in it; the frame's later script calls were stopped too";
        break;
    case Stop::Depth:
        line += "its script calls nested deeper than " + std::to_string(script::Vm::kMaxDepth) +
                " (a function that calls itself without end?)";
        break;
    default:
        line += "the handlers its builtins set off nested deeper than " + std::to_string(script::Vm::kMaxNested) +
                " (a handler that sets itself off?)";
        break;
    }
    line += " (not logged again for it on this map)";
    stopLog_.push_back(line);
    const std::lock_guard lock(g_scriptLogMutex);
    if (g_scriptLog) {
        g_scriptLog(line);
    }
}

std::optional<std::uint32_t> ScriptHost::callBlock(ServerObject::ScriptBlock& block, std::uint32_t caller, std::uint32_t trigger)
{
    // scriptCallByEventBlock. Its result is one shared word, 0 from the start of every call.
    blockResult_ = 0;
    // Block flags: 2 a handler that runs once, 1 it has run.
    if (block.flags & 1u) {
        return std::nullopt;
    }
    // (noxcpp: a mode's handler: the mode answers, not the map's script. The map script's stack
    // isn't touched, so the waiting list isn't drained here: only a map handler's call does that.)
    if (block.mode) {
        if (block.flags & 2u) {
            block.flags |= 1u;
        }
        return world_.modeHooks ? world_.modeHooks->scriptBlock(*this, block.function.view(), caller, trigger) : std::nullopt;
    }
    const auto index = block.function.empty() ? std::nullopt : vm_.file().find(block.function); // (-1 unknown)
    if (!index) {
        return std::nullopt;
    }
    if (block.flags & 2u) {
        block.flags |= 1u;
    }
    // A script mid-way (its stack not empty: one of its builtins set this off): put off till the
    // end of the next handler call made with the stack empty. (A once-only one is marked run
    // already, and a pickup handler loses its function right after this call, so when its turn
    // comes it's refused and stays first, holding up the rest: an original bug.)
    if (vm_.stackTop() != 0) {
        if (queued_.size() < kMaxQueuedBlocks) {
            queued_.push_back({&block, caller, trigger});
        }
        return blockResult_;
    }
    call(static_cast<std::uint32_t>(*index), caller, trigger);
    // What it returns, popped; then the stack emptied and the strings it (and any timer, dialog
    // or entry point since the last handler) made dropped.
    if (vm_.file().functions[*index].returns != 0) {
        blockResult_ = vm_.pop();
    }
    vm_.resetStack();
    vm_.freeRuntimeStrings();
    // Off the waiting list (every entry for this handler, caller and trigger), and the first
    // still waiting called: its call sets the shared result again (an original bug: the
    // deciding handler's caller then reads that).
    std::erase_if(queued_, [&](const QueuedBlock& q) { return q.block == &block && q.caller == caller && q.trigger == trigger; });
    if (!queued_.empty()) {
        const QueuedBlock next = queued_.front();
        callBlock(*next.block, next.caller, next.trigger);
    }
    return blockResult_;
}

void ScriptHost::runTimers(std::uint32_t frame)
{
    // Oldest first; a timer started by one that runs is appended and, if due, runs this pass.
    // (The next due one is looked for afresh after each call: what runs may cancel the one after
    // it, which the exe's walk goes on through freed, an original bug. Those before it
    // aren't due, so the order is the exe's.)
    for (;;) {
        const auto it = std::ranges::find_if(timers_, [&](const ScriptTimer& t) { return t.frame <= frame; });
        if (it == timers_.end()) {
            break;
        }
        const ScriptTimer t = *it;
        timers_.erase(it);
        const auto& functions = vm_.file().functions;
        if (t.function < functions.size() && functions[t.function].args != 0) {
            vm_.push(t.arg);
        }
        call(t.function, t.caller, t.trigger);
    }
}

void ScriptHost::forgetObject(std::uint32_t extent)
{
    for (auto it = timers_.begin(); it != timers_.end();) {
        if (it->trigger == extent) {
            it = timers_.erase(it);
        } else {
            if (it->caller == extent) {
                it->caller = 0;
            }
            ++it;
        }
    }
    objectVars_.erase(extent);
    // Its handlers put off (each is the trigger's own). (The exe keeps them, pointing into the
    // freed object.)
    std::erase_if(queued_, [&](const QueuedBlock& q) { return q.trigger == extent; });
}

std::uint32_t ScriptHost::startTimer(std::uint32_t frames, std::uint32_t function, std::uint32_t arg)
{
    const std::uint32_t id = nextTimerId_;
    nextTimerId_ = nextTimerId_ >= 32000 ? 1 : nextTimerId_ + 1;  // ids wrap from 32000 back to 1
    timers_.push_back({frame_ + frames, function, arg, id, 0, 0});
    return id;
}

map::ScriptData ScriptHost::saveData(std::uint32_t frame)
{
    map::ScriptData s;
    s.version = 1;
    s.hasState = !vm_.file().functions.empty() && !(world_.gameFlags & 0x400000u) ? 1 : 0;
    if (s.hasState == 0) {
        return s;
    }
    ByteWriter w;
    for (const std::uint32_t g : vm_.globals()) {
        w.u32(g);
    }
    // script_activatorsSave: the list in its order (new ones at its tail).
    w.u16(1);
    w.u32(frame);
    w.u32(static_cast<std::uint32_t>(timers_.size()));
    const auto scriptIdOf = [&](std::uint32_t extent) -> std::uint32_t {
        const ServerObject* o = extent ? world_.byExtent(extent) : nullptr;
        return o ? static_cast<std::uint32_t>(o->scriptId) : 0u;
    };
    for (const ScriptTimer& t : timers_) {
        w.u32(t.frame);
        w.u32(t.function);
        w.u32(t.arg);
        w.u32(scriptIdOf(t.trigger));
        w.u32(scriptIdOf(t.caller));
    }
    s.state = std::move(w).take();
    return s;
}

bool ScriptHost::loadData(const map::ScriptData& data, std::uint32_t frame)
{
    if (data.version > 1) {
        return false;
    }
    if (data.hasState == 0) {
        return true;
    }
    const auto globals = vm_.globals();
    try {
        ByteReader r(data.state);
        for (std::uint32_t& g : globals) {
            g = r.u32();
        }
        const std::uint16_t version = r.u16();
        if (version > 1 || version < 1) {
            return false;
        }
        const std::uint32_t saved = r.u32();
        const std::uint32_t count = r.u32();
        for (std::uint32_t i = 0; i < count; ++i) {
            ScriptTimer t;
            t.frame = frame - saved + r.u32();  // what was left of it at the save, from now
            t.function = r.u32();
            t.arg = r.u32();
            t.triggerId = static_cast<std::int32_t>(r.u32());
            t.callerId = static_cast<std::int32_t>(r.u32());
            t.id = nextTimerId_; // (script_activatorNextID)
            nextTimerId_ = nextTimerId_ >= 32000 ? 1 : nextTimerId_ + 1;
            timers_.push_back(t);
        }
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

void ScriptHost::resolveTimerObjects()
{
    for (ScriptTimer& t : timers_) {
        if (t.triggerId != 0) {
            const ServerObject* o = object(t.triggerId); // (server_scriptValToObjectPtr)
            t.trigger = o ? o->extent : 0;
            t.triggerId = 0;
        }
        if (t.callerId != 0) {
            const ServerObject* o = object(t.callerId);
            t.caller = o ? o->extent : 0;
            t.callerId = 0;
        }
    }
}

ServerObject* ScriptHost::object(std::int32_t value)
{
    // server_scriptValToObjectPtr: -1 the caller, -2 the trigger, else a script id.
    if (value == -1 || value == -2) {
        // (not one with flag 0x20: deleted, ours pendingDelete)
        const std::uint32_t extent = value == -1 ? vm_.caller() : vm_.trigger();
        ServerObject* o = extent ? world_.byExtent(extent) : nullptr;
        return o && !o->pendingDelete ? o : nullptr;
    }
    return world_.byScriptId(value);
}

std::string ScriptHost::lookupName(std::string_view name) const
{
    // A prefab's names carry its "%Prefix" after them (the prefix is appended).
    return std::string(name) + std::string(vm_.context().prefix);
}

void ScriptHost::addEvent(ScriptEvent e)
{
    e.frame = frame_;
    // (noxcpp: only test runs take them, each frame; a host never does, and its map
    // can stay up for days. The oldest half let go once kMaxEvents wait, so they can't pile up.)
    if (events_.size() >= kMaxEvents) {
        events_.erase(events_.begin(), events_.begin() + static_cast<std::ptrdiff_t>(kMaxEvents / 2));
    }
    events_.push_back(std::move(e));
}

void ScriptHost::wallOp(int op, std::uint32_t handle)
{
    // A wall handle: x in the high 16 bits, y in the low 16.
    const int x = static_cast<std::int32_t>(handle) >> 16;
    const int y = static_cast<int>(handle & 0xFFFF);
    WallCell* c = world_.wall(x, y);
    if (!c) {
        return;
    }
    const auto sound = [&](const char* what) {
        if (wallSounds_) {
            // Walls sit on a 23-unit grid; + 11.5 is the cell's middle.
            addEvent({ScriptEvent::Kind::WallSound, 0, what, 0, x * 23.0f + 11.5f, y * 23.0f + 11.5f, 0, 0, c->material, 0});
            // Its material's OPEN or CLOSE sound, else SecretWallOpen / SecretWallClose
            // (wallFindCloseSound and the open one's), at its middle.
            const things::WallDef* def = c->material < things_.walls.size() ? &things_.walls[c->material] : nullptr;
            const bool opening = std::string_view(what) == "open";
            std::string_view name = opening ? "SecretWallOpen" : "SecretWallClose";
            if (def && !(opening ? def->openSound : def->closeSound).empty()) {
                name = opening ? def->openSound : def->closeSound;
            }
            playSoundAt(world_, name, x * 23.0f + 11.5f, y * 23.0f + 11.5f);
        }
    };
    switch (op) {
    case 0: // open: a secret wall not open or opening starts opening
        if (c->secret && c->secretState != 3 && c->secretState != 4) {
            c->secretState = 4;
            sound("open");
        }
        break;
    case 1: // close
        if (c->secret && c->secretState != 1 && c->secretState != 2) {
            c->secretState = 2;
            sound("close");
        }
        break;
    case 2: // toggle
        if (c->secret) {
            const bool shut = c->secretState == 1 || c->secretState == 2;
            c->secretState = shut ? 4 : 2;
            sound(shut ? "open" : "close");
        }
        break;
    case 3: // break (wallPreDestroy: debris, MSG_DESTROY_WALL)
        Walls{world_, things_, wallRng_, frame_}.breakWall(x, y);
        break;
    }
}

void ScriptHost::setOn(ServerObject& o, bool on)
{
    namespace oc = things::ObjectClass;
    // objectSetOn / SetOff: an elevator sounds as it changes; traps,
    // fires and the like stop colliding while off.
    if (o.enabled() != on && (o.classes & oc::Elevator)) {
        addEvent({ScriptEvent::Kind::ObjectSound, 0, {}, o.extent, o.x, o.y, 0, 0, on ? kSoundElevatorOn : kSoundElevatorOff, 0});
        playSound(world_, static_cast<std::uint32_t>(on ? kSoundElevatorOn : kSoundElevatorOff), o);
    }
    setOnOff(o, on); // (unitSetOnOff: clients told)
    if (o.classes & 0x10042000u) {
        if (on) {
            o.flags &= ~ObjFlag::NoCollide;
        } else {
            o.flags |= ObjFlag::NoCollide;
        }
    }
    if (on && !(o.classes & oc::Missile)) {
        queueMotion(world_, o); // (objectSetOn's unitHasCollideOrUpdateFn)
    }
}

void ScriptHost::effect(std::string_view name, float x1, float y1, float x2, float y2, int ox, int oy)
{
    // The op whose name is "MSG_FX_" + name (any case), among 0x7E-0x9A (script_names.hpp); none of
    // those, nothing.
    const std::uint8_t op = scriptEffectOp(name);
    const float px = static_cast<float>(ox) + x1;
    const float py = static_cast<float>(oy) + y1;
    const auto u16 = [](int v) { return static_cast<std::uint16_t>(v); };
    const auto put = [](std::vector<std::uint8_t>& m, std::uint16_t v) {
        m.push_back(static_cast<std::uint8_t>(v));
        m.push_back(static_cast<std::uint8_t>(v >> 8));
    };
    // (How each is sent: script_names.hpp's scriptEffectKind, which the editor's script tools read.)
    switch (scriptEffectKind(op)) {
    case EffectKind::Point: { // a point (netSendPointFx)
        std::vector<std::uint8_t> m{op};
        put(m, u16(static_cast<int>(px)));
        put(m, u16(static_cast<int>(py)));
        world_.broadcasts().push_back({std::move(m), false, 0});
        break;
    }
    case EffectKind::Ray: { // a ray (netSendRayFx), whole units
        std::vector<std::uint8_t> m{op};
        put(m, u16(static_cast<int>(x1) + ox));
        put(m, u16(static_cast<int>(y1) + oy));
        put(m, u16(static_cast<int>(x2) + ox));
        put(m, u16(static_cast<int>(y2) + oy));
        world_.broadcasts().push_back({std::move(m), false, 0});
        break;
    }
    case EffectKind::Burst: { // a spark burst (netSparkExplosionFx), x2 its size
        std::vector<std::uint8_t> m{op};
        put(m, u16(static_cast<int>(px)));
        put(m, u16(static_cast<int>(py)));
        m.push_back(static_cast<std::uint8_t>(static_cast<int>(x2)));
        world_.broadcasts().push_back({std::move(m), false, 0});
        break;
    }
    case EffectKind::Quake: // a quake (earthquakeSend), x2 its strength
        jiggle(world_, px, py, static_cast<int>(x2));
        break;
    // 0x98, the green bolt: sent from four ints never set (an original bug); not sent here.
    case EffectKind::None:
        break;
    }
}

std::uint32_t ScriptHost::objectVar(std::int32_t word)
{
    const std::uint32_t trigger = vm_.trigger();
    const auto it = objectVars_.find(trigger);
    if (!trigger || word < 0 || it == objectVars_.end() || static_cast<std::size_t>(word) >= it->second.size()) {
        return 0;
    }
    return it->second[static_cast<std::size_t>(word)];
}

void ScriptHost::setObjectVar(std::int32_t word, std::uint32_t value)
{
    const std::uint32_t trigger = vm_.trigger();
    if (!trigger || word < 0) {
        return;
    }
    auto& vars = objectVars_[trigger];
    if (static_cast<std::size_t>(word) >= vars.size()) {
        vars.resize(static_cast<std::size_t>(word) + 1, 0);
    }
    vars[static_cast<std::size_t>(word)] = value;
}

int ScriptHost::builtin(std::uint32_t id, script::Vm& vm)
{
    // (noxcpp: a mode's answer first; none, the game's.)
    if (world_.modeHooks) {
        if (const std::optional<int> r = world_.modeHooks->builtin(*this, id, vm)) {
            return *r;
        }
    }
    return gameBuiltin(id, vm);
}

// A map script's builtin call: its arguments popped off the VM's stack (last argument first),
// its result pushed. 0 to go on, 1 to stop the script (StopScript). Every one of the game's 211
// is here (checked one by one); the fallback at the end (pop, push 0, counted) is left for
// safety only.
int ScriptHost::gameBuiltin(std::uint32_t id, script::Vm& vm)
{
    const auto popObject = [&] { return object(vm.popInt()); };
    const auto forGroup = [&](ServerGroup::Kind kind, auto fn) {
        // server_scriptExecuteFnForEachGroupObj: a group of that kind, its members; a
        // group of groups (3) each of its groups in turn, of whatever kind. A wall group's walk
        // falls on into case 3 (its switch has no break there: an original bug), taking each
        // wall's x as a group's id. (A group met again on the way: the exe recurses till its
        // stack runs out; ours goes no further.)
        std::vector<const ServerGroup*> walking;
        const auto walk = [&](const auto& self, const ServerGroup* g) -> void {
            if (!g || std::ranges::find(walking, g) != walking.end()) {
                return;
            }
            walking.push_back(g);
            switch (g->kind) {
            case ServerGroup::Kind::Walls:
                if (kind != ServerGroup::Kind::Walls) {
                    break;
                }
                if constexpr (std::is_invocable_v<decltype(fn), std::uint32_t>) { // (a wall group's walk: wall handles)
                    for (const auto& [x, y] : g->walls) {
                        fn(wallHandle(x, y));
                    }
                }
                for (const auto& [x, y] : g->walls) {
                    self(self, world_.group(x));
                }
                break;
            case ServerGroup::Kind::Unknown3:
                for (const GroupMember m : g->members) {
                    self(self, world_.group(m.groupId()));
                }
                break;
            default:
                if constexpr (std::is_invocable_v<decltype(fn), GroupMember>) { // (an object or waypoint group's: its members)
                    if (g->kind == kind) {
                        for (const GroupMember m : g->members) {
                            fn(m);
                        }
                    }
                }
                break;
            }
            walking.pop_back();
        };
        walk(walk, world_.group(vm.popInt()));
    };
    const auto eachGroupObject = [&](auto fn) {
        // (each member by its map number, netGetUnitByExtent: one on the map)
        forGroup(ServerGroup::Kind::Objects, [&](GroupMember m) {
            if (ServerObject* o = world_.byMapNumber(m.mapNumber())) {
                fn(*o);
            }
        });
    };
    const auto eachGroupWaypoint = [&](auto fn) {
        forGroup(ServerGroup::Kind::Waypoints, [&](GroupMember m) {
            if (ServerWaypoint* w = world_.waypoint(m.waypointId())) {
                fn(*w);
            }
        });
    };
    const auto pushName = [&](std::string s) { vm.push(vm.addString(std::move(s))); };

    switch (id) {
    case Wall: {
        int y = vm.popInt();
        int x = vm.popInt();
        const auto& c = vm.context();
        if (c.x != 0 || c.y != 0) {
            // A prefab's offset (in units) moves the wall cell too.
            x = (x * 23 + c.x) / 23;
            y = (y * 23 + c.y) / 23;
        }
        vm.push(world_.wall(x, y) ? wallHandle(x, y) : 0);
        return 0;
    }
    case WallOpen: wallOp(0, vm.pop()); return 0;
    case WallClose: wallOp(1, vm.pop()); return 0;
    case WallToggle: wallOp(2, vm.pop()); return 0;
    case WallBreak: wallOp(3, vm.pop()); return 0;
    case WallGroupOpen: forGroup(ServerGroup::Kind::Walls, [&](std::uint32_t h) { wallOp(0, h); }); return 0;
    case WallGroupClose: forGroup(ServerGroup::Kind::Walls, [&](std::uint32_t h) { wallOp(1, h); }); return 0;
    case WallGroupToggle: forGroup(ServerGroup::Kind::Walls, [&](std::uint32_t h) { wallOp(2, h); }); return 0;
    case WallGroupBreak: forGroup(ServerGroup::Kind::Walls, [&](std::uint32_t h) { wallOp(3, h); }); return 0;
    case SecondTimer:
    case FrameTimer: {
        const std::uint32_t function = vm.pop();
        const std::int32_t time = vm.popInt();
        vm.push(startTimer(static_cast<std::uint32_t>(id == SecondTimer ? time * 30 : time), function, 0));  // 30 frames a second
        return 0;
    }
    case SecondTimerWithArg:
    case FrameTimerWithArg: {
        const std::uint32_t function = vm.pop();
        const std::uint32_t arg = vm.pop();
        const std::int32_t time = vm.popInt();
        vm.push(startTimer(static_cast<std::uint32_t>(id == SecondTimerWithArg ? time * 30 : time), function, arg));
        return 0;
    }
    case CancelTimer: {
        const std::uint32_t timer = vm.pop();
        const auto it = std::find_if(timers_.begin(), timers_.end(), [&](const ScriptTimer& t) { return t.id == timer; });
        const bool found = it != timers_.end();
        if (found) {
            timers_.erase(it);
        }
        vm.push(found ? 1 : 0);
        return 0;
    }
    case ObjectOn:
    case ObjectOff:
    case ObjectToggle:
        if (ServerObject* o = popObject()) {
            setOn(*o, id == ObjectOn || (id == ObjectToggle && !o->enabled()));
        }
        return 0;
    case Move: { // Move(object, waypoint)
        const std::int32_t wp = vm.popInt();
        ServerObject* o = popObject();
        if (o && !(o->flags & ObjFlag::Dead) && world_.waypoint(wp)) {
            moveObject(world_, things_, frame_, *o, wp);
        }
        return 0;
    }
    case GiveXp: { // (script_GiveExp) GiveXp(object, xp): Plr_GiveExperience
        const float xp = vm.popFloat();
        ServerObject* o = popObject();
        if (o) {
            std::minstd_rand rng; // (the award draws nothing)
            Fight fight{world_, rng, frame_, &things_, nullptr};
            giveExperience(fight, *o, xp);
        }
        return 0;
    }
    case BecomePet:   // (script_BecomePet) BecomePet(object): the host's (player 0x1F) pet
    case BecomeEnemy: { // (script_BecomeEnemy) BecomeEnemy(object): no longer
        ServerObject* o = popObject();
        ServerObject* host = hostPlayer(world_); // (Plr_GetByIndex(0x1F): the host's own player)
        if (o && host) {
            if (id == BecomePet) {
                becomePet(world_, *host, *o);
            } else {
                removeMonitors(world_, *host, *o);
            }
        }
        return 0;
    }
    // The owners (their builtins, script_MakeFriendly /
    // MakeEnemy): unitSetOwner / unitClearOwner, so a
    // creature its player controls is lost to it (monsters.hpp). A group's members are taken
    // whatever its kind (server_scriptGetGroup), each by its map number (netGetUnitByExtent).
    case SetOwner:        // SetOwner(owner, unit)
    case GroupSetOwner:   // GroupSetOwner(owner, group): the group's members are the owner's
    case SetOwners:       // SetOwners(group, unit): each of the group in turn owns it (the last's)
    case GroupSetOwners: { // GroupSetOwners(group, group)
        const std::int32_t b = vm.popInt();
        const std::int32_t a = vm.popInt();
        const auto members = [&](std::int32_t group) {
            std::vector<ServerObject*> out;
            if (ServerGroup* g = world_.group(group)) {
                for (const GroupMember m : g->members) {
                    if (ServerObject* o = world_.byMapNumber(m.mapNumber())) {
                        out.push_back(o);
                    }
                }
            }
            return out;
        };
        if (id == SetOwner) {
            if (ServerObject* unit = object(b)) {
                setOwner(world_, object(a), *unit);
            }
        } else if (id == GroupSetOwner) {
            ServerObject* owner = object(a);
            for (ServerObject* unit : members(b)) {
                setOwner(world_, owner, *unit);
            }
        } else if (id == SetOwners) {
            if (!world_.group(a)) {
                return 0;
            }
            ServerObject* unit = object(b);
            for (ServerObject* owner : members(a)) {
                if (unit) {
                    setOwner(world_, owner, *unit);
                }
            }
        } else if (world_.group(a) && world_.group(b)) {
            for (ServerObject* owner : members(a)) {
                for (ServerObject* unit : members(b)) {
                    setOwner(world_, owner, *unit);
                }
            }
        }
        return 0;
    }
    case ClearOwner: // ClearOwner(unit)
        if (ServerObject* o = popObject()) {
            clearOwner(world_, *o);
        }
        return 0;
    case MakeFriendly:  // MakeFriendly(unit): subclass 0x100, the host's (player 0x1F's)
    case MakeEnemy: { // MakeEnemy(unit): subclass 0x100 off, nobody's
        ServerObject* o = popObject();
        if (!o) {
            return 0;
        }
        if (id == MakeEnemy) {
            o->subclasses &= ~0x100u;
            clearOwner(world_, *o);
            return 0;
        }
        o->subclasses |= 0x100u;
        if (ServerObject* host = hostPlayer(world_)) { // (player 0x1F's; no host: the exe reads through null)
            setOwner(world_, host, *o);
        }
        return 0;
    }
    case TrapSpells: { // TrapSpells(bomber, spell, spell, spell): its glyph replaced by one of these
        const std::int32_t c = vm.popInt();
        const std::int32_t b = vm.popInt();
        const std::int32_t a = vm.popInt();
        ServerObject* o = popObject();
        if (o && o->monster && (o->classes & things::ObjectClass::Monster) && (o->subclasses & 0x2000u)) {  // subclass 0x2000: BOMBER
            const auto spell = [&](std::int32_t s) -> std::uint32_t {
                const auto id = things::spellId(vm.string(s));
                return id ? *id : 0u;
            };
            // (the first glyph it carries gone, delayedDeleteObject.)
            for (const std::uint32_t e : o->inventory) {
                if (ServerObject* g = world_.byExtent(e); g && g->typeName == "Glyph") {
                    deleteObject(world_, *g);
                    g->flags |= ObjFlag::Destroyed;
                    break;
                }
            }
            carriedGlyph(world_, things_, *o, std::array<std::uint32_t, 3>{spell(a), spell(b), spell(c)}, true);
        }
        return 0;
    }
    case GroupMove: { // GroupMove(group, waypoint): its members by map number, whatever its kind
        const std::int32_t wp = vm.popInt();
        const ServerGroup* g = world_.group(vm.popInt());
        if (g && world_.waypoint(wp)) {
            for (const GroupMember m : g->members) {
                if (ServerObject* o = world_.byMapNumber(m.mapNumber())) {
                    moveObject(world_, things_, frame_, *o, wp);
                }
            }
        }
        return 0;
    }
    case ObjectGroupOn:
    case ObjectGroupOff:
    case ObjectGroupToggle:
        eachGroupObject([&](ServerObject& o) { setOn(o, id == ObjectGroupOn || (id == ObjectGroupToggle && !o.enabled())); });
        return 0;
    case WaypointOn:
    case WaypointOff:
    case WaypointToggle:
        if (ServerWaypoint* w = world_.waypoint(vm.popInt())) {
            w->enabled = id == WaypointOn || (id == WaypointToggle && !w->enabled);
        }
        return 0;
    case WaypointGroupOn:
    case WaypointGroupOff:
    case WaypointGroupToggle:
        eachGroupWaypoint([&](ServerWaypoint& w) { w.enabled = id == WaypointGroupOn || (id == WaypointGroupToggle && !w.enabled); });
        return 0;
    case Delete: // (delayedDeleteObject: a held one out of the inventory at once)
        if (ServerObject* o = popObject()) {
            deleteObject(world_, *o);
        }
        return 0;
    case GroupDelete:
        eachGroupObject([&](ServerObject& o) { deleteObject(world_, o); });
        return 0;
    case AudioEvent: {
        ServerWaypoint* w = world_.waypoint(vm.popInt());
        const std::uint32_t name = vm.pop();
        if (w) {
            addEvent({ScriptEvent::Kind::Audio, 0, vm.string(name), 0, w->x, w->y});
            playSoundAt(world_, vm.string(name), w->x, w->y);
        }
        return 0;
    }
    case Print: { // the string table's line to the caller, a player
        const std::uint32_t text = vm.pop();
        ServerObject* caller = vm.caller() ? world_.byExtent(vm.caller()) : nullptr;
        if (caller && (caller->classes & things::ObjectClass::Player)) {
            addEvent({ScriptEvent::Kind::Print, 0, vm.string(text), caller->extent});
            world_.textTo(*caller, vm.string(text));
        }
        return 0;
    }
    case PrintToAll: { // to everyone
        const std::string text = vm.string(vm.pop());
        addEvent({ScriptEvent::Kind::PrintToAll, 0, text});
        world_.textToAll(world_.text(text));
        return 0;
    }
    case Chat: { // the object says it (a solo game's spoken line too: the host's)
        const std::uint32_t text = vm.pop();
        if (ServerObject* o = popObject()) {
            addEvent({ScriptEvent::Kind::Chat, 0, vm.string(text), o->extent, o->x, o->y});
            // (Str_GetString's one variant: its text said, its speech file the voice.)
            const World::Line line = world_.line(vm.string(text));
            world_.unitSay(*o, line.text, 0);
            if ((world_.gameFlags & GameFlag::Solo) && world_.hostClient) {
                world_.hostClient->playVoice(line.speech, 100); // (playDialogFile, none too)
            }
        }
        return 0;
    }
    case StopScript:
        return 1;
    case UnlockDoor:
    case LockDoor:
        if (ServerObject* o = popObject(); o && (o->classes & things::ObjectClass::Door)) {
            o->lock = id == LockDoor ? 5 : 0;
            addEvent({ScriptEvent::Kind::ObjectSound, 0, {}, o->extent, o->x, o->y, 0, 0, id == LockDoor ? kSoundLock : kSoundUnlock});
            playSound(world_, static_cast<std::uint32_t>(id == LockDoor ? kSoundLock : kSoundUnlock), *o);
        }
        return 0;
    case IsObjectOn: {
        ServerObject* o = popObject();
        vm.push(o && o->enabled() ? 1 : 0);
        return 0;
    }
    case IsWaypointOn: {
        ServerWaypoint* w = world_.waypoint(vm.popInt());
        vm.push(w && w->enabled ? 1 : 0);
        return 0;
    }
    case IsLocked: {
        ServerObject* o = popObject();
        vm.push(o && (o->classes & things::ObjectClass::Door) && o->lock != 0 ? 1 : 0);
        return 0;
    }
    case RandomFloat: {
        const float hi = vm.popFloat();
        const float lo = vm.popFloat();
        vm.pushFloat(util::randomFloat(rng_, lo, hi).f()); // (randomFloat(lo, hi), FSTP: a top below the bottom draws downwards)
        return 0;
    }
    case Random: {
        // (randInt: lo plus a draw from the random table (rand()'s 0 - 32767) modulo the span
        // hi - lo + 1, in 32 bits; a span of 0, hi with no draw. A span past 32768 never gets
        // past lo + 32767, and with lo above hi the span is negative and the remainder lies in
        // 0 .. lo - hi - 2: from lo up, not between them. Original bugs.)
        const std::int32_t hi = vm.popInt();
        const std::int32_t lo = vm.popInt();
        const auto span = static_cast<std::int32_t>(static_cast<std::uint32_t>(hi) - static_cast<std::uint32_t>(lo) + 1u);
        if (lo == hi) {
            vm.pushInt(lo);
        } else if (span == 0) {
            vm.pushInt(hi);
        } else if (0 < span && span <= 32768) {
            vm.pushInt(nox::util::uniform_int_distribution<std::int32_t>(lo, hi)(rng_));
        } else {
            const std::int64_t width = span > 0 ? 32768 : std::min<std::int64_t>(32768, -static_cast<std::int64_t>(span));
            const auto r = nox::util::uniform_int_distribution<std::int32_t>(0, static_cast<std::int32_t>(width - 1))(rng_);
            vm.pushInt(static_cast<std::int32_t>(static_cast<std::uint32_t>(lo) + static_cast<std::uint32_t>(r)));
        }
        return 0;
    }
    case IntToString:
        pushName(std::to_string(vm.popInt()));
        return 0;
    case FloatToString: {
        char text[64];
        std::snprintf(text, sizeof text, "%f", static_cast<double>(vm.popFloat()));
        pushName(text);
        return 0;
    }
    case CreateObject: {
        ServerWaypoint* w = world_.waypoint(vm.popInt());
        const std::uint32_t type = vm.pop();
        // (Nothing at all is pushed without the waypoint: the game's own slip.)
        if (w) {
            if (!things_.findThing(vm.string(type))) {
                vm.push(0);
            } else {
                vm.pushInt(world_.spawn(things_, vm.string(type), w->x, w->y).scriptId);
            }
        }
        return 0;
    }
    case CreateMover: { // CreateMover(object, waypoint, speed): the mover's script id
        const float speed = vm.popFloat();
        const std::int32_t wp = vm.popInt();
        ServerObject* o = popObject();
        vm.pushInt(o ? createMover(world_, things_, *o, wp, speed, frame_) : 0); // (none: the lookup's 0)
        return 0;
    }
    case GroupCreateMover: { // GroupCreateMover(group, waypoint, speed): one each, nothing pushed
        const float speed = vm.popFloat();
        const std::int32_t wp = vm.popInt();
        eachGroupObject([&](ServerObject& o) { createMover(world_, things_, o, wp, speed, frame_); });
        return 0;
    }
    case GetHost: { // (the host's, player slot 0x1F; none, 0)
        const ServerObject* host = hostPlayer(world_);
        vm.pushInt(host ? host->scriptId : 0);
        return 0;
    }
    case Object: {
        ServerObject* o = world_.byScriptName(lookupName(vm.string(vm.pop())));
        vm.pushInt(o ? o->scriptId : 0);
        return 0;
    }
    case GetObjectX:
    case GetObjectY:
    case GetObjectZ: {
        ServerObject* o = popObject();
        if (!o) {
            vm.push(0);
        } else {
            vm.pushFloat(id == GetObjectX ? o->x : id == GetObjectY ? o->y : o->z);
        }
        return 0;
    }
    case GetWaypointX:
    case GetWaypointY: {
        ServerWaypoint* w = world_.waypoint(vm.popInt());
        if (!w) {
            vm.push(0);
        } else {
            vm.pushFloat(id == GetWaypointX ? w->x : w->y);
        }
        return 0;
    }
    case GetDirection: {
        ServerObject* o = popObject();
        vm.pushInt(o ? static_cast<std::int16_t>(o->direction) : 0);
        return 0;
    }
    case MoveObject: {
        const float y = vm.popFloat();
        const float x = vm.popFloat();
        if (ServerObject* o = popObject()) {
            // (Obj_SetPosition, its IMMOBILE return, sync, relink, motion,
            // harpoon and the rest)
            // (The offset by FILD, FADD, stored: chopped.)
            setPosition(world_, *o, (util::X87::fromDouble(vm.context().x) + x).f(), (util::X87::fromDouble(vm.context().y) + y).f(), frame_);
        }
        return 0;
    }
    case MoveWaypoint: {
        const float y = vm.popFloat();
        const float x = vm.popFloat();
        if (ServerWaypoint* w = world_.waypoint(vm.popInt())) {
            w->x = (util::X87::fromDouble(vm.context().x) + x).f(); // (FILD, FADD, stored)
            w->y = (util::X87::fromDouble(vm.context().y) + y).f();
        }
        return 0;
    }
    case Distance: {
        const float y2 = vm.popFloat();
        const float x2 = vm.popFloat();
        const float y1 = vm.popFloat();
        const float x1 = vm.popFloat();
        // (On the x87: the differences, squares, sum and root at 53 bits, stored once.)
        const util::X87 dx = util::X87(x1) - x2;
        const util::X87 dy = util::X87(y1) - y2;
        vm.pushFloat(util::sqrt(dx * dx + dy * dy).f());
        return 0;
    }
    case Effect: {
        const float y2 = vm.popFloat();
        const float x2 = vm.popFloat();
        const float y1 = vm.popFloat();
        const float x1 = vm.popFloat();
        const std::uint32_t name = vm.pop();
        const auto& c = vm.context();
        addEvent({ScriptEvent::Kind::Effect, 0, vm.string(name), 0, x1 + static_cast<float>(c.x), y1 + static_cast<float>(c.y),
                  x2 + static_cast<float>(c.x), y2 + static_cast<float>(c.y)});
        effect(vm.string(name), x1, y1, x2, y2, c.x, c.y);
        return 0;
    }
    case Waypoint: { // (the names matched past their "scope:")
        ServerWaypoint* w = world_.waypointMatching(lookupName(vm.string(vm.pop())));
        vm.pushInt(w ? w->id : 0);
        return 0;
    }
    case WaypointGroup:
    case ObjectGroup:
    case WallGroup: {
        // The group of that name (matched past its "scope:") and kind.
        const auto kind = id == ObjectGroup ? ServerGroup::Kind::Objects
                          : id == WaypointGroup ? ServerGroup::Kind::Waypoints
                                                : ServerGroup::Kind::Walls;
        ServerGroup* g = world_.groupMatching(lookupName(vm.string(vm.pop())), kind);
        vm.pushInt(g ? g->id : 0);
        return 0;
    }
    case DestroyChat:
        if (ServerObject* o = popObject()) {
            addEvent({ScriptEvent::Kind::DestroyChat, 0, {}, o->extent});
            world_.chatKill(o);
        }
        return 0;
    case DestroyEveryChat:
        addEvent({ScriptEvent::Kind::DestroyChat});
        world_.chatKill(nullptr);
        return 0;
    // The quest variables (quest_status.hpp): the world's one list, kept from map to map and in a
    // solo .plr; a name without a ':' is the map's ("<map>:<name>"), found ignoring case.
    case SetQuestStatus:      // journalQuestSet: a new one of type 0
    case SetQuestStatusFloat: { // journalQuestSetBool: a new one of type 1
        const std::string name = vm.string(vm.pop());
        const std::uint32_t value = vm.pop();
        setQuestVariable(world_, name, id == SetQuestStatus ? 0u : 1u, value);
        return 0;
    }
    case GetQuestStatus:
    case GetQuestStatusFloat: // (none: 0, the float 0)
        vm.push(questVariableValue(world_, vm.string(vm.pop())));
        return 0;
    case ResetQuestStatus:
        resetQuestVariables(world_, vm.string(vm.pop()));
        return 0;
    case IsTrigger:
    case IsCaller: {
        ServerObject* o = popObject();
        const std::uint32_t who = id == IsTrigger ? vm.trigger() : vm.caller();
        ServerObject* w = who ? world_.byExtent(who) : nullptr;
        vm.push(o && w && o->scriptId == w->scriptId ? 1 : 0);
        return 0;
    }
    case Music: {
        const std::int32_t volume = vm.popInt();
        const std::int32_t music = vm.popInt();
        addEvent({ScriptEvent::Kind::Music, 0, {}, 0, 0, 0, 0, 0, music, volume});
        // A solo game's host sets its own (Snd_SetMusic: host_client.hpp); else
        // MSG_MUSIC_EVENT 0xE5 {u8 music, u8 volume} to everyone.
        if (world_.gameFlags & GameFlag::Solo) {
            if (world_.hostClient) {
                world_.hostClient->music(music, volume);
            }
        } else {
            world_.broadcasts().push_back({{0xE5, static_cast<std::uint8_t>(music), static_cast<std::uint8_t>(volume)}, true, 0});
        }
        return 0;
    }
    case GetTrigger:
    case GetCaller: {
        const std::uint32_t who = id == GetTrigger ? vm.trigger() : vm.caller();
        ServerObject* w = who ? world_.byExtent(who) : nullptr;
        vm.pushInt(w ? w->scriptId : 0);
        return 0;
    }
    case NoWallSound:
        wallSounds_ = vm.pop() == 0;
        return 0;
    case SetCallback: {
        // SetCallback(monster, event, function): the function's index into that
        // event's block (the block's flags kept): 3 enemy sighted, 4 looking for an
        // enemy, 5 death, 6 change of focus, 7 hit, 8 retreat, 9 collision, 10 enemy heard, 11
        // end of waypoint, 13 lost enemy; other events nothing.
        const std::uint32_t function = vm.pop();
        const std::uint32_t event = vm.pop();
        ServerObject* o = popObject();
        if (!o || !(o->classes & things::ObjectClass::Monster) || !o->monster) {
            return 0;
        }
        auto& m = *o->monster;
        ServerObject::ScriptBlock* block = nullptr;
        switch (event) {
        case 3: block = &m.onEnemySighted; break;
        case 4: block = &m.onLookingForEnemy; break;
        case 5: block = &m.onDeath; break;
        case 6: block = &m.onChangeFocus; break;
        case 7: block = &m.onIsHit; break;
        case 8: block = &m.onRetreat; break;
        case 9: block = &m.onCollision; break;
        case 10: block = &m.onHearEnemy; break;
        case 11: block = &m.onEndOfWaypoint; break;
        case 13: block = &m.onLostEnemy; break;
        default: return 0;
        }
        const auto& functions = vm_.file().functions;
        block->function = function < functions.size() ? functions[function].name : std::string{};
        return 0;
    }
    // Dialog and the journal (script_builtins_dialog.cpp).
    case 126: return dialogBuiltin(id, vm); // SetDialog
    case 127: return dialogBuiltin(id, vm); // CancelDialog
    case 128: return dialogBuiltin(id, vm); // StoryPic
    case 129: return dialogBuiltin(id, vm); // TellStory
    case 130: return dialogBuiltin(id, vm); // StartDialog
    case 157: return dialogBuiltin(id, vm); // JournalEntry
    case 158: return dialogBuiltin(id, vm); // JournalDelete
    case 159: return dialogBuiltin(id, vm); // JournalEdit
    case 171: return dialogBuiltin(id, vm); // GetAnswer
    case 177: return dialogBuiltin(id, vm); // IsTalking
    case 193: builtinIsTrading(world_, vm); return 0;                   // IsTrading (trade.hpp)
    case 195: builtinSetShopkeeperText(world_, vm, popObject); return 0; // SetShopkeeperText (trade.hpp)
    // The cinematic builtins (script_builtins_screen.cpp).
    case 114: case 115: case 135: case 136: case 137: case 176: case 187: case 196: case 202: case 203: case 204: case 207: case 208:
        return screenBuiltin(id, vm);
    case 174: // AutoSave: a solo game's AUTOSAVE asked for, at once (requestSave(1, 0, 0)).
        if (world_.gameFlags & GameFlag::Solo) {
            world_.saveName = "AUTOSAVE"; // (setSaveFileName)
            requestSave(world_, true, 0, 0, frame_);
        }
        return 0;
    // S5's creature and object builtins (script_builtins_creatures.cpp).
    case 13: case 14: case 29: case 30: case 33: case 51: case 52: case 55: case 56: case 57: case 58:
    case 69: case 70: case 71: case 72: case 73: case 74: case 75: case 77: case 78: case 79: case 81:
    case 82: case 83: case 84: case 85: case 87: case 96: case 97: case 98: case 105: case 106: case 107:
    case 108: case 131: case 132: case 133: case 134: case 138: case 139: case 140: case 141: case 142:
    case 143: case 144: case 145: case 146: case 147: case 148: case 149: case 150: case 151: case 152:
    case 153: case 154: case 155: case 156: case 160: case 161: case 162: case 163: case 164: case 165:
    case 166: case 167: case 168: case 169: case 170: case 173: case 186: case 188: case 191: case 197:
    case 198: case 199: case 200: case 201:
        return creatureBuiltin(id, vm);
    // IsFriendly / IsPet (the community's Unknownb8 / Unknownb9): the object's subclass bit 0x100
    // (MIGRATE: MakeFriendly sets it, MakeEnemy clears it) / 0x80 (MONITOR: BecomePet's,
    // BecomeEnemy's), whatever its class. With no object they push the register they saved on
    // entry: what the builtin call left in it, 0 from an ordinary function, and from a prefab
    // function the pointer its offset check stopped at, one past the end of the offset builtins'
    // table.
    case IsFriendly:
    case IsPet:
        if (const ServerObject* o = popObject()) {
            vm.push(id == IsFriendly ? (o->subclasses >> 8) & 1u : (o->subclasses & 0xFFu) >> 7);
        } else {
            constexpr std::uint32_t kOffsetTableEnd = 0x5C3408u; // (the game's value)
            vm.push(vm.context().prefab ? kOffsetTableEnd : 0u);
        }
        return 0;
    // IsGameBall / IsCrown: whether the object is of that type. With no object, nothing is pushed.
    case IsGameBall:
    case IsCrown:
        if (const ServerObject* o = popObject()) {
            vm.push(o->typeName == (id == IsGameBall ? "GameBall" : "Crown") ? 1u : 0u);
        }
        return 0;
    // The table's empty ones: their arguments popped (an object looked up and let be), nothing
    // done, nothing pushed.
    case Unused1f:
    case Unused20:
        popObject();
        return 0;
    case Unused50:
        return 0;
    case Unused58: case Unused59: case Unused5a: case Unused5b: case Unused5c: case Unused5d: case Unused74:
        vm.pop();
        vm.pop();
        return 0;
    case Unused5e:
        // The string's text looked up (Str_GetString) and its pointer put in a table of 1024
        // that nothing reads; its index pushed: 0, 1, 2... from the script's load, 1023 once
        // the table is full.
        vm.pop();
        vm.pushInt(static_cast<std::int32_t>(wideStrings_ < 0x400 ? wideStrings_++ : 0x3FF));
        return 0;
    case GetCharacterData: {
        // The host's own character (getHostInfoPtr: the join block its client made, its
        // PlayerInfo): 0 speed, 1 strength, 2 max mana, 3 max health (u32s), 4 its class,
        // 5 female (signed bytes); else 0. (No host: ours 0s.)
        const std::int32_t what = vm.popInt();
        const ServerObject* host = hostPlayer(world_);
        const auto* info = host && host->player ? &host->player->info : nullptr;
        const auto u32At = [&](std::size_t at) {
            return info ? static_cast<std::uint32_t>((*info)[at]) | static_cast<std::uint32_t>((*info)[at + 1]) << 8 |
                              static_cast<std::uint32_t>((*info)[at + 2]) << 16 | static_cast<std::uint32_t>((*info)[at + 3]) << 24
                        : 0u;
        };
        const auto s8At = [&](std::size_t at) { return info ? static_cast<std::int32_t>(static_cast<std::int8_t>((*info)[at])) : 0; };
        switch (what) {
        case 0: vm.push(u32At(0x32)); break;
        case 1: vm.push(u32At(0x36)); break;
        case 2: vm.push(u32At(0x3A)); break;
        case 3: vm.push(u32At(0x3E)); break;
        case 4: vm.pushInt(s8At(0x42)); break;
        case 5: vm.pushInt(s8At(0x43)); break;
        default: vm.push(0); break;
        }
        return 0;
    }
    case GetHolder: {
        // Its holder's script id. (Not held: the exe reads through a null holder and crashes, an
        // original bug; ours 0.)
        ServerObject* o = popObject();
        const ServerObject* h = o && o->holder ? world_.byExtent(o->holder) : nullptr;
        vm.pushInt(h ? h->scriptId : 0);
        return 0;
    }
    case ClearMessages: // (SrvNet_SendMessagesKill) a player's: MSG_MESSAGES_KILL to its client, reliably
        if (ServerObject* o = popObject(); o && (o->classes & things::ObjectClass::Player) && o->player) {
            world_.broadcasts().push_back({{0xCB}, true, o->extent});
        }
        return 0;
    case ChangeScore: { // ChangeScore(player, by): its score and its team's up by `by`, told to all
        const std::int32_t by = vm.popInt();
        ServerObject* o = popObject();
        if (!o || !(o->classes & things::ObjectClass::Player) || !o->player) {
            return 0;
        }
        // (playerSubLessons(-by) for by < 1, changeScore(by) else: lessons + by either way, in 32
        // bits.)
        o->player->lessons = static_cast<int>(static_cast<std::uint32_t>(o->player->lessons) + static_cast<std::uint32_t>(by));
        if (ServerTeam* t = o->team ? teamById(world_, o->team) : nullptr) {
            setTeamScore(world_, *t, static_cast<int>(static_cast<std::uint32_t>(t->lessons) + static_cast<std::uint32_t>(by)));
        }
        reportLesson(world_, *o);
        return 0;
    }
    case GetScore: { // a player's score, else 0
        const ServerObject* o = popObject();
        vm.pushInt(o && (o->classes & things::ObjectClass::Player) && o->player ? o->player->lessons : 0);
        return 0;
    }
    default:
        break;
    }
    // Not implemented yet: its arguments off, a 0 for its result.
    ++unimplemented_[id];
    if (id < kSignatures.size()) {
        for (int i = 0; i < kSignatures[id].args; ++i) {
            vm.pop();
        }
        if (kSignatures[id].returns) {
            vm.push(0);
        }
    }
    return 0;
}

} // namespace nox::server
