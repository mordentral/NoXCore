// The sandboxed, capped Luau VM (luau_vm.hpp).
#include "noxcore/modes/script/luau_vm.hpp"

#include "lua.h"
#include "luacode.h"
#include "lualib.h"

#include <cstdlib>
#include <memory>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <time.h>
#endif

namespace nox::modes::script {

namespace {

LuauVm* vmOf(lua_State* L)
{
    return static_cast<LuauVm*>(lua_callbacks(L)->userdata);
}

} // namespace

std::chrono::nanoseconds LuauVm::threadCpuTime() noexcept
{
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
        return std::chrono::nanoseconds{0};
    }
    const auto ticks = [](const FILETIME& f) {
        return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; // (100 ns each)
    };
    return std::chrono::nanoseconds{static_cast<std::int64_t>((ticks(kernel) + ticks(user)) * 100)};
#else
    timespec t{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) != 0) {
        return std::chrono::nanoseconds{0};
    }
    return std::chrono::seconds{t.tv_sec} + std::chrono::nanoseconds{t.tv_nsec};
#endif
}

void* LuauVm::allocate(void* ud, void* ptr, std::size_t osize, std::size_t nsize)
{
    auto* vm = static_cast<LuauVm*>(ud);
    if (!ptr) {
        osize = 0; // (a new block: whatever osize says, nothing was counted for it)
    }
    if (nsize == 0) {
        std::free(ptr);
        vm->used_ -= osize;
        return nullptr;
    }
    // (Growth past the cap refused: Luau raises its memory error in the script.)
    if (nsize > osize && vm->used_ - osize + nsize > vm->limits_.memoryBytes) {
        return nullptr;
    }
    void* p = std::realloc(ptr, nsize);
    if (p) {
        vm->used_ = vm->used_ - osize + nsize;
    }
    return p;
}

void LuauVm::interrupt(lua_State* L, int gc)
{
    if (gc >= 0) {
        return; // (the collector's step, not the script's)
    }
    LuauVm* vm = vmOf(L);
    if (!vm || !vm->inCall_) {
        return;
    }
    // Once spent, spent for the rest of the call: a script's own pcall that catches the error runs
    // into it again at its next safepoint, and so on out to the host's call.
    if (vm->spent_) {
        luaL_error(L, "the script ran too long (its budget per call is spent)");
    }
    ++vm->safepoints_;
    const bool overCount = vm->safepoints_ > vm->limits_.safepointsPerCall;
    // (The thread's CPU time read every 32 safepoints, the first read the call's start: a loop of
    // slow library calls, which have no safepoints inside them, overruns by a few of those calls at
    // most; the first 32 safepoints aren't counted.)
    bool overTime = false;
    if ((vm->safepoints_ & 31u) == 0) {
        const std::chrono::nanoseconds now = vm->clock_();
        if (vm->cpuStart_.count() < 0) {
            vm->cpuStart_ = now;
        } else {
            // (A mod's server scripts: the budget only past real work, the backstop whatever the
            // work, so a stalled light call goes on. Others, as ever: the budget alone.)
            const std::chrono::nanoseconds took = now - vm->cpuStart_;
            const Limits& l = vm->limits_;
            overTime = (vm->safepoints_ >= l.safepointsBeforeTime && took > l.timePerCall) ||
                       (l.timeBackstop.count() > 0 && took > l.timeBackstop);
        }
    }
    if (overCount || overTime) {
        vm->spent_ = true;
        luaL_error(L, "the script ran too long (its budget per call is spent)");
    }
}

LuauVm::LuauVm(Limits limits)
    : limits_(limits)
{
    main_ = lua_newstate(&LuauVm::allocate, this);
    if (!main_) {
        throw std::runtime_error("a script VM couldn't be made");
    }
    lua_callbacks(main_)->userdata = this;
    lua_callbacks(main_)->interrupt = &LuauVm::interrupt;
    luaL_openlibs(main_);
    // Less of the base library than Luau's sandbox leaves: no environment games (getfenv/setfenv,
    // Luau's documented weak spot), no compiling text at run time (loadstring).
    for (const char* name : {"getfenv", "setfenv", "loadstring"}) {
        lua_pushnil(main_);
        lua_setglobal(main_, name);
    }
    luaL_sandbox(main_); // (the libraries and the globals read-only from here)
    thread_ = lua_newthread(main_);
    threadRef_ = lua_ref(main_, -1);
    lua_pop(main_, 1);
    luaL_sandboxthread(thread_); // (the mode's own globals, over the read-only ones)
}

LuauVm::~LuauVm()
{
    if (main_) {
        lua_close(main_);
    }
}

void LuauVm::freshThread()
{
    // The new thread made and anchored first, the old let go only after: any of these can raise
    // (memory), and the old must still be whole then.
    const int top = lua_gettop(main_);
    lua_State* fresh = nullptr;
    int ref = LUA_NOREF;
    try {
        fresh = lua_newthread(main_);
        ref = lua_ref(main_, -1);
        lua_settop(main_, top);
        luaL_sandboxthread(fresh);
    } catch (...) {
        lua_settop(main_, top);
        if (ref != LUA_NOREF) {
            lua_unref(main_, ref);
        }
        throw;
    }
    lua_unref(main_, threadRef_);
    thread_ = fresh;
    threadRef_ = ref;
}

void* LuauVm::owner(lua_State* L)
{
    const LuauVm* vm = vmOf(L);
    return vm ? vm->owner_ : nullptr;
}

void LuauVm::beginBudget()
{
    safepoints_ = 0;
    cpuStart_ = std::chrono::nanoseconds{-1}; // (read at the call's first clock check)
    spent_ = false;
    inCall_ = true;
}

bool LuauVm::run(std::string_view name, std::string_view source, int nresults, std::string& error)
{
    lua_CompileOptions options{};
    options.optimizationLevel = 1;
    options.debugLevel = 1; // (line numbers in errors)
    std::size_t size = 0;
    std::unique_ptr<char, decltype(&std::free)> bytecode(luau_compile(source.data(), source.size(), &options, &size), &std::free);
    if (!bytecode) {
        error = std::string(name) + ": couldn't be compiled";
        return false;
    }
    const std::string chunk = "=" + std::string(name);
    if (luau_load(thread_, chunk.c_str(), bytecode.get(), size, 0) != 0) {
        // (A compile error comes back as bytecode that fails to load with the message.)
        error = lua_tostring(thread_, -1) ? lua_tostring(thread_, -1) : std::string(name) + ": couldn't be loaded";
        lua_pop(thread_, 1);
        return false;
    }
    return call(0, nresults, error);
}

bool LuauVm::call(int nargs, int nresults, std::string& error)
{
    // Near the cap, a full collection first: Luau refuses an allocation without collecting, and
    // its collector lets the heap grow to about twice what's live before a cycle ends. Not again
    // at every call while what's live stays past the line: once the heap has grown a 32nd of the
    // cap since the last (or at every call within an eighth of the cap, where it always did).
    // A call made while one is running (a toolbox function the script called set off an engine
    // hook: a damage's death telling onKill) shares the outer call's budget: neither starts it
    // afresh nor ends it, so the outer call stays bounded after the inner one returns.
    const bool nested = inCall_;
    if (!nested && used_ > limits_.memoryBytes / 10 * 6 &&
        (used_ > collectedAt_ + limits_.memoryBytes / 32 || used_ > limits_.memoryBytes - limits_.memoryBytes / 8)) {
        lua_gc(main_, LUA_GCCOLLECT, 0);
        collectedAt_ = used_;
    }
    if (!nested) {
        beginBudget();
    }
    const int status = lua_pcall(thread_, nargs, nresults, 0);
    if (!nested) {
        inCall_ = false;
    }
    if (status == LUA_OK) {
        return true;
    }
    if (status == LUA_ERRMEM) {
        error = "the script ran out of memory (its cap is " + std::to_string(limits_.memoryBytes >> 20) + " MB)";
    } else {
        const char* msg = lua_tostring(thread_, -1);
        error = msg ? msg : "the script failed";
    }
    lua_pop(thread_, 1);
    if (status == LUA_ERRMEM) {
        // (What the failed call held is garbage now: collected at once, as Luau doesn't collect
        // before it refuses an allocation; the next call has the room back.)
        lua_gc(main_, LUA_GCCOLLECT, 0);
        collectedAt_ = used_;
    }
    return false;
}

} // namespace nox::modes::script
