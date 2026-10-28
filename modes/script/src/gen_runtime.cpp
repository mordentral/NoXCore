// The map generator's VM and its `gen` library (gen_runtime.hpp).
#include "noxcore/modes/script/gen_runtime.hpp"

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/mapgen/layout.hpp"
#include "noxcore/modes/script/builtin_scripts.hpp"
#include "noxcore/modes/script/luau_vm.hpp"
#include "noxcore/modes/script/script_data.hpp"
#include "noxcore/util/random.hpp"
#include "noxcore/util/x87.hpp"

#include "lua.h"
#include "luacode.h"
#include "lualib.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <vector>

namespace nox::mapgen::detail {
/// The map generator's blocks' part of `gen` (gen_bindings.cpp), into the table on the stack's top.
void openGenBlocks(lua_State* L);
} // namespace nox::mapgen::detail

namespace nox::modes::script {

namespace {

/// The built-in generator scripts' id (builtin_scripts.hpp).
constexpr std::string_view kScripts = "mapgen";

/// Cells as one number: x * 4096 + y + 2048 (y from -2048 to 2047), so numeric order is
/// std::set<Cell>'s (x, then y).
constexpr std::int64_t kRow = 4096;
constexpr std::int64_t kYOff = 2048;

// ---- The bytecode, compiled once per process ----------------------------------------------

struct Compiled {
    std::map<std::string, std::string, std::less<>> modules; ///< "layout" -> its bytecode
};

std::string compile(std::string_view source)
{
    lua_CompileOptions options{};
    options.optimizationLevel = 1;
    options.debugLevel = 1; // (line numbers in errors)
    std::size_t size = 0;
    std::unique_ptr<char, decltype(&std::free)> bytecode(luau_compile(source.data(), source.size(), &options, &size), &std::free);
    if (!bytecode) {
        throw GenerationFailed("mapgen: a generator script couldn't be compiled");
    }
    return std::string(bytecode.get(), size);
}

/// (Its own scope, to nearest: the compiler folds constants with the thread's rounding.)
NOX_NOINLINE void compileAll(Compiled& out)
{
    const util::RoundingScope nearest(false);
    for (const BuiltinScript& s : builtinScripts()) {
        if (s.mode != kScripts) {
            continue;
        }
        std::string name(s.path); // ("mapgen/layout.luau": "layout")
        if (name.starts_with("mapgen/")) {
            name.erase(0, 7);
        }
        if (name.ends_with(".luau")) {
            name.resize(name.size() - 5);
        }
        out.modules.emplace(std::move(name), compile(s.text));
    }
}

const Compiled& compiled()
{
    // (never destroyed (made on the heap, never freed): a Delve level still being built on a
    // worker as the program exits must not read it after its destruction)
    static Compiled& all = *new Compiled();
    static std::once_flag once;
    std::call_once(once, [] { compileAll(all); });
    return all;
}

// ---- The VM's own state -------------------------------------------------------------------

} // namespace

struct GenVm::Impl {
    Limits limits;
    /// The native memory the script's values hold (genCharge), and what's been charged since the
    /// collector's last step. (Ahead of vm: the values' release at its closing still finds them.)
    std::size_t nativeBytes = 0;
    std::size_t sinceStep = 0;
    LuauVm vm;
    int requiredRef = 0;
    std::uint64_t safepoints = 0;
    std::chrono::nanoseconds cpuStart{}; ///< the thread's CPU time at the call's start
    bool guardHit = false;
    std::string guard;   ///< which guard, when one was hit
    bool refused = false;
    std::string refusal; ///< gen.refuse's text
    /// The native blocks' exceptions raised in this call (genThrow), each raised as a pointer to
    /// its own entry: only the one whose pointer is the error reaching the host is rethrown (one a
    /// script's pcall caught and moved past is not).
    std::deque<std::exception_ptr> natives;
    std::exception_ptr native; ///< the one that reached the host's call
    /// A mod's own modules (none: the built-in ones alone).
    std::shared_ptr<const GenModules> modules;

    explicit Impl(Limits l)
        : limits(l)
        , vm(LuauVm::Limits{.memoryBytes = l.memoryBytes, .safepointsPerCall = l.safepoints, .timePerCall = l.time})
    {
    }

    void begin()
    {
        safepoints = 0;
        cpuStart = LuauVm::threadCpuTime();
        guardHit = false;
        guard.clear();
        refused = false;
        refusal.clear();
        natives.clear();
        native = nullptr;
    }

    /// After a failed call: the refusal as a FormatError, anything else a GenerationFailed (logged).
    [[noreturn]] void fail(const std::string& error) const
    {
        if (native && !guardHit) {
            std::rethrow_exception(native); // (a block's: its own type, as a C++ caller would see it)
        }
        if (refused && !guardHit) {
            throw FormatError(refusal);
        }
        const std::string what = "mapgen: the generator failed (" + (guardHit ? guard : error) + ")";
        std::fprintf(stderr, "%s\n", what.c_str());
        throw GenerationFailed(what);
    }
};

namespace {

GenVm::Impl& implOf(lua_State* L)
{
    return *static_cast<GenVm::Impl*>(LuauVm::owner(L));
}

/// The guards (in place of the mode VMs' budget): time and safepoints for the whole call; once
/// hit, every safepoint errors till the call ends. The time is the thread's own CPU time since the
/// call began (LuauVm::threadCpuTime), never the wall clock: a host asleep or suspended mid-level
/// never trips it, only a level's own work does (the blocks' included).
void interrupt(lua_State* L, int gc)
{
    if (gc >= 0) {
        return;
    }
    GenVm::Impl& impl = implOf(L);
    if (impl.guardHit) {
        luaL_error(L, "%s", impl.guard.c_str());
    }
    ++impl.safepoints;
    const bool overCount = impl.safepoints > impl.limits.safepoints;
    const bool overTime = (impl.safepoints & 1023u) == 0 && LuauVm::threadCpuTime() - impl.cpuStart > impl.limits.time;
    if (overCount || overTime) {
        impl.guardHit = true;
        impl.guard = overCount ? "its safepoint guard was reached" : "its time guard was reached";
        luaL_error(L, "%s", impl.guard.c_str());
    }
}

// ---- Numbers -------------------------------------------------------------------------------

/// A whole number from 0 to 2^32 - 1.
std::uint32_t checkU32(lua_State* L, int arg)
{
    const double v = luaL_checknumber(L, arg);
    luaL_argcheck(L, v >= 0 && v <= 4294967295.0 && v == std::floor(v), arg, "a whole number from 0 to 4294967295");
    return static_cast<std::uint32_t>(v);
}

int genF32(lua_State* L)
{
    lua_pushnumber(L, static_cast<double>(nearestFloat(luaL_checknumber(L, 1))));
    return 1;
}

int genMul32(lua_State* L)
{
    const std::uint32_t a = checkU32(L, 1);
    const std::uint32_t b = checkU32(L, 2);
    lua_pushnumber(L, static_cast<double>(static_cast<std::uint32_t>(a * b)));
    return 1;
}

/// What gen.refuse raises: this, never a text, so only a refusal reaching the host's call (not
/// one a script's own pcall caught, nor an error that reads like one) counts as one.
char kRefusal;

int genRefuseFn(lua_State* L)
{
    GenVm::Impl& impl = implOf(L);
    impl.refusal = luaL_checkstring(L, 1);
    lua_pushlightuserdata(L, &kRefusal);
    lua_error(L);
}

// ---- Random streams: the engines themselves ------------------------------------------------

template <class Engine>
struct StreamType;
template <>
struct StreamType<std::mt19937> {
    static constexpr const char* kName = "gen.mt19937";
};
template <>
struct StreamType<std::minstd_rand> {
    static constexpr const char* kName = "gen.minstd";
};

template <class Engine>
Engine& checkStream(lua_State* L, int arg)
{
    return *static_cast<Engine*>(luaL_checkudata(L, arg, StreamType<Engine>::kName));
}

template <class Engine>
int streamNext(lua_State* L)
{
    lua_pushnumber(L, static_cast<double>(checkStream<Engine>(L, 1)()));
    return 1;
}

template <class Engine>
int streamBelow(lua_State* L)
{
    Engine& e = checkStream<Engine>(L, 1);
    const double n = luaL_checknumber(L, 2);
    luaL_argcheck(L, n >= 1 && n <= 4294967295.0 && n == std::floor(n), 2, "below: a whole number from 1");
    lua_pushnumber(L, static_cast<double>(static_cast<std::uint64_t>(e()) % static_cast<std::uint64_t>(n)));
    return 1;
}

template <class Engine>
int streamSeed(lua_State* L)
{
    checkStream<Engine>(L, 1).seed(static_cast<typename Engine::result_type>(checkU32(L, 2)));
    return 0;
}

/// The list's 1..n in place, in util::shuffle's order (Microsoft's std::shuffle): the swaps it
/// makes depend only on the draws and the positions, so a list of positions shuffled the same way
/// moves the values exactly as the C++ moves its vector's.
template <class Engine>
int streamShuffle(lua_State* L)
{
    Engine& e = checkStream<Engine>(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    const int n = lua_objlen(L, 2);
    std::vector<std::ptrdiff_t> order(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        order[static_cast<std::size_t>(i)] = i;
    }
    util::shuffle(order.begin(), order.end(), e);
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; ++i) {
        lua_rawgeti(L, 2, static_cast<int>(order[static_cast<std::size_t>(i)]) + 1);
        lua_rawseti(L, -2, i + 1);
    }
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, -1, i);
        lua_rawseti(L, 2, i);
    }
    lua_pop(L, 1);
    return 0;
}

template <class Engine>
int newStream(lua_State* L)
{
    const std::uint32_t seed = checkU32(L, 1);
    void* at = lua_newuserdata(L, sizeof(Engine)); // (both engines trivially destructible)
    new (at) Engine(static_cast<typename Engine::result_type>(seed));
    luaL_getmetatable(L, StreamType<Engine>::kName);
    lua_setmetatable(L, -2);
    return 1;
}

template <class Engine>
void streamMetatable(lua_State* L)
{
    luaL_newmetatable(L, StreamType<Engine>::kName);
    lua_createtable(L, 0, 4);
    static constexpr std::pair<const char*, lua_CFunction> kMethods[] = {
        {"next", streamNext<Engine>}, {"below", streamBelow<Engine>}, {"shuffle", streamShuffle<Engine>}, {"seed", streamSeed<Engine>}};
    for (const auto& [name, fn] : kMethods) {
        lua_pushcfunction(L, fn, name);
        lua_setfield(L, -2, name);
    }
    lua_setreadonly(L, -1, 1);
    lua_setfield(L, -2, "__index");
    lua_pushstring(L, StreamType<Engine>::kName);
    lua_setfield(L, -2, "__type");
    lua_setreadonly(L, -1, 1);
    lua_pop(L, 1);
}

// ---- Cells and grids -----------------------------------------------------------------------

std::int64_t checkKey(lua_State* L, int idx)
{
    const double v = lua_tonumber(L, idx);
    if (!lua_isnumber(L, idx) || v != std::floor(v) || std::fabs(v) > 1e15) {
        luaL_error(L, "a cell key must be a whole number");
    }
    return static_cast<std::int64_t>(v);
}

mapgen::Cell cellOf(std::int64_t k)
{
    std::int64_t x = k / kRow;
    if (k % kRow < 0) {
        --x; // (floored)
    }
    return {static_cast<int>(x), static_cast<int>(k - x * kRow - kYOff)};
}

double keyOf(mapgen::Cell c)
{
    return static_cast<double>(static_cast<std::int64_t>(c.first) * kRow + c.second + kYOff);
}

/// gen.sorted(t): t's keys (whole numbers) in ascending order, as an array.
int genSorted(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<std::int64_t> keys;
    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
        lua_pop(L, 1);
        keys.push_back(checkKey(L, -1));
    }
    std::ranges::sort(keys);
    lua_createtable(L, static_cast<int>(keys.size()), 0);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        lua_pushnumber(L, static_cast<double>(keys[i]));
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
}

// ---- require -------------------------------------------------------------------------------

/// require("name"): a mod's own gen/name.luau first (a mod's VM), else the built-in generator script
/// name.luau, run once, its result kept.
int genRequire(lua_State* L)
{
    GenVm::Impl& impl = implOf(L);
    const std::string name = luaL_checkstring(L, 1);
    lua_getref(L, impl.requiredRef);
    const int cache = lua_gettop(L);
    lua_rawgetfield(L, cache, name.c_str());
    if (!lua_isnil(L, -1)) {
        if (lua_islightuserdata(L, -1)) {
            luaL_error(L, "require: %s requires itself (a cycle)", name.c_str());
        }
        return 1;
    }
    lua_pop(L, 1);
    const std::string* code = nullptr;
    std::string chunk;
    if (impl.modules) {
        if (const auto own = impl.modules->bytecode.find(name); own != impl.modules->bytecode.end()) {
            code = &own->second;
            chunk = impl.modules->chunkPrefix + name + ".luau";
        }
    }
    if (!code) {
        const auto& modules = compiled().modules;
        const auto it = modules.find(name);
        if (it == modules.end()) {
            luaL_error(L, "require: no generator script %s", name.c_str());
        }
        code = &it->second;
        chunk = "=mapgen/" + name + ".luau";
    }
    if (luau_load(L, chunk.c_str(), code->data(), code->size(), 0) != 0) {
        lua_error(L);
    }
    lua_pushlightuserdata(L, &impl); // (loading: a cycle's mark)
    lua_rawsetfield(L, cache, name.c_str());
    lua_call(L, 0, 1);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_pushvalue(L, -1);
    lua_rawsetfield(L, cache, name.c_str());
    return 1;
}

void openGen(GenVm::Impl& impl)
{
    lua_State* L = impl.vm.thread();
    streamMetatable<std::mt19937>(L);
    streamMetatable<std::minstd_rand>(L);
    static constexpr std::pair<const char*, lua_CFunction> kGen[] = {
        {"f32", genF32},
        {"mul32", genMul32},
        {"refuse", genRefuseFn},
        {"mt19937", newStream<std::mt19937>},
        {"minstd", newStream<std::minstd_rand>},
        {"sorted", genSorted},
    };
    lua_createtable(L, 0, static_cast<int>(std::size(kGen)));
    for (const auto& [name, fn] : kGen) {
        lua_pushcfunction(L, fn, name);
        lua_setfield(L, -2, name);
    }
    // (The map generator's own blocks: gen_bindings.cpp.)
    mapgen::detail::openGenBlocks(L);
    lua_setreadonly(L, -1, 1);
    lua_setglobal(L, "gen");
    lua_pushcfunction(L, genRequire, "require");
    lua_setglobal(L, "require");
    lua_newtable(L);
    impl.requiredRef = lua_ref(L, -1);
    lua_pop(L, 1);
}

/// One protected call on the guards, to nearest (its own scope: whatever the caller's rounding).
NOX_NOINLINE bool guardedCall(GenVm::Impl& impl, int nargs, int nresults, std::string& error)
{
    const util::RoundingScope nearest(false);
    impl.begin();
    lua_State* L = impl.vm.thread();
    // (Near the cap, a full collection first: Luau refuses an allocation without collecting.)
    if (impl.vm.memoryUsed() + impl.nativeBytes > impl.limits.memoryBytes / 10 * 6) {
        lua_gc(L, LUA_GCCOLLECT, 0);
    }
    const int status = lua_pcall(L, nargs, nresults, 0);
    if (status == LUA_OK) {
        return true;
    }
    if (status == LUA_ERRMEM) {
        error = "the script ran out of memory (its guard is " + std::to_string(impl.limits.memoryBytes >> 20) + " MB)";
    } else if (lua_islightuserdata(L, -1) && lua_touserdata(L, -1) == &kRefusal) {
        impl.refused = true;
    } else if (const void* p = lua_islightuserdata(L, -1) ? lua_touserdata(L, -1) : nullptr;
               p && std::ranges::any_of(impl.natives, [&](const std::exception_ptr& e) { return &e == p; })) {
        impl.native = *static_cast<const std::exception_ptr*>(p);
        error = "a native block failed";
    } else {
        const char* msg = lua_tostring(L, -1);
        error = msg ? msg : "the script failed";
    }
    lua_pop(L, 1);
    if (status == LUA_ERRMEM) {
        lua_gc(L, LUA_GCCOLLECT, 0);
    }
    return false;
}

} // namespace

double genCellKey(int x, int y) noexcept
{
    return keyOf({x, y});
}

std::pair<int, int> genCellOf(double key) noexcept
{
    return cellOf(static_cast<std::int64_t>(key));
}

void genRefuse(lua_State* L, const std::string& text)
{
    implOf(L).refusal = text;
    lua_pushlightuserdata(L, &kRefusal);
    lua_error(L);
}

void genThrow(lua_State* L, std::exception_ptr e)
{
    GenVm::Impl& impl = implOf(L);
    if (impl.natives.size() >= 64) {
        impl.natives.pop_front(); // (a script catching blocks' failures in a loop: the oldest go; the rest stay put)
    }
    impl.natives.push_back(std::move(e));
    lua_pushlightuserdata(L, &impl.natives.back());
    lua_error(L);
}

namespace {

/// A collector's step each time this much more native memory has been charged.
constexpr std::size_t kStepEvery = 8u << 20;

} // namespace

void genCharge(lua_State* L, std::size_t bytes)
{
    GenVm::Impl& impl = implOf(L);
    impl.nativeBytes += bytes;
    impl.sinceStep += bytes;
    const auto over = [&] { return impl.vm.memoryUsed() + impl.nativeBytes > impl.limits.memoryBytes; };
    if (over()) {
        impl.sinceStep = 0;
        lua_gc(L, LUA_GCCOLLECT, 0); // (the dead values' memory released first)
        if (over()) {
            impl.guardHit = true;
            impl.guard = "its memory guard was reached (" + std::to_string(impl.limits.memoryBytes >> 20) +
                         " MB, the native cell sets' memory counted)";
            luaL_error(L, "%s", impl.guard.c_str());
        }
    } else if (impl.sinceStep >= kStepEvery) {
        lua_gc(L, LUA_GCSTEP, static_cast<int>(impl.sinceStep >> 10));
        impl.sinceStep = 0;
    }
}

void genRelease(lua_State* L, std::size_t bytes) noexcept
{
    if (auto* impl = static_cast<GenVm::Impl*>(LuauVm::owner(L))) {
        impl->nativeBytes -= std::min(bytes, impl->nativeBytes);
    }
}

std::minstd_rand& genMinstd(lua_State* L, int arg)
{
    return checkStream<std::minstd_rand>(L, arg);
}

std::mt19937& genMt19937(lua_State* L, int arg)
{
    return checkStream<std::mt19937>(L, arg);
}

GenVm::GenVm()
    : GenVm(Limits{})
{
}

GenVm::GenVm(Limits limits)
    : GenVm(limits, nullptr)
{
}

GenVm::GenVm(Limits limits, std::shared_ptr<const GenModules> modules)
    : impl_(std::make_unique<Impl>(limits))
{
    (void)compiled(); // (compiled once per process, before any VM uses it)
    impl_->modules = std::move(modules);
    impl_->vm.setOwner(impl_.get());
    lua_callbacks(impl_->vm.thread())->interrupt = &interrupt;
    openGen(*impl_);
}

bool builtinGenModule(std::string_view name)
{
    return compiled().modules.contains(name);
}

std::shared_ptr<const GenModules> compileGenModules(const std::map<std::string, std::string, std::less<>>& sources,
                                                    std::string_view modId, std::string& error)
{
    auto out = std::make_shared<GenModules>();
    out->chunkPrefix = "=" + std::string(modId) + "/gen/";
    for (const auto& [name, source] : sources) {
        const bool ok = !name.empty() && name.size() <= 32 &&
                        std::ranges::all_of(name, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
        if (!ok) {
            error = "gen/" + name + ".luau: a generator module's name is 1-32 of a-z, 0-9 and _";
            return nullptr;
        }
        if (builtinGenModule(name)) {
            error = "gen/" + name + ".luau: \"" + name + "\" is a built-in generator module's name";
            return nullptr;
        }
        const util::RoundingScope nearest(false); // (constants folded as the built-in ones are)
        out->bytecode.emplace(name, compile(source));
    }
    return out;
}

GenVm::~GenVm() = default;

lua_State* GenVm::state() const noexcept
{
    return impl_->vm.thread();
}

std::size_t GenVm::memoryUsed() const noexcept
{
    return impl_->vm.memoryUsed();
}

std::size_t GenVm::nativeUsed() const noexcept
{
    return impl_->nativeBytes;
}

namespace {

/// The VM's stack put back as it was, however the call ends.
struct StackScope {
    lua_State* L;
    int top;
    ~StackScope() { lua_settop(L, top); }
};

} // namespace

void GenVm::call(std::string_view module, std::string_view fn, const std::function<int(lua_State*)>& push, int nresults,
                 const std::function<void(lua_State*)>& read)
{
    lua_State* L = impl_->vm.thread();
    const StackScope scope{L, lua_gettop(L)};
    std::string error;
    // The module (require's, protected) and its function.
    lua_pushcfunction(L, genRequire, "require");
    lua_pushlstring(L, module.data(), module.size());
    if (!guardedCall(*impl_, 1, 1, error)) {
        impl_->fail(error);
    }
    lua_pushlstring(L, fn.data(), fn.size());
    lua_rawget(L, -2);
    if (!lua_isfunction(L, -1)) {
        impl_->fail(std::string(module) + " has no function " + std::string(fn));
    }
    lua_remove(L, -2);
    const int nargs = push ? push(L) : 0;
    if (!guardedCall(*impl_, nargs, nresults, error)) {
        impl_->fail(error);
    }
    if (read) {
        read(L);
    }
}

void GenVm::define(const char* name, int (*fn)(lua_State*))
{
    lua_State* L = impl_->vm.thread();
    lua_pushcfunction(L, fn, name);
    lua_setglobal(L, name);
}

void GenVm::run(std::string_view name, std::string_view source, int nresults, const std::function<void(lua_State*)>& read)
{
    lua_State* L = impl_->vm.thread();
    const int top = lua_gettop(L);
    std::string bytecode;
    {
        const util::RoundingScope nearest(false);
        bytecode = compile(source);
    }
    const std::string chunk = "=" + std::string(name);
    if (luau_load(L, chunk.c_str(), bytecode.data(), bytecode.size(), 0) != 0) {
        const std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "couldn't be loaded";
        lua_settop(L, top);
        impl_->begin();
        impl_->fail(error);
    }
    std::string error;
    if (!guardedCall(*impl_, 0, nresults, error)) {
        lua_settop(L, top);
        impl_->fail(error);
    }
    if (read) {
        read(L);
    }
    lua_settop(L, top);
}

} // namespace nox::modes::script
