// One Luau VM for one script mode's half (its server scripts on the host, later its UI on a
// client), sandboxed and capped:
//   - Luau's own sandbox (luaL_sandbox: the libraries and globals read-only), less getfenv,
//     setfenv and loadstring; the mode's code runs in a thread of its own (its own globals);
//   - memory: our allocator refuses past the cap (the call fails, "ran out of memory"; the VM
//     goes on);
//   - time: each call from the host has a budget, counted at the VM's safepoints (calls and loop
//     back edges) and checked against the thread's own CPU time (never the wall clock: a host
//     asleep or suspended mid-call doesn't spend it); past it the call errors ("ran too long"), and
//     keeps erroring at every safepoint till it ends (a script's own pcall can't keep it going).
// The host's own Lua API calls around a call (pushing arguments, reading results) can still raise
// a Lua error (memory): each host entry point catches it (std::exception) and resets the stack.
// The host calls into it only on one thread (the server's).
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

struct lua_State;

namespace nox::modes::script {

class LuauVm {
public:
    struct Limits {
        std::size_t memoryBytes = 64u << 20;          ///< the VM's whole heap
        std::uint64_t safepointsPerCall = 4'000'000;  ///< (calls and loop turns, roughly)
        std::chrono::milliseconds timePerCall{250};
        /// A built-in mode's script (the program's own, trusted: the Tide's, the Delve's): a mod's
        /// memory, no count of safepoints, a minute's CPU time a call (a heavy rule on a slow or
        /// stalled machine isn't ended, and what it does doesn't hang on the machine's speed).
        static Limits builtIn() noexcept
        {
            Limits l;
            l.safepointsPerCall = ~std::uint64_t{0};
            l.timePerCall = std::chrono::minutes(1);
            return l;
        }
    };

    explicit LuauVm(Limits limits);
    ~LuauVm();
    LuauVm(const LuauVm&) = delete;
    LuauVm& operator=(const LuauVm&) = delete;

    /// The mode's thread: where its chunks run and the host calls its functions.
    lua_State* thread() const noexcept { return thread_; }
    /// A new thread with fresh globals of its own (a reload's: the old code's functions keep the
    /// globals they were made with, so they still run if the new code fails).
    void freshThread();
    /// Whoever the host functions (the toolbox) answer for: kept for them (owner(L)).
    void setOwner(void* owner) noexcept { owner_ = owner; }
    static void* owner(lua_State* L);

    /// `source` compiled as chunk `name` ("server/main.luau") and run with no arguments, its
    /// `nresults` results left on the thread's stack. False: the compile's or the run's error in
    /// `error` (nothing left on the stack).
    bool run(std::string_view name, std::string_view source, int nresults, std::string& error);
    /// The function below its `nargs` arguments on the thread's stack called, protected and on a
    /// fresh budget; `nresults` results left. False: its error in `error`.
    bool call(int nargs, int nresults, std::string& error);

    std::size_t memoryUsed() const noexcept { return used_; }
    const Limits& limits() const noexcept { return limits_; }

    /// The calling thread's own CPU time so far (user and kernel), what the time budgets count:
    /// time the thread spends asleep, waiting or suspended isn't in it. (Windows counts it in its
    /// timer's ticks, about 16 ms.)
    static std::chrono::nanoseconds threadCpuTime() noexcept;

private:
    static void* allocate(void* ud, void* ptr, std::size_t osize, std::size_t nsize);
    static void interrupt(lua_State* L, int gc);
    void beginBudget();

    Limits limits_;
    lua_State* main_ = nullptr;
    lua_State* thread_ = nullptr;
    int threadRef_ = 0;
    void* owner_ = nullptr;
    std::size_t used_ = 0;
    std::size_t collectedAt_ = 0; ///< (the heap after the last full collection a call began with)
    std::uint64_t safepoints_ = 0;
    /// (The call's CPU time at its first clock check; -1: not read yet. Read lazily, so a short
    /// call costs no clock read at all.)
    std::chrono::nanoseconds cpuStart_{-1};
    bool inCall_ = false;
    bool spent_ = false; ///< (this call's budget gone: every safepoint errors till it ends)
};

} // namespace nox::modes::script
