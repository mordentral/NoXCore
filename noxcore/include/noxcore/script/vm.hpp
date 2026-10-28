// The NoxScript interpreter, as GAME.EXE's server runs it (script_callByIndex).
//
// - One value stack for everything, 1024 words (a push onto a full stack is lost; a pop from an
//   empty one reads the bottom word again).
// - A function's locals are its own, not a call's: one set per function, kept between calls (a
//   recursive call shares them). Its arguments are popped into its first words, the last
//   pushed first.
// - Function 1 (GLOBAL)'s locals are the globals. A negative variable index is the trigger
//   object's script variable, laid out by function 0's symbols.
// - Strings are ids into one table: the file's constants, then each string a concatenation or
//   a builtin makes (at most 1024 in all; past that the last id is handed out again). The ones
//   made at run time live until the host drops them (freeRuntimeStrings: the game does at the
//   end of each object-handler call, scriptCallByEventBlock), and their ids are then reused.
// - A builtin returning 1 ends the function that called it; so does an array index out of
//   bounds, after pushing the element (IndexInt: past size - 1; IndexFloat and IndexPtr: past
//   size, the game's off-by-one).
// - Float instructions give what the game's x87 gives in its chop mode (each result rounded
//   towards zero; compares with a NaN as the FPU's flags read them: vm.cpp).
// - Object variables: GAME.EXE never allocates an object's script-variable words (the pointer is
//   null from the object's creation), so any object-variable load or store faults there. The host
//   keeps them per object as the VM's addressing implies (function 0's word offsets), which only
//   NoX Core's own scripts can rely on.
//
// Builtins act on the server's world, so they're the host's (Host::builtin); the VM only runs
// the code. Where the game would crash (a division by zero, a variable outside its function's
// locals, no trigger object) this gives 0 instead, and a step and depth budget stops a script
// that would hang the game (the budgets: kMaxSteps below).
#pragma once

#include "noxcore/script/script_file.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nox::script {

class Vm;

class Host {
public:
    virtual ~Host() = default;
    /// Runs builtin `id`: pops its arguments from the VM's stack (the last first) and pushes
    /// its result, if any. 1 ends the calling function (StopScript and the like); the rest
    /// return 0.
    virtual int builtin(std::uint32_t id, Vm& vm) = 0;
    /// The trigger object's script variable `word` (0 when there's none).
    virtual std::uint32_t objectVar(std::int32_t word) { (void)word; return 0; }
    virtual void setObjectVar(std::int32_t word, std::uint32_t value) { (void)word; (void)value; }
};

/// What a prefab function ("Name%Prefix%X%Y") gives the builtin being called: a
/// prefix for the name lookups (Wall, Object, Waypoint, the groups), an offset for Wall,
/// MoveObject, MoveWaypoint, PushObject and Walk. Empty / zero otherwise.
struct BuiltinContext {
    std::string_view prefix;
    std::int32_t x = 0;
    std::int32_t y = 0;
    /// The calling function is a prefab (whatever the builtin): the game's builtin call then
    /// goes through its prefix and offset checks, which leave a register some builtins push.
    bool prefab = false;
};

class Vm {
public:
    static constexpr std::size_t kStackWords = 0x400;
    static constexpr std::size_t kMaxStrings = 0x400;
    /// The step budgets. GAME.EXE has none: a script that loops for ever hangs its server
    /// (vanilla_deviations.md, VD-41). Instructions are counted, never timed, so a run stops at
    /// the same instruction every time (replays and tests stay the same).
    /// - kMaxSteps: what one top-level call may run, the script calls it makes and the handlers
    ///   its builtins set off (a call made while one runs) included. The stock maps' largest
    ///   call runs about 2,100.
    /// - kMaxFrameSteps: what every top-level call between two newFrame()s (a server frame) may
    ///   run between them. Once it's spent, each further call that frame stops at its first
    ///   instruction. The stock maps' busiest frame runs about 2,100.
    /// A call that runs out is stopped there, every script call it was in unwinding with it
    /// (call() gives false). It's never resumed, so the value stack is emptied: the words it left
    /// would hold up the map's later handlers, which wait for an empty stack.
    static constexpr std::uint64_t kMaxSteps = 1'000'000;
    static constexpr std::uint64_t kMaxFrameSteps = 2'000'000;
    /// The depth budgets (GAME.EXE has none: it runs out of stack and crashes, OB-401):
    /// - kMaxDepth: script functions nested in one top-level call, those of the handlers its
    ///   builtins set off included (they go on from the depth they were set off at). About 256
    ///   bytes of stack each in a release build. Stock maps nest at most 3.
    /// - kMaxNested: handlers nested in one top-level call (a call made while one runs). About 2
    ///   KB of stack each (a Damage setting off a generator's onHurt), on server threads of 1 MB:
    ///   the two at their limits use about 128 KB. Stock maps nest at most 2.
    /// Either reached stops the whole top-level call, as the step budget does.
    static constexpr int kMaxDepth = 256;
    static constexpr int kMaxNested = 32;

    /// Why the last top-level call stopped.
    enum class Stop : std::uint8_t {
        None,       ///< it didn't: it ended (or there was nothing to run)
        Steps,      ///< it ran kMaxSteps
        FrameSteps, ///< the frame's kMaxFrameSteps ran out in it (or before it)
        Depth,      ///< its script calls nested past kMaxDepth
        Nested,     ///< the handlers its builtins set off nested past kMaxNested
    };

    Vm(const ScriptFile& file, Host& host);

    const ScriptFile& file() const noexcept { return *file_; }

    /// Runs function `index` with these caller and trigger objects (0: none), its arguments
    /// already pushed. False if the step or depth budget stopped it. Called while another call
    /// runs (a handler one of its builtins set off), it runs on that call's step and depth
    /// budgets.
    bool call(std::size_t index, std::uint32_t caller = 0, std::uint32_t trigger = 0);
    bool call(std::string_view name, std::uint32_t caller = 0, std::uint32_t trigger = 0);
    /// NoX Core's editor only (hot reload, server/script_reload.hpp; never in a game): runs the
    /// code words [begin, end) of function `index` as a call of it (no arguments popped), ending
    /// at `end` as at a Return. Hot reload runs only new globals' initialisers so, from
    /// GLOBAL's code (whose initialisers are straight-line code, no jumps).
    bool callRange(std::size_t index, std::size_t begin, std::size_t end);

    /// A new frame: kMaxFrameSteps afresh (the server's, at each frame's start).
    void newFrame() noexcept { frameSteps_ = 0; }
    /// The instructions the top-level calls have run since newFrame().
    std::uint64_t frameSteps() const noexcept { return frameSteps_; }
    /// The instructions the last top-level call ran, and why it stopped (if it did).
    std::uint64_t lastSteps() const noexcept { return lastSteps_; }
    Stop lastStop() const noexcept { return lastStop_; }
    /// A call is running (a call() now would be nested in it).
    bool running() const noexcept { return active_ != 0; }

    // For builtins.
    void push(std::uint32_t v) noexcept;
    std::uint32_t pop() noexcept;
    void pushInt(std::int32_t v) noexcept { push(static_cast<std::uint32_t>(v)); }
    std::int32_t popInt() noexcept { return static_cast<std::int32_t>(pop()); }
    void pushFloat(float v) noexcept { push(std::bit_cast<std::uint32_t>(v)); }
    float popFloat() noexcept { return std::bit_cast<float>(pop()); }
    std::size_t stackTop() const noexcept { return top_; }
    /// The stack emptied (its top back to 0; its words stay, so a pop then reads the bottom one
    /// as the game's does).
    void resetStack() noexcept { top_ = 0; }

    /// A string by id ("" for one there isn't: the game's slot is then NULL).
    const std::string& string(std::uint32_t id) const noexcept;
    /// A new string's id (script_getString).
    std::uint32_t addString(std::string s);
    std::size_t stringCount() const noexcept { return strings_.size(); }
    /// The file's constants: the strings that are never dropped.
    std::size_t constantCount() const noexcept { return constants_; }
    /// Every string made at run time dropped, the count back to the constants' (their ids are
    /// handed out again).
    void freeRuntimeStrings() noexcept;

    std::uint32_t caller() const noexcept { return caller_; }
    std::uint32_t trigger() const noexcept { return trigger_; }
    const BuiltinContext& context() const noexcept { return context_; }

    /// The globals (function 1's locals), as a savegame keeps them.
    std::span<std::uint32_t> globals() noexcept;
    /// A function's locals.
    std::span<std::uint32_t> locals(std::size_t function) noexcept { return locals_.at(function); }

    /// The whole value stack (its words below and above the top), for hot reload's copy of a VM's
    /// state onto its successor (the editor's only).
    const std::array<std::uint32_t, kStackWords>& stackWords() const noexcept { return stack_; }
    void setStack(const std::array<std::uint32_t, kStackWords>& words, std::size_t top) noexcept
    {
        stack_ = words;
        top_ = top < kStackWords ? top : kStackWords;
    }

private:
    /// Runs a function; false once the budget's gone (everything unwinds then). The top-level
    /// call of callRange starts at `begin` and stops at `end` (else 0 and the code's end).
    bool run(std::size_t index, int depth, std::size_t begin = 0, std::size_t end = SIZE_MAX);
    /// call / callRange: a top-level call's budget set up and accounted for (a nested one runs
    /// on its caller's).
    bool enter(std::size_t index, std::size_t begin, std::size_t end);
    /// A depth budget reached: the whole top-level call stops at its next instruction (as at the
    /// step budget's end, whatever builtin the stop was in).
    void halt(Stop why) noexcept;
    /// LoadVar's variable: a symbol index (negative: an object variable).
    std::uint32_t loadVar(std::size_t fn, bool global, std::int32_t var) const;
    /// A store's word: an offset (negative: the object variable at -offset).
    std::uint32_t read(std::size_t fn, bool global, std::int32_t offset) const;
    void write(std::size_t fn, bool global, std::int32_t offset, std::uint32_t value);
    /// strcmp of two strings by id.
    int compare(std::uint32_t left, std::uint32_t right) const;

    // (Pointers, not references, so a VM can be assigned over: hot reload's swap.)
    const ScriptFile* file_;
    Host* host_;
    std::array<std::uint32_t, kStackWords> stack_{};
    std::size_t top_ = 0;
    std::vector<std::string> strings_;
    std::size_t constants_ = 0;
    std::vector<std::vector<std::uint32_t>> locals_;
    std::uint32_t caller_ = 0;
    std::uint32_t trigger_ = 0;
    BuiltinContext context_;
    std::uint64_t steps_ = 0;      ///< the running top-level call's instructions
    std::uint64_t limit_ = 0;      ///< what it may run: kMaxSteps, or less with the frame's nearly spent
    std::uint64_t frameSteps_ = 0; ///< since newFrame()
    std::uint64_t lastSteps_ = 0;
    Stop lastStop_ = Stop::None;
    Stop halted_ = Stop::None;     ///< the running call stopped by a depth budget (halt())
    int active_ = 0;               ///< calls running (nested ones too)
    int depth_ = 0;                ///< the running function's depth (a nested call's go on from it)
    bool stopped_ = false;
};

} // namespace nox::script
