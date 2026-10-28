// The NoxScript compiler: source in the NoxTools nsc dialect (NoxScript 3.0, as its documentation
// describes it) to the compiled script the game runs (script_file.hpp). The code it makes
// follows the shapes of the game's own compiled scripts, so a decompiled stock script compiles
// back to the same words.
//
// Debug info (names of functions, globals and locals, where each string and statement came
// from) is kept apart from the compiled script: the editor keeps it in its sidecar, never in the
// map.
#pragma once

#include "noxcore/script/builtins.hpp"
#include "noxcore/script/script_file.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nox::script {

class ArgValueSets; // (arg_values.hpp)

struct SourceLoc {
    std::uint32_t line = 0;   ///< 1-based
    std::uint32_t column = 0; ///< 1-based, in bytes
    bool operator==(const SourceLoc&) const = default;
};

struct Diagnostic {
    enum class Severity : std::uint8_t { Error, Warning };
    Severity severity = Severity::Error;
    SourceLoc loc;
    std::string message;
    /// "file:line:column: error: message".
    std::string format(std::string_view fileName) const;
};

/// A variable's symbol: a global (GLOBAL's symbol), a function's argument or local, or a
/// compiler temporary (a switch's, named "").
struct DebugVar {
    std::string name;
    Type type = Type::Int;
    std::uint32_t symbol = 0; ///< the symbol index (LoadVar's operand)
    std::uint32_t size = 1;   ///< words (an array's length)
    SourceLoc loc;
};

/// A statement's first code word.
struct DebugLine {
    std::uint32_t word = 0;
    SourceLoc loc;
};

struct DebugFunction {
    std::string name;
    std::uint32_t index = 0; ///< in the script's functions
    Type returns = Type::Void;
    SourceLoc loc;
    std::vector<DebugVar> symbols; ///< every symbol in order: the arguments, then the rest
    std::vector<DebugLine> lines;
};

/// One global's initialiser: GLOBAL's code words [begin, end).
struct DebugGlobalInit {
    std::uint32_t symbol = 0;
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    SourceLoc loc;
};

/// Where a string constant's literal is.
struct DebugString {
    std::uint32_t id = 0;
    SourceLoc loc;
};

struct DebugInfo {
    static constexpr int kVersion = 1;
    int version = kVersion;
    std::vector<DebugVar> globals; ///< GLOBAL's symbols, SELF, OTHER, TRUE, FALSE first
    /// Function 0's stored symbols, the object variables (`object.name`): `symbol` is the index
    /// LoadVar negates (from 1; the loader's size-0 placeholder 0 isn't listed). The compiler's
    /// pad (unless //@raw_object_vars) is symbol 1, named "".
    std::vector<DebugVar> objectVars;
    std::vector<DebugFunction> functions; ///< the script's functions from 2 on
    std::vector<DebugGlobalInit> globalInits;
    std::vector<DebugString> strings;
    bool embeddedSource = false; ///< string 0 holds the source (every literal's id one up)

    const DebugFunction* function(std::string_view name) const;
    /// JSON, one object (for the editor's sidecar).
    std::string toJson() const;
    /// Nothing for text that isn't such an object.
    static std::optional<DebugInfo> fromJson(std::string_view text);
};

struct CompileOptions {
    /// Keep the source in string 0, after kEmbeddedSourcePrefix, as nsc does.
    bool embedSource = false;
    /// Lint warnings (the editor's and the CLI's --lint; they never change the code):
    /// - a function that returns a value given to a timer or SetDialog, or named as an entry
    ///   point (MapInitialize...): the game calls those without taking the value, which leaves
    ///   the stack non-empty and puts every later handler off (OB-204);
    /// - a built string (a concatenation, IntToString, FloatToString) stored in a global: the
    ///   game drops such strings at the end of the next handler and gives their ids out again,
    ///   so the global later reads another string (OB-201); the same for a local given a built
    ///   string that can be read before the call sets it (locals keep their values between calls);
    /// - a timer whose function doesn't take what it gives: *WithArg naming a function with no
    ///   parameter (the argument is lost), a plain timer one with parameters (they read 0 and the
    ///   stack);
    /// - globals, locals and object variables never read;
    /// - functions never called or named (only with `externalNames`, the map's handler names,
    ///   since a handler is called by the name the map gives it);
    /// - an object variable used (directly or through the functions called) by a timer's
    ///   function or an entry point: those run with no object (self).
    /// (Object variables themselves are always warned once: GAME.EXE crashes on them.)
    bool lint = false;
    /// The function names the map's objects, polygons and so on name as handlers (for the
    /// unused-function lint; null: that lint is off).
    const std::vector<std::string>* externalNames = nullptr;
    /// The builtins' string parameters' value sets (arg_values.hpp), for the lint: a string
    /// literal given to such a parameter that isn't one of its values (a misspelt effect, an
    /// unknown thing type, a name the map doesn't have...) is warned, with the closest values.
    /// Only literals; a set that isn't there (null, or no game data for it) isn't checked. The
    /// map's sets aren't checked in a prefab's functions (their names get the prefab's prefix).
    const ArgValueSets* argValues = nullptr;
    /// Object variables (`int object.hp;`, `object.hp`): a NoX Core-only feature (GAME.EXE never
    /// gives objects their storage and crashes on a script that uses one: OB-210). Off, every
    /// declaration and use is an error saying how to turn them on (the map editor's Editor
    /// Settings, or the script compiler's --allow-object-vars); on, they compile with one warning that the
    /// original game crashes on them. The decompiler's own check compiles with them on.
    bool allowObjectVariables = false;
};

/// How to turn object variables on, as the compiler's error says it.
constexpr std::string_view kObjectVariablesHowToEnable =
    "in the map editor, Editor Settings > \"Allow NoX Core-only script features (object variables)\"; with noxscript, --allow-object-vars";

/// The prefixes of the functions the game calls by name at a map's start and end
/// (ScriptHost::runEntryPoints): MapInitialize, MapEntry, MapExit, MapShutdown, PlayerDeath.
bool isEntryPointName(std::string_view name);
/// The prefix of the functions the headless test runner calls (server/script_tests.hpp). Only
/// these (void, no arguments) may use `Wait(frames);`, which ends the call with a FrameTimer back
/// to the test and resumes after the Wait (plain stock code; functions without one are unchanged).
constexpr std::string_view kTestPrefix = "Test";
/// The text an assert that fails prints (with PrintToAll), before "line N: message".
constexpr std::string_view kAssertPrefix = "ASSERT ";

struct CompileResult {
    std::optional<ScriptFile> file; ///< nothing on an error
    DebugInfo debug;
    std::vector<Diagnostic> diagnostics;
    /// The source has object variables and CompileOptions::allowObjectVariables was off (each
    /// declaration and use is an error).
    bool objectVariablesRefused = false;

    bool ok() const { return file.has_value(); }
    /// Every diagnostic, one a line.
    std::string report(std::string_view fileName) const;
};

/// nsc's marker in front of the source it keeps in string 0.
constexpr std::string_view kEmbeddedSourcePrefix = "NOXSCRIPT3.0";

CompileResult compile(std::string_view source, const CompileOptions& options = {});

/// The source kept in string 0 (nsc's habit, or ours with embedSource), if there is one.
std::optional<std::string> embeddedSource(const ScriptFile& file);

/// Whether a compiled script has object variables: function 0 lays out symbols past the loader's
/// placeholder, or code reads or writes one (a LoadVar of a negative local). Such a script needs
/// a NoX Core server: the original game crashes when it runs (OB-210).
bool usesObjectVariables(const ScriptFile& file);

} // namespace nox::script
