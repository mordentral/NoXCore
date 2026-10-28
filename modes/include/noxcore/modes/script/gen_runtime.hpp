// The map generator's own Luau VM: the design half of
// mapgen runs in it, over the `gen` library of generation primitives (numbers, the C++'s random
// streams, cells and grids, and later the map writer). It isn't a script mode: no `nox` toolbox,
// no World, no host. It reads only what it's given.
//
// Threads: a VM is made for one level, on the thread that builds it (the server's, or one of the
// Delve's workers), and dropped after; no VM is ever shared. The built-in generator scripts
// (modes/common/script/mapgen, built in under the id "mapgen") are compiled once per process and
// shared read-only.
//
// Rounding: every call runs to nearest (a RoundingScope of its own, inside buildArena's): the
// generator is NoX Core's own figuring, the same on every thread and platform. gen.f32 rounds to
// nearest by hand (nearestFloat), never by the conversion's own rounding.
//
// Failures: gen.refuse(text) is a level that can't be built as asked (a FormatError with that
// text: the Delve draws it again). A native block's own exception (the writer's, the NoxScript
// builder's, any block's) comes out as it was thrown, as when the design was C++ (genNative).
// Anything else, a failure of the script itself (an error, the memory, time or safepoint guard),
// is a GenerationFailed: logged, and never taken for a failed draw, so no machine's speed can
// change which level is made.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

struct lua_State;

namespace nox::modes::script {

/// The generator broke (not a refusal): a script error, or a guard reached.
class GenerationFailed : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// A mod's own generator scripts (its gen/*.luau), compiled once as the mod's game starts and
/// shared read-only by its builds on any thread: require finds them before the built-in modules
/// (whose names they may not take).
struct GenModules {
    std::map<std::string, std::string, std::less<>> bytecode; ///< "area" -> its bytecode
    std::string chunkPrefix;                                   ///< "=<mod id>/gen/": errors' file
};

/// `sources` ("area" -> gen/area.luau's text) compiled; none: `error` says why (a name a built-in
/// module has, or one that isn't a-z, 0-9 and _).
std::shared_ptr<const GenModules> compileGenModules(const std::map<std::string, std::string, std::less<>>& sources,
                                                    std::string_view modId, std::string& error);
/// Whether `name` is a built-in generator module's ("arena", "layout"...).
bool builtinGenModule(std::string_view name);

class GenVm {
public:
    /// Guards only: a level never comes near them. `time` is the calling thread's CPU time for the
    /// call (never the wall clock: a host asleep or suspended mid-level doesn't spend it).
    struct Limits {
        std::size_t memoryBytes = 256u << 20;
        std::chrono::milliseconds time{60'000};
        std::uint64_t safepoints = 60'000'000'000ull;
    };

    /// (Two constructors, not a default argument: GCC won't take {} for a nested struct with
    /// member initializers before its enclosing class is complete.)
    GenVm();
    explicit GenVm(Limits limits);
    /// A mod's: its own modules found by require first.
    GenVm(Limits limits, std::shared_ptr<const GenModules> modules);
    ~GenVm();
    GenVm(const GenVm&) = delete;
    GenVm& operator=(const GenVm&) = delete;

    /// The VM's thread (where its chunks run): for pushing arguments and reading results.
    lua_State* state() const noexcept;

    /// require(module) (a built-in generator script, "layout" for layout.luau), then its field
    /// `fn` called with what `push` pushes (it returns how many), `nresults` results read by
    /// `read` (the top of the stack). Throws FormatError for gen.refuse; a native block's own
    /// exception as it was (genNative); GenerationFailed for any other failure of the script.
    void call(std::string_view module, std::string_view fn, const std::function<int(lua_State*)>& push, int nresults,
              const std::function<void(lua_State*)>& read);

    /// A chunk compiled from `source` and run, `nresults` results read by `read` (tests).
    void run(std::string_view name, std::string_view source, int nresults, const std::function<void(lua_State*)>& read);

    /// A global function of the VM's own (a test's block).
    void define(const char* name, int (*fn)(lua_State*));

    /// The script's own memory (Luau's), and the native memory its values hold (genCharge's).
    std::size_t memoryUsed() const noexcept;
    std::size_t nativeUsed() const noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

/// A cell (x, y) as the generator's scripts number it (x * 4096 + y + 2048, y from -2048 to
/// 2047: numeric order is (x, then y)), and back.
double genCellKey(int x, int y) noexcept;
std::pair<int, int> genCellOf(double key) noexcept;

/// For the generator's native bindings:
/// - a refusal raised from a C function (gen.refuse's: a FormatError for the host);
[[noreturn]] void genRefuse(lua_State* L, const std::string& text);
/// - a native exception carried out through the script to the host's call, as it was;
[[noreturn]] void genThrow(lua_State* L, std::exception_ptr e);
/// - native memory a script's value holds (a cell set's bits, kept outside Luau's allocator),
///   counted with the script's own against the memory guard: charged when the value is made (a
///   collection first when the two together pass the guard, then the guard's failure; a
///   collector's step as the charges grow, so the collector sees them), released when it's
///   collected;
void genCharge(lua_State* L, std::size_t bytes);
void genRelease(lua_State* L, std::size_t bytes) noexcept;
/// - the streams given as arguments.
std::minstd_rand& genMinstd(lua_State* L, int arg);
std::mt19937& genMt19937(lua_State* L, int arg);

/// `f` (native work only: no Lua API calls in it), any exception it throws carried out of the
/// script to the host as it was (genThrow).
template <class F>
auto genNative(lua_State* L, F&& f) -> decltype(f())
{
    std::exception_ptr e;
    try {
        return f();
    } catch (...) {
        e = std::current_exception();
    }
    genThrow(L, std::move(e));
}

} // namespace nox::modes::script
