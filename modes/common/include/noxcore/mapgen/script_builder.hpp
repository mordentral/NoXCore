// A generated map's NoxScript, built in code. The
// game has no compiler of its own here: maps carry the compiled form (script_file.hpp), so a
// generated room's traps and puzzles are written straight as instructions:
//
//   ScriptBuilder s;
//   const int opened = s.global("vaultOpened");
//   s.function("VaultLever")
//       .loadGlobal(opened).jumpIf("done")
//       .setGlobal(opened, 1)
//       .pushString("VaultWalls").call("WallGroup").call("WallGroupOpen")
//       .label("done");
//   map.scriptObject.code = s.build().serialize();
//
// Builtins go by name (opcodes.cpp's table) and take their arguments pushed in order; functions
// are named by what calls them (a trigger's handler, a timer's) and found by name when it's
// built. Every function ends with Return0 and is checked (script::check) as it's built.
//
// Globals: function 1's locals. The engine presets the first four (self -2, other -1, true 1,
// false 0: ScriptHost::setLanguageGlobals); ours come after. Function 1's code runs at the
// map's load (runGlobalInit): setGlobalAtLoad gives one its first value there.
#pragma once

#include "noxcore/script/opcodes.hpp"
#include "noxcore/script/script_file.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace nox::mapgen {

class ScriptBuilder {
public:
    /// One function's code, an instruction at a time; each call returns *this.
    class Fn {
    public:
        Fn& pushInt(std::int32_t v);
        Fn& pushString(std::string_view s);
        /// A function's index (a timer's or SetCallback's argument), by its name.
        Fn& pushFunction(std::string_view name);
        /// Object("name"), its script id on the stack.
        Fn& object(std::string_view name);
        /// A builtin by name ("WallGroupOpen"); throws for one the table hasn't.
        Fn& call(std::string_view builtin);
        /// Another of the script's functions, by name.
        Fn& callScript(std::string_view name);
        Fn& op(script::Op o);
        Fn& loadGlobal(int g);
        Fn& setGlobal(int g, std::int32_t v);
        /// global = the value pushed after it: storeGlobal(g) then the value's pushes, then
        /// endStore() (the store leaves the value on the stack, as the game's compiler does).
        Fn& storeGlobal(int g);
        Fn& endStore();
        Fn& loadLocal(int l);
        Fn& label(std::string_view name);
        Fn& jumpIf(std::string_view label);

    private:
        friend class ScriptBuilder;
        Fn(ScriptBuilder& owner, std::size_t index) : owner_(owner), index_(index) {}
        void emit(std::uint32_t w) { code_.push_back(w); }
        ScriptBuilder& owner_;
        std::size_t index_;
        std::vector<std::uint32_t> code_;
        std::map<std::string, std::uint32_t, std::less<>> labels_;
        std::vector<std::pair<std::size_t, std::string>> jumps_;     ///< operand at, label
        std::vector<std::pair<std::size_t, std::string>> functions_; ///< operand at, function
    };

    ScriptBuilder();
    ScriptBuilder(const ScriptBuilder&) = delete;
    ScriptBuilder& operator=(const ScriptBuilder&) = delete;

    /// A new global (one word), its symbol index; the same name again, the same one.
    int global(std::string_view name);
    /// Its first value, set as the map loads.
    void setGlobalAtLoad(int g, std::int32_t v);
    /// A new function (or the one by that name, to add to). `locals` one-word locals, the first
    /// `args` of them its arguments.
    Fn& function(std::string_view name, std::uint32_t args = 0, std::uint32_t locals = 0);
    bool has(std::string_view name) const;
    /// No functions nor globals: nothing to write (a map without a script).
    bool empty() const { return functions_.empty() && globals_.empty(); }
    /// A string constant's id (one per text).
    std::uint32_t string(std::string_view s);

    /// The script: jumps and function references resolved, each function checked (throws
    /// std::runtime_error naming what's wrong). Nothing but the two GLOBALs: an empty script.
    script::ScriptFile build() const;

private:
    struct Entry {
        std::string name;
        std::uint32_t args = 0;
        std::uint32_t locals = 0;
        std::unique_ptr<Fn> fn;
    };
    std::vector<Entry> functions_; ///< from 2 (0 and 1 are the GLOBALs)
    std::vector<std::string> globals_;
    std::vector<std::pair<int, std::int32_t>> atLoad_;
    std::vector<std::string> strings_;
};

} // namespace nox::mapgen
